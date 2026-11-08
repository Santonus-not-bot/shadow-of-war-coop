// coop: the first co-op steps between two test copies on one PC.
//
// The sending copy (coop_send) reads its player (the chain the probe's "player" command
// uses: [[GameClient]+0x888], position +0xAC, rotation +0xB8, health on [player+0x26A8])
// and sends it as a 60-byte UDP packet to 127.0.0.1 with a microsecond timestamp. With
// sim_hook it sends once per game frame from the simulation thread; until then (and
// without sim_hook) a timer thread sends coop_rate_hz times a second. Nothing in the
// sending game is written. The receiving copy (coop_listen) keeps the recent packets and
// either shows the latest position with the game's own "last known position" effect
// (coop_show=lkp, at most every show_every_ms and only after a show_min_move move), or
// moves a puppet (coop_show=puppet): every frame, on the simulation thread, the nearest
// character is moved to where the sender was coop_delay_ms ago, interpolated between the
// two packets around that moment: teleported (coop_puppet_move=teleport), or walked there
// with its own locomotion through the game's move request, walking or running by the
// sender's speed and teleported only when it falls far behind (coop_puppet_move=walk).
// coop_puppet_ai=off skips the puppet's behavior tree so its AI no longer moves it.
// Loopback only.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "coop.h"
#include "coop_rules.h"
#include "enemy_proto.h"
#include "input.h"
#include "net_proto.h"
#include "probe.h"
#include "simhook.h"
#include "steamguard.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

#pragma pack(push, 1)
struct Packet {
  char magic[4];  // "SOWC"
  uint16_t version;
  uint16_t flags;  // bit 0: the sender has a player in the world
  uint32_t seq;
  uint8_t move_mode;  // the sender's locomotion mode (player+0xEE0; kModeAir: in the air)
  uint8_t on_ground;  // the sender's on-ground flag (player+0x112B; 0 in the air and on walls)
  uint8_t reserved[2];
  uint64_t time_us;  // sender's clock (QueryPerformanceCounter), microseconds
  float pos[3];
  float rot[4];
  float health, max_health;
  uint32_t input_mask;  // which of inputs[] the sender's player had set (bit i: inputs[i])
  float inputs[kAnimInputs];  // the sender's animation inputs, last frame (simhook.h)
  float root_speed;  // the sender's root-motion speed last frame, units/s (horizontal)
  uint32_t action_count;  // the sender's non-zero game actions last frame (buttons; simhook.h)
  SimAction actions[kMaxActions];
  uint32_t attack_id;     // the attack the sender's player is in (its name hash; 0: none)
  uint32_t attack_stamp;  // when it started (a millisecond clock), so a repeated strike is new
};
#pragma pack(pop)
static_assert(sizeof(SimAction) == 8, "action layout");
static_assert(sizeof(Packet) == 80 + 4 * kAnimInputs + 8 * kMaxActions, "packet layout");
constexpr uint16_t kVersion = 8;
// The player's locomotion mode, a byte at player+0xEE0: 1 walking or standing, 6 then 3 in a
// free-run, 2 from a jump's takeoff through the fall until it lands (session an; the
// on-ground flags +0x112B and +0x1130 drop ~0.1 s after takeoff).
constexpr uintptr_t kMoveMode = 0xEE0;
constexpr uint8_t kModeAir = 2;
// The on-ground flag, a byte at player+0x112B: 1 on the ground, 0 from ~0.1 s after a jump's
// takeoff and all through a climb (the mode reads 2, 3, 4 and 1 on a wall; session ao).
constexpr uintptr_t kOnGround = 0x112B;

CoopConfig g_cfg;
bool g_started = false;

uint64_t NowUs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (uint64_t)(c.QuadPart / freq.QuadPart * 1000000 + c.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

// Receiver state.
CRITICAL_SECTION g_lock;
Packet g_last{};
ULONGLONG g_last_at = 0, g_first_at = 0, g_last_show_at = 0;
unsigned g_packets = 0, g_lost = 0, g_bad = 0;
bool g_have = false, g_shown_any = false;
float g_shown_pos[3] = {};
std::string g_show_err;

// Recent packets with a player, oldest first, for the puppet's interpolation.
struct Sample {
  uint64_t t_us;  // sender's clock
  float pos[3];
  float rot[4];
  uint32_t input_mask;
  float inputs[kAnimInputs];
  float root_speed;
  uint32_t action_count;
  SimAction actions[kMaxActions];
  uint8_t move_mode, on_ground;
  uint32_t attack_id, attack_stamp;
};
constexpr size_t kRing = 128;
Sample g_ring[kRing];
size_t g_ring_count = 0, g_ring_next = 0;
int64_t g_offset_us = 0;  // receiver clock minus sender clock, the smallest seen lately
int64_t g_offset_window_min = INT64_MAX;
uint64_t g_offset_window_start = 0;
bool g_have_offset = false;

// Sender state.
SOCKET g_send_socket = INVALID_SOCKET;
sownet::Endpoint* g_net = nullptr;  // the host/client transport (coop_role), below
volatile LONG g_net_recv_errors = 0;  // recvfrom errors other than "nothing there" (ICMP resets of sends to a gone peer)
CRITICAL_SECTION g_net_lock;
volatile LONG g_send_seq = 0;
volatile LONG g_was_present = -1;

// This copy's player, read through structured exception handling (it may be torn down
// while the game loads a world).
bool ReadPlayer(Packet* p) {
  const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  __try {
    const uint8_t* gc = *reinterpret_cast<const uint8_t* const*>(base + g_cfg.gameclient_rva);
    const uint8_t* player = gc ? *reinterpret_cast<const uint8_t* const*>(gc + 0x888) : nullptr;
    if (!player) return false;
    memcpy(p->pos, player + 0xAC, sizeof p->pos);
    memcpy(p->rot, player + 0xB8, sizeof p->rot);
    p->move_mode = player[kMoveMode];
    p->on_ground = player[kOnGround];
    const uint8_t* ch = *reinterpret_cast<const uint8_t* const*>(player + 0x26A8);
    p->max_health = ch ? *reinterpret_cast<const float*>(ch + 0x428) : 0.0f;
    p->health = ch ? *reinterpret_cast<const float*>(ch + 0x42C) : 0.0f;
    // The attack the player is in: StartAttack (2E297C) keeps its record at health+0xE0 (0 once
    // it ends) and the start time at +0x470; an attack record has type 0xA15 at +0 and its name
    // hash at +4, the same in every copy (session aq).
    const uint8_t* atk = ch ? *reinterpret_cast<const uint8_t* const*>(ch + 0xE0) : nullptr;
    p->attack_id = atk && *reinterpret_cast<const uint32_t*>(atk) == 0xA15 ? *reinterpret_cast<const uint32_t*>(atk + 4) : 0;
    p->attack_stamp = p->attack_id ? *reinterpret_cast<const uint32_t*>(ch + 0x470) : 0;
    // The motion the player's animation produced last frame (local space) over its frame time.
    const float* d = reinterpret_cast<const float*>(player + 0x11D8);
    const float dt = *reinterpret_cast<const float*>(player + 0x1118);
    p->root_speed = dt > 0.0f && dt < 0.5f ? std::sqrt(d[0] * d[0] + d[2] * d[2]) / dt : 0.0f;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

sockaddr_in Loopback(unsigned port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((u_short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

void SendSample() {
  Packet p{};
  memcpy(p.magic, "SOWC", 4);
  p.version = kVersion;
  // A paused game reads position 0,0,0 and an identity rotation (seen 2026-10-21): no player then.
  const bool present = ReadPlayer(&p) && (p.pos[0] != 0.0f || p.pos[1] != 0.0f || p.pos[2] != 0.0f);
  p.flags = present ? 1 : 0;
  p.seq = (uint32_t)InterlockedIncrement(&g_send_seq);
  p.time_us = NowUs();
  if (present && !SimAnimInputs(p.inputs, &p.input_mask)) p.input_mask = 0;
  if (present) p.action_count = (uint32_t)SimPlayerActions(g_cfg.gameclient_rva, p.actions, kMaxActions);
  if (g_net) {
    // Over the transport: at coop_state_hz; the newest state replaces one not yet sent.
    static ULONGLONG last_sent_ms = 0;
    const ULONGLONG now_ms = NowUs() / 1000;
    if (now_ms - last_sent_ms < 1000u / std::max(1u, g_cfg.state_hz)) return;
    // The net thread holds this lock while it works on a datagram, and that can include waiting for this very
    // thread (a spawn it asked for); so this thread never waits for it, it skips the send (the next one replaces it).
    if (!TryEnterCriticalSection(&g_net_lock)) return;
    last_sent_ms = now_ms;
    g_net->sendState(reinterpret_cast<const uint8_t*>(&p), sizeof p);
    LeaveCriticalSection(&g_net_lock);
    return;
  }
  const sockaddr_in to = Loopback(g_cfg.send_port);
  if (sendto(g_send_socket, reinterpret_cast<const char*>(&p), sizeof p, 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to) != (int)sizeof p &&
      p.seq < 5)
    SowLog(L"coop: sendto failed (%d)", WSAGetLastError());
  if (InterlockedExchange(&g_was_present, present ? 1 : 0) != (present ? 1 : 0))
    SowLog(L"coop: sending %s (packet %u)", present ? L"the player" : L"no player (not in the world)", p.seq);
}

// Until the simulation thread sends once per frame (sim_hook and the world started), a
// high-resolution timer paces the packets.
DWORD WINAPI SendThread(void*) {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const LONGLONG period_100ns = 10000000LL / std::max(1u, std::min(g_cfg.rate_hz, 240u));
  bool logged_frames = false;
  for (;;) {
    if (SimHookInstalled() && InputGameThread()) {
      if (!logged_frames) SowLog(L"coop: sending once per game frame from the simulation thread");
      logged_frames = true;
      Sleep(100);
      continue;
    }
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -period_100ns;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 1000);
    } else {
      Sleep((DWORD)(period_100ns / 10000));
    }
    SendSample();
  }
}

ULONGLONG g_last_pick = 0;
bool g_puppet_spawn_due = false;  // the net thread wants a stand-in spawned; the simulation thread does it
float g_puppet_spawn_at[3] = {};
unsigned g_puppet_spawn_tries = 0;

// coop_show=puppet: pick the nearest character once the world has started (a heap scan,
// so on this thread, retried every 10 s). The moves happen in CoopFrame.
void PickPuppet() {
  const ULONGLONG now = GetTickCount64();
  if (SimHasPuppet() || !InputGameThread() || now - g_last_pick < 10000) return;
  g_last_pick = now;
  std::string r;
  if (!g_cfg.spawn_type.empty()) {
    // At the sender's position, or after a failed try a little beside it: the game creates
    // nothing where something already stands (seen with this copy's own player there). The
    // puppet is moved to the sender right after anyway.
    static const float kAround[5][2] = {{0, 0}, {150, 0}, {-150, 0}, {0, 150}, {0, -150}};
    static unsigned tries = 0;
    float pos[3];
    EnterCriticalSection(&g_lock);
    memcpy(pos, g_last.pos, sizeof pos);
    LeaveCriticalSection(&g_lock);
    pos[0] += kAround[tries % 5][0];
    pos[2] += kAround[tries % 5][1];
    // This runs on the net thread, inside the transport's lock; a spawn waits for the simulation thread, which may
    // be paused (a death screen, a load): the transport would stall with it (the client's save then never arrived,
    // session bn). The simulation thread makes the spawn (CoopFrame).
    EnterCriticalSection(&g_lock);
    memcpy(g_puppet_spawn_at, pos, sizeof g_puppet_spawn_at);
    g_puppet_spawn_due = true;
    LeaveCriticalSection(&g_lock);
    ++tries;
    return;
  }
  const bool ok = SimPickNearestPuppet(g_cfg.gameclient_rva, &r);
  SowLog(L"coop: %S%S", ok ? "" : "no puppet yet: ", r.c_str());
}

void MaybeShow(const Packet& p) {
  if (g_cfg.puppet && (p.flags & 1)) {
    PickPuppet();
    return;
  }
  if (!g_cfg.show || !(p.flags & 1)) return;
  const ULONGLONG now = GetTickCount64();
  float moved = 1e9f;
  EnterCriticalSection(&g_lock);
  if (g_shown_any) {
    float dx = p.pos[0] - g_shown_pos[0], dy = p.pos[1] - g_shown_pos[1], dz = p.pos[2] - g_shown_pos[2];
    moved = std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  const bool due = now - g_last_show_at >= g_cfg.show_every_ms && moved >= g_cfg.show_min_move;
  LeaveCriticalSection(&g_lock);
  if (!due) return;
  std::string err;
  const bool ok = SimRequestLkp(g_cfg.lkp_rva, g_cfg.gameclient_rva, p.pos, &err);
  EnterCriticalSection(&g_lock);
  if (ok) {
    memcpy(g_shown_pos, p.pos, sizeof g_shown_pos);
    g_shown_any = true;
    g_last_show_at = now;
    g_show_err.clear();
  } else if (err != g_show_err) {
    g_show_err = err;
    SowLog(L"coop: cannot show the remote player yet: %S", err.c_str());
  }
  LeaveCriticalSection(&g_lock);
}

// Called with g_lock held, for every good packet.
void Remember(const Packet& p, uint64_t recv_us) {
  // Clock offset: the smallest receive-minus-send difference is the one with the least
  // delay; it is re-measured every 10 s so a drifting clock is followed.
  const int64_t d = (int64_t)(recv_us - p.time_us);
  if (recv_us - g_offset_window_start > 10000000) {
    if (g_offset_window_min != INT64_MAX) g_offset_us = g_offset_window_min;
    g_offset_window_min = INT64_MAX;
    g_offset_window_start = recv_us;
  }
  g_offset_window_min = std::min(g_offset_window_min, d);
  if (!g_have_offset || d < g_offset_us) g_offset_us = d;
  g_have_offset = true;
  if (!(p.flags & 1)) return;
  if (g_ring_count && p.time_us <= g_ring[(g_ring_next + kRing - 1) % kRing].t_us) return;  // late or repeated
  Sample& s = g_ring[g_ring_next];
  s.t_us = p.time_us;
  memcpy(s.pos, p.pos, sizeof s.pos);
  memcpy(s.rot, p.rot, sizeof s.rot);
  s.input_mask = p.input_mask;
  memcpy(s.inputs, p.inputs, sizeof s.inputs);
  s.root_speed = p.root_speed;
  s.move_mode = p.move_mode;
  s.on_ground = p.on_ground;
  s.attack_id = p.attack_id;
  s.attack_stamp = p.attack_stamp;
  s.action_count = std::min<uint32_t>(p.action_count, kMaxActions);
  memcpy(s.actions, p.actions, s.action_count * sizeof(SimAction));
  g_ring_next = (g_ring_next + 1) % kRing;
  g_ring_count = std::min(g_ring_count + 1, kRing);
}

// Where the sender was at sender time t: between the two samples around t (position
// linear, rotation normalized-linear on the shorter arc), or the nearest end. g_lock held.
bool Interpolate(uint64_t t, float pos[3], float rot[4]) {
  if (!g_ring_count) return false;
  auto at = [](size_t i) -> const Sample& { return g_ring[(g_ring_next + kRing - g_ring_count + i) % kRing]; };
  size_t hi = 0;
  while (hi < g_ring_count && at(hi).t_us <= t) ++hi;
  if (hi == 0 || hi == g_ring_count) {
    const Sample& s = at(hi == 0 ? 0 : g_ring_count - 1);
    memcpy(pos, s.pos, sizeof s.pos);
    memcpy(rot, s.rot, sizeof s.rot);
    return true;
  }
  const Sample &a = at(hi - 1), &b = at(hi);
  const float u = (float)(t - a.t_us) / (float)(b.t_us - a.t_us);
  for (int i = 0; i < 3; ++i) pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * u;
  const float dot = a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3];
  const float sign = dot < 0 ? -1.0f : 1.0f;
  float len = 0;
  for (int i = 0; i < 4; ++i) {
    rot[i] = a.rot[i] + (sign * b.rot[i] - a.rot[i]) * u;
    len += rot[i] * rot[i];
  }
  len = std::sqrt(len);
  if (len > 1e-6f)
    for (int i = 0; i < 4; ++i) rot[i] /= len;
  return true;
}

// The sender's animation inputs, root-motion speed and game actions at sender time t: those
// of the last sample at or before t (not blended: several are angles). g_lock held.
uint32_t InputsAt(uint64_t t, float inputs[kAnimInputs], float* root_speed, SimAction actions[kMaxActions],
                  int* action_count) {
  if (!g_ring_count) return 0;
  auto at = [](size_t i) -> const Sample& { return g_ring[(g_ring_next + kRing - g_ring_count + i) % kRing]; };
  size_t i = 0;
  while (i + 1 < g_ring_count && at(i + 1).t_us <= t) ++i;
  memcpy(inputs, at(i).inputs, sizeof at(i).inputs);
  *root_speed = at(i).root_speed;
  *action_count = (int)at(i).action_count;
  memcpy(actions, at(i).actions, at(i).action_count * sizeof(SimAction));
  return at(i).input_mask;
}

// Whether the sender was off the ground at sender time t (the sample at or before t): in the
// air (mode 2) or on a wall (on-ground flag 0).
bool OffGroundAt(uint64_t t) {
  if (!g_ring_count) return false;
  auto at = [](size_t i) -> const Sample& { return g_ring[(g_ring_next + kRing - g_ring_count + i) % kRing]; };
  size_t i = 0;
  while (i + 1 < g_ring_count && at(i + 1).t_us <= t) ++i;
  return at(i).move_mode == kModeAir || !at(i).on_ground;
}

// The sender's attack at sender time t (the sample at or before t). g_lock held.
void AttackAt(uint64_t t, uint32_t* id, uint32_t* stamp) {
  *id = *stamp = 0;
  if (!g_ring_count) return;
  auto at = [](size_t i) -> const Sample& { return g_ring[(g_ring_next + kRing - g_ring_count + i) % kRing]; };
  size_t i = 0;
  while (i + 1 < g_ring_count && at(i + 1).t_us <= t) ++i;
  *id = at(i).attack_id;
  *stamp = at(i).attack_stamp;
}

std::string g_puppet_err;
volatile LONG g_frames_logged = 0;
volatile LONG g_follow = 1;
bool g_ai_requested = false;
bool g_inputs_requested = false;
volatile LONG g_inputs_given = 0;  // frames the puppet got the sender's animation inputs
volatile LONG g_inputs_on = 1;     // coop_puppet_inputs, changeable by the probe ("puppet inputs")
volatile LONG g_speed_on = 1;      // coop_anim_speed, changeable by the probe ("puppet speed")
volatile LONG g_actions_on = 1;    // coop_puppet_actions, changeable by the probe ("puppet actions")
volatile LONG g_follower_on = 1;   // coop_puppet_follower, changeable by the probe ("puppet follower")
volatile LONG g_targetable_on = 0;  // coop_puppet_targetable, changeable by the probe ("puppet targetable")
volatile LONG g_air_on = 1;         // coop_air_follow, changeable by the probe ("puppet air")
volatile LONG g_attacks_on = 1;     // coop_puppet_attacks, changeable by the probe ("puppet attacks")
volatile LONG g_resets_on = 0;      // put-backs restart the puppet's animation (probe "puppet reset")
bool g_reset_requested = false;
std::string g_attack_err;
bool g_attack_logged = false;
volatile LONG g_air_frames = 0;     // frames anim mode moved the puppet to the sender in the air
volatile LONG g_air_landings = 0;   // put-backs when the sender landed
bool g_was_air = false;
bool g_target_requested = false;
std::string g_actions_err;
std::string g_follower_err;
std::string g_inputs_err;
// Walk mode: the last request (re-sent when the target moved, the gait changed, or every
// 100 ms) and counters for the probe.
float g_req_dest[3] = {};
int g_req_type = -1;
ULONGLONG g_req_at = 0;
unsigned g_walk_teleports = 0;
std::string g_walk_err;
// Walk mode, one frame: pos is where the sender was, prev 200 ms before that. pinned
// ("both"): the puppet is teleported to pos every frame anyway, and the requests only make
// its locomotion play the gait; they aim half a second ahead along the sender's motion.
void WalkFrame(const float pos[3], const float rot[4], const float prev[3], bool pinned) {
  float at[3];
  if (!SimPuppetPos(at)) return;
  const float vx = (pos[0] - prev[0]) / 0.2f, vz = (pos[2] - prev[2]) / 0.2f;
  const float speed = std::sqrt(vx * vx + vz * vz);
  const float ex = pos[0] - at[0], ez = pos[2] - at[2];
  const float behind = std::sqrt(ex * ex + ez * ez), dy = std::fabs(pos[1] - at[1]);
  std::string err;
  if (pinned && !SimRequestPuppetMove(g_cfg.teleport_rva, pos, rot, &err)) {
    // reported below
  } else if (pinned && speed > 40.0f) {
    const float dest[3] = {pos[0] + vx * 0.5f, pos[1], pos[2] + vz * 0.5f};
    const int type = speed > g_cfg.run_speed ? 2 : 1;
    const ULONGLONG now = GetTickCount64();
    if (type != g_req_type || now - g_req_at >= 100) {
      if (SimPuppetWalk(g_cfg.move_request_rva, dest, type, 1.0f, &err)) {
        memcpy(g_req_dest, dest, sizeof g_req_dest);
        g_req_type = type;
        g_req_at = now;
      }
    }
  } else if (pinned) {
    if (g_req_type != 0 && SimPuppetWalk(g_cfg.move_request_rva, pos, 0, 1.0f, &err)) g_req_type = 0;
  } else if (behind > g_cfg.teleport_dist || dy > 300.0f) {
    if (SimRequestPuppetMove(g_cfg.teleport_rva, pos, rot, &err)) {
      ++g_walk_teleports;
      if (g_walk_teleports <= 5 || g_walk_teleports % 50 == 0)
        SowLog(L"coop: puppet %.0f units behind (%.0f up/down); teleported (#%u)", behind, dy, g_walk_teleports);
    }
    g_req_type = -1;
  } else if (speed > 40.0f || behind > 120.0f) {
    // Aim a little ahead along the sender's motion, so the walk does not slow down to
    // arrive at every target.
    const float dest[3] = {pos[0] + vx * 0.3f, pos[1], pos[2] + vz * 0.3f};
    const int type = speed > g_cfg.run_speed || behind > 300.0f ? 2 : 1;
    const float mx = dest[0] - g_req_dest[0], mz = dest[2] - g_req_dest[2];
    const ULONGLONG now = GetTickCount64();
    if (type != g_req_type || mx * mx + mz * mz > 40.0f * 40.0f || now - g_req_at >= 100) {
      if (SimPuppetWalk(g_cfg.move_request_rva, dest, type, 1.0f, &err)) {
        memcpy(g_req_dest, dest, sizeof g_req_dest);
        g_req_type = type;
        g_req_at = now;
      }
    }
  } else if (g_req_type != 0) {
    if (SimPuppetWalk(g_cfg.move_request_rva, pos, 0, 1.0f, &err)) g_req_type = 0;
  }
  if (err != g_walk_err && !err.empty()) SowLog(L"coop: cannot walk the puppet: %S", err.c_str());
  g_walk_err = err;
}

// Anim mode, one frame: the sender's inputs (set just before) move the puppet by its own
// animation, so it strides like the sender; pinning it every frame instead holds it in a
// stretched pose, and teleports every few frames make it slide with its legs together
// (seen 2026-10-22). Drift is put right with a teleport: when it is more than anim_snap off
// (or 150 up or down), and once the sender has stood still for half a second and the puppet
// is more than 15 units or 15 degrees off. Standing is slower than 10 units/s, or slower than
// 40 with the stick let go: after a stop the sender creeps at 10-20 units/s for a second or
// more, which kept the put-back waiting while the puppet stood 100+ units off. (Moving it in
// place a little every frame instead, by writing its position, made the drift worse.)
// After some strikes a put-back does not hold: the puppet reaches the sender's spot and the next
// frame its own movement has it back where it was, ~22 units off, so the put-backs flipped it
// between the two spots every other frame (session bd). Once a standing put-back has landed
// (the puppet within 5 units) and the puppet is back within 8 units of where it started inside
// half a second, standing put-backs stop until the sender moves again.
ULONGLONG g_sender_moving_at = 0;
unsigned g_anim_snaps = 0;
volatile float g_anim_snap = 150;  // coop_anim_snap, changeable by the probe ("puppet snap")
ULONGLONG g_putback_at = 0;        // the last put-back, where the puppet was then, whether it landed
float g_putback_from[3] = {};
bool g_putback_landed = false, g_putback_stuck = false;
// What anim mode last saw (probe "remote"): frames it ran, frames it could not read the
// puppet, put-backs refused, frames a standing put-back was held back as not holding, and the
// last frame's gap, turn and sender speed.
volatile LONG g_anim_frames = 0, g_anim_no_xform = 0, g_anim_refused = 0, g_frames_not_ok = 0, g_anim_stuck = 0;
float g_anim_off = 0, g_anim_turn = 0, g_anim_sender_speed = 0;

// In some places the game does not move the stand-in at all (session cm: the host's stand-in of the client east of the test arena,
// x 25,000-26,500; also after a respawn): the queued teleport never lands and its animation does not carry it, so it was put back
// every frame and stood 500-1,600 units off. A direct write of its position does hold there. After kNoLandFrames way-off put-backs
// in a row that left it where it was, it is frozen: moved by direct writes (whenever 15 units off) until it is seen moving by
// itself again. (Direct writes every frame are not used otherwise: arrows stop hitting a character written that way.)
constexpr int kNoLandFrames = cooprules::kNoLandFrames;
int g_no_land = 0;
bool g_puppet_frozen = false;
float g_frozen_at[3] = {};
volatile LONG g_frozen_writes = 0, g_frozen_spells = 0;

bool FrozenPutBack(const float at[3], const float pos[3], const float rot[4], bool way_off, ULONGLONG now) {
  std::string err;
  if (g_puppet_frozen) {
    const float fx = at[0] - g_frozen_at[0], fy = at[1] - g_frozen_at[1], fz = at[2] - g_frozen_at[2];
    if (fx * fx + fy * fy + fz * fz > 4.0f) {  // it moved without a write of ours: the game moves it again
      g_puppet_frozen = false;
      g_no_land = 0;
      SowLog(L"coop: the stand-in moves by itself again; usual put-backs");
      return false;
    }
    const float ex = pos[0] - at[0], ez = pos[2] - at[2];
    if (ex * ex + ez * ez > 225.0f || std::fabs(pos[1] - at[1]) > 50.0f) {
      if (SimRequestPuppetMove(g_cfg.teleport_rva, pos, rot, &err, true)) {
        memcpy(g_frozen_at, pos, sizeof g_frozen_at);
        InterlockedIncrement(&g_frozen_writes);
      }
    }
    return true;
  }
  if (!way_off) {
    g_no_land = 0;
    return false;
  }
  const float mx = at[0] - g_putback_from[0], mz = at[2] - g_putback_from[2];
  g_no_land = cooprules::NoLandStep(g_no_land, true, now, g_putback_at, mx * mx + mz * mz);
  if (g_no_land < kNoLandFrames) return false;
  if (!SimRequestPuppetMove(g_cfg.teleport_rva, pos, rot, &err, true)) return false;
  g_puppet_frozen = true;
  memcpy(g_frozen_at, pos, sizeof g_frozen_at);
  InterlockedIncrement(&g_frozen_writes);
  if (InterlockedIncrement(&g_frozen_spells) <= 20)
    SowLog(L"coop: the stand-in did not move after %d put-backs (stuck at %.0f,%.0f,%.0f); moved by direct writes until it moves by itself",
           g_no_land, at[0], at[1], at[2]);
  return true;
}

void AnimFrame(const float pos[3], const float rot[4], const float prev[3], bool stick_released) {
  float at[3], at_rot[4];
  if (!SimPuppetXform(at, at_rot)) {
    InterlockedIncrement(&g_anim_no_xform);
    return;
  }
  InterlockedIncrement(&g_anim_frames);
  const float ex = pos[0] - at[0], ez = pos[2] - at[2], dy = std::fabs(pos[1] - at[1]);
  const float off = std::sqrt(ex * ex + ez * ez);
  const float vx = (pos[0] - prev[0]) / 0.2f, vz = (pos[2] - prev[2]) / 0.2f;
  const ULONGLONG now = GetTickCount64();
  const float standing = stick_released ? 40.0f : 10.0f;
  if (vx * vx + vz * vz > standing * standing) {
    g_sender_moving_at = now;
    g_putback_stuck = false;
  }
  const float dot = std::fabs(rot[0] * at_rot[0] + rot[1] * at_rot[1] + rot[2] * at_rot[2] + rot[3] * at_rot[3]);
  const float turn_deg = 2.0f * std::acos(std::min(dot, 1.0f)) * 57.29578f;
  g_anim_off = off;
  g_anim_turn = turn_deg;
  g_anim_sender_speed = std::sqrt(vx * vx + vz * vz);
  if (off < 5.0f) g_putback_landed = true;
  const bool settled = now - g_sender_moving_at > 500;
  const bool way_off = off > g_anim_snap || dy > 150.0f;
  if (!way_off && !(settled && (off > 15.0f || turn_deg > 15.0f))) return;
  if (!way_off) {
    const float bx = at[0] - g_putback_from[0], bz = at[2] - g_putback_from[2];
    if (g_putback_landed && now - g_putback_at < 500 && bx * bx + bz * bz < 64.0f) g_putback_stuck = true;
    if (g_putback_stuck) {
      InterlockedIncrement(&g_anim_stuck);
      return;
    }
  }
  std::string err;
  if (FrozenPutBack(at, pos, rot, way_off, now)) return;
  if (SimRequestPuppetMove(g_cfg.teleport_rva, pos, rot, &err)) {
    g_putback_at = now;
    memcpy(g_putback_from, at, sizeof g_putback_from);
    g_putback_landed = false;
    ++g_anim_snaps;
    if (g_anim_snaps <= 5 || g_anim_snaps % 50 == 0)
      SowLog(L"coop: puppet %.0f units and %.0f degrees off (%.0f up/down)%s; put back (#%u)", off, turn_deg, dy,
             settled ? L", sender standing" : L"", g_anim_snaps);
  } else {
    InterlockedIncrement(&g_anim_refused);
    if (err != g_walk_err) {
      SowLog(L"coop: cannot move the puppet: %S", err.c_str());
      g_walk_err = err;
    }
  }
}

// Anim mode's catch-up for the puppet's root-motion speed: twice the gap to where the sender
// is shown, along the puppet's own heading, per second (at most 300 units/s either way), since
// the speed it follows is smoothed and lags when the sender speeds up into a free-run. Only
// the puppet's speed changes, never its direction.
float RootCatchUp(const float pos[3]) {
  float at[3], at_rot[4];
  if (!SimPuppetXform(at, at_rot)) return 0.0f;
  const float h = 2.0f * std::atan2(at_rot[1], at_rot[3]);  // heading: forward is (sin h, cos h) in x, z
  const float along = (pos[0] - at[0]) * std::sin(h) + (pos[2] - at[2]) * std::cos(h);
  return std::min(std::max(2.0f * along, -300.0f), 300.0f);
}

// ---- enemies (STAGE2-DESIGN.md) -----------------------------------------------------------------------------
// Host: every 66 ms the enemies within coop_enemy_radius of either player are announced (NEW/GONE events) and
// streamed (a STATE2 snapshot). Client: each announced enemy is a proxy character of the same type with its AI
// off, placed, animated and kept in the host's health; its own ambient population is kept away.

sowenemy::HostTracker g_enemy_host;
sowenemy::ClientTracker g_enemy_client;
std::vector<std::vector<uint8_t>> g_enemy_events_in;  // from the net thread, taken by the frame
std::vector<uint8_t> g_enemy_snap_in;
uint64_t g_enemy_snap_ms = 0;
uint32_t g_enemy_session = 0;
volatile LONG64 g_test_dmg_owner = 0;  // probe "damage": a swing of this proxy hits this copy's player, on the next frame
volatile LONG g_test_dmg_attack = 0;
volatile LONG64 g_test_standin_owner = 0;  // probe "standinhit": a swing of this enemy at the stand-in (host)
volatile LONG g_test_standin_result = 0;
volatile LONG g_test_dmg_result = 0;   // 0 none, 1 applied, -1 failed
volatile LONG g_enemy_dmg_sent = 0, g_enemy_dmg_applied = 0, g_enemy_dmg_failed = 0, g_enemy_dmg_stale = 0, g_enemy_dmg_borrowed = 0;
volatile LONG g_enemy_hits_unmatched = 0, g_enemy_hits_sent = 0, g_enemy_hits_applied = 0, g_enemy_hits_failed = 0, g_enemy_hits_stale = 0;
volatile LONG g_enemy_sent = 0, g_enemy_announced = 0, g_enemy_spawned = 0, g_enemy_spawn_failed = 0,
              g_enemy_despawn_asked = 0, g_enemy_away = 0, g_enemy_captains = 0, g_enemy_captains_missing = 0, g_enemy_members_sent = 0,
              g_enemy_attack_ok = 0, g_enemy_attack_fail = 0, g_enemy_play_ok = 0, g_enemy_play_fail = 0, g_enemy_anims_sent = 0,
              g_enemy_anims_played = 0, g_enemy_anims_refused = 0, g_enemy_killed = 0, g_enemy_kill_failed = 0, g_enemy_local_damage = 0, g_enemy_local_sent = 0, g_enemy_local_after_strike = 0, g_enemy_local_bystander = 0, g_enemy_local_fall = 0, g_enemy_losses_applied = 0, g_enemy_losses_killed = 0, g_enemy_losses_stale = 0,
              g_enemy_hits_by_decoy = 0, g_enemy_hits_poked = 0;
extern uintptr_t g_decoy_owner;  // the host's decoy (below)
// No state from the host for this long: its world is stopped (a death screen, a load, its pause menu), so its
// enemies' pictures would stand frozen. They are taken away, and shown again where the host's next states put them.
constexpr ULONGLONG kHostAwayMs = 5000;
volatile LONG g_nemesis_sent = 0, g_nemesis_got = 0, g_nemesis_skipped = 0;
std::string g_enemy_last_error;

void QueueEnemyEvent(const uint8_t* p, size_t n) {
  const size_t limit = n > 0 && p[0] == sowenemy::kOpMemberData ? sizeof(sowenemy::MemberDataHead) + sowenemy::kMemberChunk : 200;
  if (n < 5 || n > limit) return;
  EnterCriticalSection(&g_lock);
  if (g_enemy_events_in.size() < 4096) g_enemy_events_in.emplace_back(p, p + n);
  LeaveCriticalSection(&g_lock);
}

void QueueEnemySnapshot(const uint8_t* p, size_t n) {
  if (n < sowenemy::kSnapshotHead || n > sizeof(sowenemy::Snapshot)) return;
  EnterCriticalSection(&g_lock);
  g_enemy_snap_in.assign(p, p + n);
  g_enemy_snap_ms = NowUs() / 1000;
  LeaveCriticalSection(&g_lock);
}

float DistXZ(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dz * dz);
}

float YawOf(const float q[4]) { return 2.0f * std::atan2(q[1], q[3]); }

// The host: the client's strikes on its proxies, applied to the real enemies.
void EnemyHostHits() {
  std::vector<std::vector<uint8_t>> events;
  EnterCriticalSection(&g_lock);
  events.swap(g_enemy_events_in);
  LeaveCriticalSection(&g_lock);
  int applied_now = 0;
  for (const auto& ev : events) {
    {  // health the client's proxy lost outside the hit function: exactly that, from the real enemy (session cm)
      uint32_t lid = 0;
      float amount = 0, lpos[3];
      if (sowenemy::ParseLoss(ev.data(), ev.size(), &lid, &amount, lpos)) {
        const uintptr_t owner = g_enemy_host.keyOf(lid);
        float cur = 0, top = 0;
        if (!owner || !SimHealthNow(owner, &cur, &top) || !(top > 0) || cur <= 0) {
          InterlockedIncrement(&g_enemy_losses_stale);
          continue;
        }
        const float left = cur - amount;
        if (left <= 0.5f) {  // the game's own death (health 0 written by hand left bodies the game never removed)
          if (SimKillCharacter(g_cfg.hit_rva, g_cfg.attack_table_rva, owner, g_cfg.gameclient_rva, "PC_NT_L1_D1_F_LL_RL_04"))
            InterlockedIncrement(&g_enemy_losses_killed);
          else
            SimProxyHealth(owner, 0.0f);
        } else {
          SimProxyHealth(owner, left / top);
          if (g_decoy_owner) SimPokeEnemy(g_cfg.hit_rva, g_cfg.attack_table_rva, owner, g_decoy_owner, "AI_Infantry_Melee_Front_1_RR");
        }
        if (InterlockedIncrement(&g_enemy_losses_applied) <= 12)
          SowLog(L"coop: the client's proxy of enemy %u lost %.1f there: applied here, health %.0f -> %.0f of %.0f", lid, amount, cur,
                 left > 0 ? left : 0.0f, top);
        continue;
      }
    }
    sowenemy::HitMsg m;
    if (!sowenemy::ParseHit(ev.data(), ev.size(), &m)) continue;
    const uintptr_t owner = g_enemy_host.keyOf(m.id);
    if (!owner) {
      InterlockedIncrement(&g_enemy_hits_stale);  // the enemy is gone or was never announced in this session
      continue;
    }
    if (++applied_now > 16) break;
    // An enemy turns to whoever hits it. Hit by the host's player (the only attacker that did damage, session bm) it turned to
    // the host's player, and when the hunt then pointed it back at the decoy it stood still (session cg). So the decoy (the
    // client's place) is the attacker, or the host's player hits and the decoy then pokes (no damage) to be the last attacker.
    const uintptr_t decoy = g_decoy_owner;
    const int mode = decoy ? g_cfg.hit_by : 0;
    float hp_before = 0, hp_after = 0, top = 0;
    SimHealthNow(owner, &hp_before, &top);
    bool ok = SimApplyHit(g_cfg.hit_rva, g_cfg.attack_table_rva, owner, m.pos, m.attack_id, g_cfg.gameclient_rva, mode == 1 ? decoy : 0);
    const wchar_t* how = mode == 1 ? L"the decoy" : L"this player";
    SimHealthNow(owner, &hp_after, &top);
    if (ok && mode == 1 && hp_after >= hp_before && hp_before > 0) {
      // The decoy's hit did nothing: the host's player hits, the decoy pokes.
      ok = SimApplyHit(g_cfg.hit_rva, g_cfg.attack_table_rva, owner, m.pos, m.attack_id, g_cfg.gameclient_rva, 0);
      SimHealthNow(owner, &hp_after, &top);
      how = L"this player (the decoy's did nothing), then the decoy's poke";
      if (ok && hp_after > 0) SimPokeEnemy(g_cfg.hit_rva, g_cfg.attack_table_rva, owner, decoy, "AI_Infantry_Melee_Front_1_RR");
      InterlockedIncrement(&g_enemy_hits_poked);
    } else if (ok && mode == 2) {
      if (hp_after > 0) SimPokeEnemy(g_cfg.hit_rva, g_cfg.attack_table_rva, owner, decoy, "AI_Infantry_Melee_Front_1_RR");
      how = L"this player, then the decoy's poke";
      InterlockedIncrement(&g_enemy_hits_poked);
    } else if (ok && mode == 1) {
      InterlockedIncrement(&g_enemy_hits_by_decoy);
    }
    if (ok) {
      if (InterlockedIncrement(&g_enemy_hits_applied) <= 12)
        SowLog(L"coop: the client hit enemy %u (attack %08X) at %.0f,%.0f,%.0f: applied by %s, health %.0f -> %.0f of %.0f", m.id,
               m.attack_id, m.pos[0], m.pos[1], m.pos[2], how, hp_before, hp_after, top);
    } else {
      InterlockedIncrement(&g_enemy_hits_failed);
    }
  }
}

// The decoy (the host): enemies do not attack the stand-in (a Talion is not an enemy for the AI); an allied soldier with
// no AI of its own is, so one stands where the stand-in stands, never dies, and what hits it is told to the client like
// a hit on the stand-in.
uintptr_t g_decoy_owner = 0;
ULONGLONG g_decoy_next = 0;
volatile LONG g_decoy_spawned = 0;
volatile LONG g_decoy_hurt = 0, g_decoy_hurt_amount = 0, g_decoy_hurt_sent = 0;  // health the decoy lost outside the hit function (times, total)
volatile LONG g_decoy_hide = 1;                            // probe "coopset decoyhide off": the decoy shown at full size (a test)
// Where the decoy was last put by the game's own teleport (or spawned). Writing its transform moves what is drawn, not where the
// game's navigation and physics have it: in session cj orcs 700 units from a decoy that had been moved from the tower to the test
// arena that way never reached it (0 swings in 60 s), and a decoy spawned in the arena drew 22 in 50 s.
float g_decoy_anchor[3] = {};
bool g_decoy_anchored = false;
volatile LONG g_decoy_reanchors = 0, g_decoy_remade = 0;

// Removed decoys the game has not deleted yet. Each stays in the proxy set (its behavior tree skipped) until the game has
// deleted it, as the client's freed proxies do (g_limbo). Until session cm a removed decoy left the proxy set at once: its own
// AI ran again on a character whose health had been written 0 and whose despawn was taking it apart, and the host crashed in
// the AI tick reading a missing component ([character+0x2D08] at 2FC037, [character+0x2CD8] at 342E1A): 2 crashes in 15 jump
// cycles that remade the decoy ~4 times each (tools/jump_cycles.py --arena).
std::vector<uintptr_t> g_decoy_limbo;
volatile LONG g_decoy_limbo_gone = 0, g_decoy_by_remote = 0, g_decoy_unwritten = 0;

// The host's game never deleted a removed decoy (health 0 and a despawn ask: 61 decoys in 16 jump cycles, none deleted; the
// fights then stopped). A kill the game's way did not take on it (session cm: probe kill with the host's player, the decoy itself
// or an orc as the attacker: "not killed"), so it stays in limbo with its AI off. 56: the proxy set has 64 slots and on the host
// only decoys use it; a remake comes with a long jump of the client (joining, a respawn), a few per session.
volatile LONG g_decoy_killed = 0;  // kept in the status line (always 0 since the kill was dropped)

void DecoyRetire(uintptr_t owner) {
  SimProxyHealth(owner, 0.0f);
  SimDespawnNow(g_cfg.despawn_rva, owner);
  if (g_cfg.decoy_limbo && g_decoy_limbo.size() < 56) g_decoy_limbo.push_back(owner);
  else SimProxyDrop(owner);
}

void DecoyLimboFrame() {
  for (size_t i = 0; i < g_decoy_limbo.size();) {
    if (!SimProxyValid(g_decoy_limbo[i])) {
      SimProxyDrop(g_decoy_limbo[i]);
      g_decoy_limbo.erase(g_decoy_limbo.begin() + i);
      InterlockedIncrement(&g_decoy_limbo_gone);
    } else {
      ++i;
    }
  }
}

void DecoyRemove() {
  if (!g_decoy_owner) return;
  SimSetDecoy(0);
  SimRetargetDropTarget(g_decoy_owner);  // no redirect may write the removed decoy into an enemy's AI
  DecoyRetire(g_decoy_owner);
  g_decoy_owner = 0;
}

bool ReadFloatsAt(uintptr_t addr, float out[3]);  // below

volatile LONG g_standin_hurt = 0, g_standin_hurt_sent = 0;

// Health `who` (the decoy or the stand-in) lost outside the hit function is an arrow (session cm): sent to the client as a swing
// of the nearest announced enemy after the decoy, an archer first, with AI_Archer_Shoot for an archer (its "current attack" can be
// a taunt, or 0 between two shots) and that enemy's current attack otherwise. False when nothing could be sent.
bool SendArrowHit(uintptr_t who) {
  if (!g_cfg.decoy_hurt_send || !g_net || !g_decoy_owner) return false;
  uintptr_t list[256];
  const size_t n = SimEnemyList(list, 256);
  float dpos[3];
  if (!ReadFloatsAt(who + 0xAC, dpos)) return false;
  uint32_t best_id = 0, best_attack = 0;
  float best = 1e30f;
  bool best_archer = false;
  for (size_t i = 0; i < n; ++i) {
    SimEnemyInfo e{};
    if (SimEnemyTarget(list[i]) != g_decoy_owner || !SimEnemyRead(list[i], &e)) continue;
    const uint32_t id = g_enemy_host.idOf(list[i]);
    if (!id) continue;
    const bool archer = strstr(e.type, "Archer") != nullptr;
    const float d = DistXZ(e.pos, dpos);
    if ((archer && !best_archer) || (archer == best_archer && d < best)) {
      best = d, best_id = id, best_attack = e.attack_id, best_archer = archer;
    }
  }
  static uint32_t shoot_id = 0;
  if (!shoot_id) shoot_id = SimAttackId("AI_Archer_Shoot");
  if (best_id && (best_archer || !best_attack)) best_attack = shoot_id;
  if (!best_id || !best_attack || !TryEnterCriticalSection(&g_net_lock)) return false;
  const std::vector<uint8_t> ev = sowenemy::DamageEvent(best_id, best_attack, dpos);
  g_net->sendEvent(ev.data(), ev.size());
  LeaveCriticalSection(&g_net_lock);
  return true;
}

void DecoyFrame() {
  const ULONGLONG now = GetTickCount64();
  float at[3], rot[4];
  DecoyLimboFrame();
  if (!SimPuppetXform(at, rot) || !SimHasPuppet()) {
    if (g_decoy_owner) {
      SimSetDecoy(0);
      SimRetargetDropTarget(g_decoy_owner);
      DecoyRetire(g_decoy_owner);
      g_decoy_owner = 0;
    }
    return;
  }
  // The decoy stands where the client says it is (its latest packet, under a second old), not where the stand-in got to: the
  // stand-in moves by put-backs and fell 300-1,700 units behind a client roaming rough ground (session cm), taking the decoy and
  // so the host's enemies with it. coop_decoy_follow=standin: the stand-in's place (before session cm).
  if (g_cfg.decoy_follow_remote) {
    EnterCriticalSection(&g_lock);
    const bool fresh = g_have && now - g_last_at < 1000;
    float sent[3] = {g_last.pos[0], g_last.pos[1], g_last.pos[2]};
    LeaveCriticalSection(&g_lock);
    if (fresh && std::isfinite(sent[0]) && std::isfinite(sent[1]) && std::isfinite(sent[2])) {
      if (DistXZ(sent, at) > 150.0f) InterlockedIncrement(&g_decoy_by_remote);
      memcpy(at, sent, sizeof sent);
    }
  }
  if (g_decoy_owner && !SimProxyValid(g_decoy_owner)) {
    SimSetDecoy(0);
    SimProxyDrop(g_decoy_owner);
    g_decoy_owner = 0;
    g_decoy_next = now + 2000;
  }
  if (!g_decoy_owner) {
    if (now < g_decoy_next || SimSpawnBusy()) return;
    SimSpawnRvas rv;
    rv.gameclient_rva = g_cfg.gameclient_rva;
    std::string report;
    uintptr_t obj = 0;
    if (SimSpawn(rv, "Gondorian_Soldier", at, nullptr, &report, &obj, true) && obj && SimProxyAdopt(obj)) {
      g_decoy_owner = obj;
      memcpy(g_decoy_anchor, at, sizeof g_decoy_anchor);
      g_decoy_anchored = true;
      SimSetDecoy(obj);
      InterlockedIncrement(&g_decoy_spawned);
      SowLog(L"coop: a decoy ally stands where the stand-in stands (enemies attack it for the stand-in)");
    } else {
      g_decoy_next = now + 3000;
      SowLog(L"coop: no decoy yet: %S", report.c_str());
    }
    return;
  }
  float where[3] = {at[0], at[1] + g_cfg.decoy_dy, at[2]};
  // A long jump (the client joined at the host's place and then travelled, or fast travel): the decoy is made again there. Moved
  // by the game's teleport instead, a decoy made at the tower where the client joins and taken 5,800 units down to the test
  // arena drew no swing in 120 s, and one made in the arena drew 13 in 60 s (session ck); the teleport works for short moves
  // (session cj: 800 units). The game's navigation likely keeps it where it was made.
  if (g_cfg.decoy_remake > 0 && g_decoy_anchored &&
      (DistXZ(where, g_decoy_anchor) > g_cfg.decoy_remake || std::fabs(where[1] - g_decoy_anchor[1]) > 400.0f)) {
    if (InterlockedIncrement(&g_decoy_remade) <= 20)
      SowLog(L"coop: the client's place jumped %.0f units (%.0f up or down): the decoy is made again there",
             DistXZ(where, g_decoy_anchor), where[1] - g_decoy_anchor[1]);
    DecoyRemove();
    g_decoy_next = now;  // made again on the next frame the spawner is free
    return;
  }
  // Far from where the game last put it: the game's teleport (navigation and physics follow); otherwise the transform written.
  const bool reanchor = g_cfg.decoy_reanchor > 0 && (!g_decoy_anchored || DistXZ(where, g_decoy_anchor) > g_cfg.decoy_reanchor ||
                                                   std::fabs(where[1] - g_decoy_anchor[1]) > 200.0f);
  // Already where it should be: not written. A character whose transform is written every frame is not hit by arrows (session
  // cm, tools/archer_vs.py: a Gondorian_Soldier shot dead by two archers in 26 s took one arrow in 40 s with its position written
  // every 0.3 s; the host's archers had never hit the decoy).
  float now_at[3];
  const bool still = g_cfg.decoy_still > 0 && !reanchor && ReadFloatsAt(g_decoy_owner + 0xAC, now_at) &&
                     DistXZ(now_at, where) < g_cfg.decoy_still && std::fabs(now_at[1] - where[1]) < 50.0f;
  if (still) {
    InterlockedIncrement(&g_decoy_unwritten);
  } else if (reanchor && SimProxyMove(g_cfg.teleport_rva, g_decoy_owner, where, rot, false)) {
    memcpy(g_decoy_anchor, where, sizeof g_decoy_anchor);
    g_decoy_anchored = true;
    InterlockedIncrement(&g_decoy_reanchors);
  } else {
    SimProxyMove(g_cfg.teleport_rva, g_decoy_owner, where, rot, true);
  }
  // Hits through the hit function on the decoy are dropped (and told to the client), so health it lost since the last frame came
  // another way: arrows do not go through the hit function (session cf). Sent to the client as an archer's shot.
  float cur = 0, top = 0;
  if (SimHealthNow(g_decoy_owner, &cur, &top) && top > 0 && cur < top - 0.5f) {
    InterlockedAdd(&g_decoy_hurt_amount, (LONG)(top - cur + 0.5f));
    if (InterlockedIncrement(&g_decoy_hurt) <= 20)
      SowLog(L"coop: the decoy lost %.0f of %.0f health outside the hit function (an arrow? fire?)", top - cur, top);
    if (SendArrowHit(g_decoy_owner)) InterlockedIncrement(&g_decoy_hurt_sent);
  }
  SimProxyHealth(g_decoy_owner, 1.0f);
  // The archers' arrows meant for the decoy land on the stand-in, which stands where the decoy stands (session cm: the stand-in
  // took 7 arrows of ~28 in 50 s while the decoy took 1; arrows do not go through the hit function, so nothing was sent and the
  // stand-in's health ran down). Its losses outside the hit function go to the client the same way, and it is healed.
  const uintptr_t standin = SimPuppetOwner();
  if (g_cfg.decoy_hurt_send && standin && SimHealthNow(standin, &cur, &top) && top > 0 && cur > 0 && cur < top - 0.5f) {
    if (InterlockedIncrement(&g_standin_hurt) <= 20)
      SowLog(L"coop: the stand-in lost %.0f of %.0f health outside the hit function (an arrow?)", top - cur, top);
    if (SendArrowHit(standin)) InterlockedIncrement(&g_standin_hurt_sent);
    SimProxyHealth(standin, 1.0f);
  }
  if (g_decoy_hide) SimHideCharacter(g_decoy_owner);
}

// The host: an enemy that is after the host's own player goes after the client's decoy instead when the decoy is the nearer
// of the two (the client is the nearer player), and back when it is not. Only enemies that already have the player (or the
// decoy) as their target are touched: an idle enemy wakes when something hits it (the client's strikes, the host's player).
volatile LONG g_hunt_retargets = 0, g_hunt_engaged = 0, g_hunt_woken = 0, g_hunt_wake_failed = 0, g_hunt_unstuck = 0;
volatile LONG g_hunt_player_left = 0;  // redirects to the host's player dropped (coop_hunt_to_player=off)
// The last time each enemy's swing reached the decoy (EnemyHostDamage; simulation thread): the stuck-enemy watch below.
std::unordered_map<uintptr_t, ULONGLONG> g_swing_at;
ULONGLONG g_last_swing_at = 0;  // the last swing of any enemy that reached the decoy
volatile LONG g_decoy_stall_remade = 0;

bool ReadFloatsAt(uintptr_t addr, float out[3]) {
  __try {
    memcpy(out, reinterpret_cast<const void*>(addr), 12);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void HuntFrame() {
  if (!g_decoy_owner) return;
  static ULONGLONG last = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - last < 100) return;
  last = now;
  const uintptr_t me = SimLocalPlayer(g_cfg.gameclient_rva);
  if (!me) return;
  float mine[3], dec[3];
  if (!ReadFloatsAt(me + 0xAC, mine) || !ReadFloatsAt(g_decoy_owner + 0xAC, dec)) return;
  // coop_hunt_strict: a redirect swaps only these two, and an enemy that turned on someone else (an orc that hit it) is left to it.
  // Off by default: with it on, the orcs at the client fought each other more and attacked the client's place less (session cg:
  // 9, 19, 38 damage messages in three 90 s runs, against 49, 69, 63 with it off).
  if (g_cfg.hunt_strict) SimRetargetFrom(me, g_decoy_owner);
  else SimRetargetFrom(0, 0);
  uintptr_t list[256];
  const size_t count = SimEnemyList(list, 256);
  static std::unordered_map<uintptr_t, ULONGLONG> engaged;  // enemy -> when it was last seen after the player or the decoy
  LONG now_engaged = 0;
  for (size_t i = 0; i < count && g_cfg.hunt_retarget; ++i) {
    const uintptr_t e = list[i];
    const uintptr_t target = SimEnemyTarget(e);
    if (target != me && target != g_decoy_owner) continue;
    SimEnemyInfo info;
    if (!SimEnemyRead(e, &info)) continue;
    ++now_engaged;
    engaged[e] = now;
    const float to_me = DistXZ(info.pos, mine), to_dec = DistXZ(info.pos, dec);
    const bool want_decoy = target == g_decoy_owner ? to_dec < to_me + 150.0f : to_dec + 150.0f < to_me;
    const uintptr_t want = want_decoy ? g_decoy_owner : me;
    // Never redirected to the host's player (session cm): the swap writes the new target over every copy of the old one in the
    // enemy's AI data, and the host's player is not an AI character (its +0x2CE8/+0x2CF0 are a list head, +0x2D18 is 1), so the
    // AI's code later read it as one and the host crashed (11AA3A reading 0x28, 190F7C reading 0x1). The redirect is dropped
    // instead and the enemy's own AI chooses. coop_hunt_to_player=on keeps the old swap.
    if (want == me && !g_cfg.hunt_to_player) {
      if (target != me) InterlockedIncrement(&g_hunt_player_left);
      SimRetarget(e, 0);
      continue;
    }
    if (want != target) InterlockedIncrement(&g_hunt_retargets);
    SimRetarget(e, want);
  }
  // Wake idle enemies near the decoy: a hit from the decoy (health given back) makes it the enemy's target, the way the
  // game's own enemies notice the host's player. Hostile kinds only, not followers, each at most once in 15 s.
  static std::unordered_map<uintptr_t, ULONGLONG> poked;
  int pokes_left = 2;
  for (size_t i = 0; i < count && pokes_left > 0; ++i) {
    const uintptr_t e = list[i];
    if (SimEnemyTarget(e) != 0 || SimIsFollower(e)) continue;
    auto it = poked.find(e);
    if (it != poked.end() && now - it->second < 15000) continue;
    SimEnemyInfo info;
    if (!SimEnemyRead(e, &info) || !(info.hp > 0) || DistXZ(info.pos, dec) > g_cfg.wake_radius) continue;
    static const char* const kHostile[] = {"Orc", "OlogHai", "Warg", "Caragor", "Graug", "Ghul", "Uruk"};
    static const char* const kNot[] = {"Prisoner", "Slave", "Banner", "Tamed", "Captive", "Worker"};
    bool hostile = false;
    for (const char* k : kHostile) hostile = hostile || strncmp(info.type, k, strlen(k)) == 0;
    for (const char* k : kNot) hostile = hostile && !strstr(info.type, k);
    if (!hostile) continue;
    poked[e] = now;
    --pokes_left;
    if (SimPokeEnemy(g_cfg.hit_rva, g_cfg.attack_table_rva, e, g_decoy_owner, "AI_Infantry_Melee_Front_1_RR")) {
      if (InterlockedIncrement(&g_hunt_woken) <= 5) SowLog(L"coop: an idle %S near the client's decoy was woken", info.type);
    } else {
      InterlockedIncrement(&g_hunt_wake_failed);
    }
  }
  if (poked.size() > 512) poked.clear();
  // Stuck enemies: one that has the decoy as its target and stands near it, but whose swing has not reached it for
  // coop_stuck_sec, is let go by the hunt and poked by the decoy (it is hit by the client's place again, the way it got there).
  // Session cg saw an orc stand 430 units from the decoy, its target the decoy, without a swing for 50 s; the cause is not known.
  static std::unordered_map<uintptr_t, ULONGLONG> near_since, fixed_at;
  static std::unordered_map<uintptr_t, std::pair<uint32_t, ULONGLONG>> stamp_seen;  // last attack stamp, when it changed
  static std::unordered_map<uintptr_t, int> fixes;
  const ULONGLONG stuck_ms = (ULONGLONG)g_cfg.stuck_sec * 1000;
  for (size_t i = 0; i < count && stuck_ms; ++i) {
    const uintptr_t e = list[i];
    SimEnemyInfo info;
    if (SimEnemyTarget(e) != g_decoy_owner || !SimEnemyRead(e, &info) || !(info.hp > 0) || DistXZ(info.pos, dec) > 600.0f) {
      near_since.erase(e);
      continue;
    }
    auto& st = stamp_seen[e];
    if (st.first != info.attack_stamp || !st.second) st = {info.attack_stamp, now};
    ULONGLONG& since = near_since[e];
    if (!since) since = now;
    ULONGLONG quiet_from = since;
    auto sw = g_swing_at.find(e);
    if (sw != g_swing_at.end() && sw->second > quiet_from) quiet_from = sw->second;
    auto fx = fixed_at.find(e);
    if (fx != fixed_at.end() && fx->second > quiet_from) quiet_from = fx->second;
    if (now - quiet_from < stuck_ms) continue;
    fixed_at[e] = now;
    const int nth = ++fixes[e];
    SimRetarget(e, 0);
    engaged.erase(e);
    const bool poked_ok = SimPokeEnemy(g_cfg.hit_rva, g_cfg.attack_table_rva, e, g_decoy_owner, "AI_Infantry_Melee_Front_1_RR");
    if (InterlockedIncrement(&g_hunt_unstuck) <= 40)
      SowLog(L"coop: stuck: %S abs:%llX %.0f units from the decoy, health %.0f/%.0f, no swing at it for %llu s, its last attack %08X "
             L"started %llu s ago; let go and poked (%s, time %d for it)",
             info.type, (unsigned long long)e, DistXZ(info.pos, dec), info.hp, info.max_hp, (now - quiet_from) / 1000, info.attack_id,
             (now - st.second) / 1000, poked_ok ? L"ok" : L"poke failed", nth);
  }
  if (near_since.size() > 512) near_since.clear();
  if (fixed_at.size() > 512) fixed_at.clear();
  if (stamp_seen.size() > 512) stamp_seen.clear();
  if (fixes.size() > 512) fixes.clear();
  if (g_swing_at.size() > 512) g_swing_at.clear();
  // The decoy wears out in a fight (session ck): after ~90 s, orcs 300-500 units from it with it as their target went on with their
  // attacks and none landed (the hit function saw no hit); a decoy made again drew swings again. With coop_decoy_stall, when an
  // enemy after the decoy has stood within 600 of it and no swing of anyone has reached it for that long, it is made again.
  static ULONGLONG stall_from = 0, stall_remade_at = 0;
  bool near_after = false;
  for (size_t i = 0; i < count && g_cfg.decoy_stall > 0 && !near_after; ++i) {
    SimEnemyInfo info;
    near_after = SimEnemyTarget(list[i]) == g_decoy_owner && SimEnemyRead(list[i], &info) && info.hp > 0 && DistXZ(info.pos, dec) <= 600.0f;
  }
  if (!near_after) {
    stall_from = 0;
  } else {
    if (!stall_from) stall_from = now;
    ULONGLONG quiet_from = stall_from;
    if (g_last_swing_at > quiet_from) quiet_from = g_last_swing_at;
    if (stall_remade_at > quiet_from) quiet_from = stall_remade_at;
    if (now - quiet_from >= (ULONGLONG)g_cfg.decoy_stall * 1000) {
      if (InterlockedIncrement(&g_decoy_stall_remade) <= 20)
        SowLog(L"coop: enemies after the decoy, no swing reached it for %llu s: the decoy is made again", (now - quiet_from) / 1000);
      stall_remade_at = now;
      stall_from = 0;
      g_hunt_engaged = now_engaged;
      DecoyRemove();
      g_decoy_next = now;
      return;
    }
  }
  g_hunt_engaged = now_engaged;
  for (auto it = engaged.begin(); it != engaged.end();) {  // forget enemies that stopped: the retarget slots are few
    if (now - it->second > 3000 || !g_cfg.hunt_retarget) {
      SimRetarget(it->first, 0);
      it = engaged.erase(it);
    } else {
      ++it;
    }
  }
}

// The last damage reports (probe `dmglog`): the host's swings that reached the decoy (the attacker's distance from where it hit
// tells an arrow from a sword) and what the client did with the host's damage messages.
struct DmgLogEntry {
  ULONGLONG at;
  uintptr_t owner;  // host: the attacking character; client: its proxy (0: none)
  uint32_t id, attack_id;
  float dist;   // host: the attacker's distance from the hit; client: -1
  char result;  // host: 's' sent, 'u' not an announced enemy; client: 'a' applied, 'f' failed, 'x' no proxy (stale), 'b' applied with the
  // nearest proxy as the attacker (dist: that proxy's distance from the hit), 'h' a strike of this player on that proxy (sent to
  // the host; session cm, to line up the client's counters with the damage it takes)
};
constexpr LONG kDmgLogSize = 128;
DmgLogEntry g_dmg_log[kDmgLogSize];
LONG g_dmg_log_n = 0;
CRITICAL_SECTION* DmgLogLock() {
  static CRITICAL_SECTION cs;
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID p, PVOID*) -> BOOL { InitializeCriticalSection((CRITICAL_SECTION*)p); return TRUE; },
                      &cs, nullptr);
  return &cs;
}
void DmgLog(uintptr_t owner, uint32_t id, uint32_t attack_id, float dist, char result) {
  EnterCriticalSection(DmgLogLock());
  g_dmg_log[g_dmg_log_n++ % kDmgLogSize] = DmgLogEntry{GetTickCount64(), owner, id, attack_id, dist, result};
  LeaveCriticalSection(DmgLogLock());
}

// Counters (session cm): the host's enemy hits the host's picture of the client whatever the client does on its own screen, so a
// countered swing was still taken. The client's counter strike reaches its proxy about 0.8 s after that swing's damage message
// (counter_log.py: +796, +828 ms). The health this player had before the last swing of each enemy is kept; a counter strike on
// that enemy within 1.5 s gives it back (never above it). Off by default (coop_counter_heal=on): in the tests the counters that
// followed a damage message did not leave a health loss to give back, and others hit enemies whose last swing was seconds old (no
// damage message for the countered swing), so whether a countered swing still hurts the client is not settled.
struct LastSwing {
  ULONGLONG at;
  float before;  // this player's health before the swing was applied
};
std::unordered_map<uint32_t, LastSwing> g_last_swing_by;  // enemy id -> its last swing applied here
volatile LONG g_counter_heals = 0, g_counter_strikes = 0, g_counter_logged = 0;

std::string DamageLogText() {
  std::string s = "ok dmglog n=" + std::to_string(g_dmg_log_n) + "\n";
  EnterCriticalSection(DmgLogLock());
  const ULONGLONG now = GetTickCount64();
  const LONG first = g_dmg_log_n > kDmgLogSize ? g_dmg_log_n - kDmgLogSize : 0;
  for (LONG i = first; i < g_dmg_log_n; ++i) {
    const DmgLogEntry& e = g_dmg_log[i % kDmgLogSize];
    char line[160];
    snprintf(line, sizeof line, "%c ago=%llu owner=abs:%llX id=%u attack=%08X dist=%.0f\n", e.result, now - e.at,
             (unsigned long long)e.owner, e.id, e.attack_id, e.dist);
    s += line;
  }
  LeaveCriticalSection(DmgLogLock());
  return s;
}

// The host: enemy swings that reached the client's stand-in go to the client, whose player takes them.
void EnemyHostDamage() {
  static std::vector<std::vector<uint8_t>> pending;
  SimHitReport rp[16];
  const size_t n = SimDamageReports(rp, 16);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t id = g_enemy_host.idOf(rp[i].owner);
    g_swing_at[rp[i].owner] = g_last_swing_at = GetTickCount64();
    float at[3];
    const float dist = ReadFloatsAt(rp[i].owner + 0xAC, at)
                           ? std::sqrt((at[0] - rp[i].pos[0]) * (at[0] - rp[i].pos[0]) + (at[1] - rp[i].pos[1]) * (at[1] - rp[i].pos[1]) +
                                       (at[2] - rp[i].pos[2]) * (at[2] - rp[i].pos[2]))
                           : -1.0f;
    DmgLog(rp[i].owner, id, rp[i].attack_id, dist, id ? 's' : 'u');
    if (id) pending.push_back(sowenemy::DamageEvent(id, rp[i].attack_id, rp[i].pos));
    else InterlockedIncrement(&g_enemy_dmg_stale);  // an attacker that is not an announced enemy (an ally, a stray)
  }
  if (!pending.empty() && TryEnterCriticalSection(&g_net_lock)) {
    for (const auto& ev : pending) g_net->sendEvent(ev.data(), ev.size());
    InterlockedAdd(&g_enemy_dmg_sent, (LONG)pending.size());
    pending.clear();
    LeaveCriticalSection(&g_net_lock);
  }
  if (pending.size() > 64) pending.clear();
}

// ---- Nemesis events ------------------------------------------------------------------------------------------
// Both roles record the events their game applies (SimFactionHookInstall). The host sends each one that is not load
// noise; the client counts the host's (EnemyClientFrame hands them to NemesisGot) and its own.

std::vector<std::vector<uint8_t>> g_nemesis_out;  // host: kept until the transport can take them

volatile LONG g_nemesis_applied = 0, g_nemesis_failed = 0;

sowenemy::MemberAnnounced g_members_announced;  // host: the members the client has been sent
sowenemy::MemberAssembler g_member_asm;         // client: the member being received
volatile LONG g_member_sent = 0, g_member_got = 0, g_member_skipped = 0, g_member_built = 0, g_member_failed = 0;

// Host: puts the member's text in front of what names him (queue), once, so that a client that has no such member (one
// made after it joined) can make him. A member the client has already is not rebuilt there (same seed).
void AnnounceMember(uint32_t faction_hash, uint32_t id, std::vector<std::vector<uint8_t>>& queue) {
  if (!g_cfg.member_data || !faction_hash || id == sowenemy::kNoMember) return;
  char name[40];
  const uintptr_t member = SimFindMember(faction_hash, id, name);
  if (!member) return;
  const uint64_t seed = SimMemberSeed(member);
  if (!g_members_announced.needs(faction_hash, id, seed)) return;
  std::string text, err;
  if (!SimMemberExport(member, &text, &err)) {
    if (InterlockedIncrement(&g_member_failed) <= 5) SowLog(L"coop: member %S (%u) cannot be sent: %S", name, id, err.c_str());
    return;
  }
  const auto chunks = sowenemy::MemberDataEvents(faction_hash, id, seed, text);
  if (chunks.empty()) return;
  for (const auto& c : chunks) queue.push_back(c);
  if (InterlockedIncrement(&g_member_sent) <= 20)
    SowLog(L"coop: member %S (id %u) sent to the client: %zu bytes in %zu messages", name, id, text.size(), chunks.size());
}

// Client: a member from the host. One this copy already has (same seed in that slot) is left alone.
void MemberDataGot(const uint8_t* p, size_t n) {
  uint32_t hash = 0, id = 0;
  uint64_t seed = 0;
  std::string text;
  if (!g_member_asm.add(p, n, &hash, &id, &seed, &text)) return;
  InterlockedIncrement(&g_member_got);
  char name[40];
  const uintptr_t have = SimFindMember(hash, id, name);
  if (have && SimMemberSeed(have) == seed) {
    InterlockedIncrement(&g_member_skipped);
    return;
  }
  std::string err;
  if (SimMemberImport(hash, id, text, &err)) {
    if (InterlockedIncrement(&g_member_built) <= 20)
      SowLog(L"coop: member %u made here from the host's (%zu bytes)%S", id, text.size(), have ? " in place of another" : "");
  } else if (InterlockedIncrement(&g_member_failed) <= 20) {
    SowLog(L"coop: member %u from the host could not be made here: %S", id, err.c_str());
  }
}

void NemesisGot(const sowenemy::NemesisMsg& m) {
  InterlockedIncrement(&g_nemesis_got);
  if (!g_cfg.nemesis_follow) {
    if (g_nemesis_got <= 20) SowLog(L"coop: Nemesis event from the host: %S (subject %S, other %S)", m.event, m.subj_name, m.other_name);
    return;
  }
  {  // where the game's presentations will look for the subject: the character his member points to (a proxy or his own)
    static int shown = 0;
    uint32_t hash = 0, id = 0;
    memcpy(&hash, m.subj, 4);
    memcpy(&id, m.subj + 8, 4);
    uintptr_t member = 0, character = 0;
    float at[3] = {};
    if (shown < 80 && id != sowenemy::kNoMember && SimMemberCharacter(hash, id, &member, &character, at)) {
      ++shown;
      const char* kind = !character ? "none" : "his own";
      for (auto& kv : g_enemy_client.proxies())
        if (character && kv.second.handle == character) kind = "a proxy";
      Packet me{};
      float mine = -1.0f;
      if (ReadPlayer(&me)) mine = DistXZ(at, me.pos);
      SowLog(L"coop: Nemesis event %S, subject %S (%u): his member points to %S %llX, %.0f from this player", m.event, m.subj_name, id,
             kind, (unsigned long long)character, character ? mine : -1.0f);
    }
  }
  SimFactionEvent e{};
  memcpy(e.subj, m.subj, 16);
  memcpy(e.other, m.other, 16);
  e.event_hash = m.event_hash;
  e.region_hash = m.region_hash;
  memcpy(e.flags, m.flags, 2);
  std::string err;
  if (SimFactionReplay(g_cfg.faction_event_rva, e, &err)) {
    if (InterlockedIncrement(&g_nemesis_applied) <= 20)
      SowLog(L"coop: Nemesis event from the host applied here: %S (subject %S, other %S)", m.event, m.subj_name, m.other_name);
  } else if (InterlockedIncrement(&g_nemesis_failed) <= 20) {
    SowLog(L"coop: Nemesis event from the host not applied: %S (subject %S): %S", m.event, m.subj_name, err.c_str());
  }
}

void NemesisFrame() {
  static bool installed = false;
  if (!installed) {
    installed = true;
    std::string e;
    if (!SimFactionHookInstall(g_cfg.faction_event_rva, g_cfg.faction_lookup_rva, &e)) SowLog(L"coop: Nemesis events are not recorded: %S", e.c_str());
    else SowLog(L"coop: the Nemesis events this game applies are recorded");
    if (g_cfg.net_role == "client" && g_cfg.nemesis_follow) {
      SimFactionDrop(true);
      SimFactionPrime();
      SowLog(L"coop: this client's own Nemesis events are dropped; the host's are applied");
    }
    if (g_cfg.net_role == "client") {
      if (!SimAmbushEnable(g_cfg.ambush_flag_rva, false, &e)) SowLog(L"coop: this client's own ambushes stay on: %S", e.c_str());
      else SowLog(L"coop: this client's own ambushes are off (the host's show up as its enemies)");
    }
  }
  static sownet::Status status = sownet::Status::kIdle;
  if (TryEnterCriticalSection(&g_net_lock)) {  // never wait for it: see SendSample
    status = g_net->status();
    LeaveCriticalSection(&g_net_lock);
  }
  SimFactionEvent ev[64];
  const size_t n = SimFactionTake(ev, 64);
  if (g_cfg.net_role != "host") return;
  for (size_t i = 0; i < n; ++i) {
    if (status != sownet::Status::kConnected) continue;
    if (!sowenemy::NemesisShared(ev[i].event)) {
      InterlockedIncrement(&g_nemesis_skipped);
      continue;
    }
    for (const uint8_t* ref : {ev[i].subj, ev[i].other}) {
      uint32_t hash = 0, id = 0;
      memcpy(&hash, ref, 4);
      memcpy(&id, ref + 8, 4);
      AnnounceMember(hash, id, g_nemesis_out);
    }
    sowenemy::NemesisMsg m{};
    memcpy(m.subj, ev[i].subj, 16);
    memcpy(m.other, ev[i].other, 16);
    m.event_hash = ev[i].event_hash;
    m.region_hash = ev[i].region_hash;
    memcpy(m.flags, ev[i].flags, 2);
    memcpy(m.event, ev[i].event, sizeof m.event);
    memcpy(m.subj_name, ev[i].subj_name, sizeof m.subj_name);
    memcpy(m.other_name, ev[i].other_name, sizeof m.other_name);
    g_nemesis_out.push_back(sowenemy::NemesisEvent(m));
    static int raw_logged = 0;
    if (raw_logged < 6) {
      ++raw_logged;
      char hex[0x60 * 2 + 1];
      for (size_t k = 0; k < 0x60; ++k) snprintf(hex + 2 * k, 3, "%02X", ev[i].raw[k]);
      SowLog(L"coop: Nemesis event %S raw %S", ev[i].event, hex);
    }
  }
  if (!g_nemesis_out.empty() && TryEnterCriticalSection(&g_net_lock)) {
    for (const auto& msg : g_nemesis_out) g_net->sendEvent(msg.data(), msg.size());
    InterlockedAdd(&g_nemesis_sent, (LONG)g_nemesis_out.size());
    g_nemesis_out.clear();
    LeaveCriticalSection(&g_net_lock);
  }
  if (g_nemesis_out.size() > 4096) g_nemesis_out.clear();
}

void EnemyHostFrame() {
  if (g_test_standin_owner) {
    const uintptr_t who = (uintptr_t)InterlockedExchange64(&g_test_standin_owner, 0);
    g_test_standin_result = SimHitStandin(g_cfg.hit_rva, g_cfg.attack_table_rva, who, (uint32_t)g_test_dmg_attack) ? 1 : -1;
  }
  static ULONGLONG last_ms = 0;
  const ULONGLONG now = GetTickCount64();
  static bool hurt_set = false;
  if (!hurt_set) {
    hurt_set = true;
    SimStandinHurt(true);  // before the puppet code sets what the stubs skip
    SimDropStandinHits(!g_cfg.standin_hits);
    std::string e;
    if (!SimAttackHooksInstall(g_cfg.target_check_rva, g_cfg.hit_rva, g_cfg.aoe_rva, &e))
      SowLog(L"coop: enemy swings cannot be carried to the client: %S", e.c_str());
  }
  EnemyHostHits();
  EnemyHostDamage();
  if (g_cfg.decoy) {
    DecoyFrame();
    HuntFrame();
  } else {
    DecoyRemove();
  }
  if (now - last_ms < 66) return;
  last_ms = now;
  static sownet::Status status = sownet::Status::kIdle;
  static uint32_t session = 0;
  if (TryEnterCriticalSection(&g_net_lock)) {  // never wait for it: see SendSample
    status = g_net->status();
    session = g_net->session();
    LeaveCriticalSection(&g_net_lock);
  }
  if (status != sownet::Status::kConnected) return;
  static bool started = false;
  if (!started) {
    started = true;
    std::string e;
    if (g_cfg.anim_mirror && !SimActionTapInstall(g_cfg.action_start_rva, &e)) SowLog(L"coop: the host's enemies' animations cannot be mirrored: %S", e.c_str());
    if (!SimEnemyRegistryStart(g_cfg.brain_site_rva, g_cfg.tree_rva, &e)) SowLog(L"coop: the host cannot list its enemies: %S", e.c_str());
    else SowLog(L"coop: the host announces the enemies within %.0f units of either player", g_cfg.enemy_radius);
  }
  if (session != g_enemy_session) {
    g_enemy_session = session;
    g_enemy_host.forgetAll();
    g_members_announced.forgetAll();
  }
  Packet me{};
  float mine[3], theirs[3];
  bool have_theirs = false;
  if (!ReadPlayer(&me)) return;
  memcpy(mine, me.pos, sizeof mine);
  EnterCriticalSection(&g_lock);
  if (g_have) {
    memcpy(theirs, g_last.pos, sizeof theirs);
    have_theirs = true;
  }
  LeaveCriticalSection(&g_lock);
  uintptr_t list[256];
  const size_t count = SimEnemyList(list, 256);
  struct Cand {
    float d;
    sowenemy::Seen s;
  };
  std::vector<Cand> cand;
  for (size_t i = 0; i < count; ++i) {
    SimEnemyInfo info;
    if (!SimEnemyRead(list[i], &info) || !info.type[0]) continue;
    float d = DistXZ(info.pos, mine);
    if (have_theirs) d = std::min(d, DistXZ(info.pos, theirs));
    if (d > g_cfg.enemy_radius) continue;
    Cand c;
    c.d = d;
    c.s.key = list[i];
    memcpy(c.s.pos, info.pos, sizeof c.s.pos);
    c.s.yaw = YawOf(info.rot);
    c.s.attack_id = info.attack_id;
    c.s.attack_stamp = info.attack_stamp;
    c.s.hp_frac = info.max_hp > 0 ? info.hp / info.max_hp : 1.0f;
    c.s.alive = info.hp > 0;
    c.s.targets_client = g_decoy_owner && SimEnemyTarget(list[i]) == g_decoy_owner;
    sowenemy::CopyType(c.s.type, info.type);
    if (g_cfg.captain_look && info.member_id != sowenemy::kNoMember) {
      c.s.member_id = info.member_id;
      c.s.faction_hash = info.faction_hash;
      sowenemy::CopyName(c.s.name, info.member_name);
    }
    cand.push_back(c);
  }
  // Captains first (a crowd of orcs must not push the captain out of the 16 shown), then by distance.
  std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
    const bool ca = a.s.member_id != sowenemy::kNoMember, cb = b.s.member_id != sowenemy::kNoMember;
    return ca != cb ? ca : a.d < b.d;
  });
  std::vector<sowenemy::Seen> chosen;
  for (size_t i = 0; i < cand.size() && i < sowenemy::kMaxEnemies; ++i) chosen.push_back(cand[i].s);
  const sowenemy::HostTracker::Out out = g_enemy_host.update(chosen, now);
  // Events are kept until the transport can take them (the lock may be busy: see SendSample); a snapshot is not.
  static std::vector<std::vector<uint8_t>> pending;
  for (const auto& c : chosen)
    if (c.member_id != sowenemy::kNoMember) AnnounceMember(c.faction_hash, c.member_id, pending);
  for (const auto& ev : out.events) pending.push_back(ev);
  {  // the graph actions the announced enemies have started since the last tick, in order (a repeat of the same one is dropped)
    SimActionStart starts[256];
    const size_t n = SimActionTapDrain(starts, 256);
    static std::unordered_map<uintptr_t, std::string> last;
    for (size_t i = 0; i < n; ++i) {
      const uint32_t id = g_enemy_host.idOf(starts[i].character);
      if (!id) continue;
      auto& l = last[starts[i].character];
      if (l == starts[i].name) continue;
      l = starts[i].name;
      pending.push_back(sowenemy::AnimEvent(id, starts[i].name));
      InterlockedIncrement(&g_enemy_anims_sent);
    }
    if (last.size() > 2048) last.clear();
  }
  if (!TryEnterCriticalSection(&g_net_lock)) return;
  for (const auto& ev : pending) {
    g_net->sendEvent(ev.data(), ev.size());
    if (!ev.empty() && ev[0] == sowenemy::kOpMember) InterlockedIncrement(&g_enemy_members_sent);
  }
  InterlockedAdd(&g_enemy_announced, (LONG)pending.size());
  pending.clear();
  if (!out.snapshot.empty()) g_net->sendState2(out.snapshot.data(), out.snapshot.size());
  LeaveCriticalSection(&g_net_lock);
  if (!out.snapshot.empty()) InterlockedIncrement(&g_enemy_sent);
}

std::vector<uintptr_t> g_limbo;  // freed proxies the game has not removed yet: their AI stays off until they are gone

void FreeProxy(sowenemy::Proxy& p) {
  if (!p.handle) return;
  if (p.applied_record) SimProxyStopAttack(p.handle, p.applied_record);
  // The host's enemy is gone (dead, out of range, or the session ended): the proxy dies and is asked to go once
  // it is out of sight. Until the game has removed it, it stays a proxy: no AI, so no stray fight.
  // A captain's proxy was made from this copy's own member: it is let go of the member first, so that its death is an
  // ordinary orc's and not this copy's captain's.
  if (p.built_member != sowenemy::kNoMember) {
    SimMemberLink(p.faction_hash, p.built_member, 0, p.handle);
    SimProxyDetachMember(p.handle);
  }
  if (!p.killed && !SimKillCharacter(g_cfg.hit_rva, g_cfg.attack_table_rva, p.handle, g_cfg.gameclient_rva, "PC_NT_L1_D1_F_LL_RL_04"))
    SimProxyHealth(p.handle, 0.0f);
  SimDespawnNow(g_cfg.despawn_rva, p.handle);
  if (g_limbo.size() < 48) g_limbo.push_back(p.handle);
  else SimProxyDrop(p.handle);
  p.handle = 0;
}

// ---- The client's AI tables (session cm) ----------------------------------------------------------------------------------
// Each AI character has a table (class 1FF9F50, at +0x140 of a 0xE2C8-byte object; the client holds 256) whose entries the
// animation state machine's start-action makes (195988 -> 373E1C -> 3749D8); a normal character's update prunes the list (how
// is not found), and the game crashes on purpose when a walk of it counts 256 (11244A). A proxy's tree is skipped, so under fire
// its table grows ~0.5 entries a second until the client crashes (three times in session cm). A proxy or stand-in whose count or
// list reaches coop_proxy_table_max (200) is retired: taken far out of sight, freed the usual way (killed, despawned, kept with
// its AI off until the game deletes it), and made again where the host's enemy is, with an empty table.
constexpr uintptr_t kAiTableVt = 0x1FF9F50;
std::vector<uintptr_t> g_ai_tables;  // found by AiTableScan (the table objects); guarded by g_lock
volatile LONG g_ai_table_scans = 0, g_proxy_recycled = 0, g_ai_table_max_seen = 0, g_puppet_recycled = 0, g_ai_list_max_seen = 0;

DWORD WINAPI AiTableScan(void*) {
  std::vector<uintptr_t> hits(1024);
  for (;;) {
    size_t scanned = 0;
    const size_t n = SimScanHeap(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kAiTableVt, hits.data(), hits.size(),
                                 &scanned);
    EnterCriticalSection(&g_lock);
    g_ai_tables.assign(hits.begin(), hits.begin() + n);
    LeaveCriticalSection(&g_lock);
    InterlockedIncrement(&g_ai_table_scans);
    Sleep(15000);
  }
}

// The table's count, the length of its entry list and its character (0 when it is not one). The count is only worked out when
// the game walks the list (112354, then the 256 check in 1123CC); the client's stand-in of the host was seen with count 0 and 407
// entries, growing 1.75 a second while the host fought (session cm), so the next walk would have stopped the game. The list is
// circular with its head at table+8 (next at +8); the walk here stops at kAiListCap.
constexpr uint32_t kAiListCap = 512;
bool AiTableRead(uintptr_t t, uint32_t* count, uint32_t* list, uintptr_t* character) {
  const uintptr_t vt = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kAiTableVt;
  __try {
    if (*reinterpret_cast<const uintptr_t*>(t) != vt) return false;
    *count = *reinterpret_cast<const uint32_t*>(t + 0x70);
    const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(t + 0x60);
    *character = obj ? *reinterpret_cast<const uintptr_t*>(obj + 0x40) : 0;
    const uintptr_t head = t + 8;
    uint32_t n = 0;
    for (uintptr_t node = *reinterpret_cast<const uintptr_t*>(head + 8); node && node != head && n < kAiListCap; ++n)
      node = *reinterpret_cast<const uintptr_t*>(node + 8);
    *list = n;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void FreeProxy(sowenemy::Proxy& p);  // below

void ProxyTableFrame(ULONGLONG now) {
  static ULONGLONG next = 0;
  static bool thread = false;
  if (g_cfg.proxy_table_max <= 0 || now < next) return;
  next = now + 2000;
  if (!thread) {
    thread = true;
    HANDLE h = CreateThread(nullptr, 0, AiTableScan, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    return;
  }
  std::vector<uintptr_t> tables;
  EnterCriticalSection(&g_lock);
  tables = g_ai_tables;
  LeaveCriticalSection(&g_lock);
  auto& proxies = g_enemy_client.proxies();
  const uintptr_t puppet = SimPuppetOwner();
  for (uintptr_t t : tables) {
    uint32_t count = 0, list = 0;
    uintptr_t ch = 0;
    if (!AiTableRead(t, &count, &list, &ch) || !ch) continue;
    if ((LONG)count > g_ai_table_max_seen) InterlockedExchange(&g_ai_table_max_seen, (LONG)count);
    if ((LONG)list > g_ai_list_max_seen && (ch == puppet || g_cfg.net_role == "client"))
      InterlockedExchange(&g_ai_list_max_seen, (LONG)list);
    if ((int)std::max(count, list) < g_cfg.proxy_table_max) continue;
    count = std::max(count, list);
    if (ch == puppet) {  // the stand-in's table grows too (~77 on the client after 7 minutes of fighting, session cm)
      const uintptr_t old = SimRetirePuppet();
      if (old) {
        SimDespawnNow(g_cfg.despawn_rva, old);
        InterlockedIncrement(&g_puppet_recycled);
        SowLog(L"coop: the stand-in's AI table held %u entries; a new stand-in is made", count);
      }
      continue;
    }
    if (g_cfg.net_role != "client") continue;
    for (auto& kv : proxies) {
      sowenemy::Proxy& p = kv.second;
      if (p.handle != ch) continue;
      float away[3] = {p.cur.pos[0], p.cur.pos[1] - 20000.0f, p.cur.pos[2]};
      const float rot[4] = {0, 0, 0, 1};
      SimProxyMove(g_cfg.teleport_rva, p.handle, away, rot, true);
      FreeProxy(p);
      p.applied_record = 0;
      p.applied_attack = p.applied_stamp = 0;
      p.applied_loco = 255;
      p.shown_set = false;
      p.next_spawn_ms = now;
      if (InterlockedIncrement(&g_proxy_recycled) <= 20)
        SowLog(L"coop: enemy %u (%S): its proxy's AI table held %u entries; made again", p.id, p.type, count);
      break;
    }
  }
}

void EnemyClientFrame() {
  const ULONGLONG now = GetTickCount64();
  static bool started = false;
  if (!started) {
    started = true;
    std::string e;
    if (!SimEnemyRegistryStart(g_cfg.brain_site_rva, g_cfg.tree_rva, &e)) SowLog(L"coop: the client cannot keep its proxies' AI off: %S", e.c_str());
    if (!SimAmbientHookInstall(g_cfg.ambient_rva, &e)) SowLog(L"coop: the client's own ambient enemies are not stopped: %S", e.c_str());
    else SimAmbientBlock(true);
    // A strike of this player on a proxy is dropped here (a proxy is a picture) and told to the host.
    if (!SimAttackHooksInstall(g_cfg.target_check_rva, g_cfg.hit_rva, g_cfg.aoe_rva, &e))
      SowLog(L"coop: this client's strikes on proxies are not carried: %S", e.c_str());
    else {
      SimHitWatch(g_cfg.gameclient_rva, true);
      SimProxyReact(g_cfg.proxy_react);
      SimDropStandinHits(!g_cfg.standin_hits);
    }
    SowLog(L"coop: this client shows the host's enemies as proxies and keeps its own ambient enemies away");
  }
  static uint32_t session = 0;
  static sownet::Status status = sownet::Status::kIdle;
  if (TryEnterCriticalSection(&g_net_lock)) {  // never wait for it: see SendSample
    session = g_net->session();
    status = g_net->status();
    LeaveCriticalSection(&g_net_lock);
  }
  auto& proxies = g_enemy_client.proxies();
  for (size_t i = 0; i < g_limbo.size();) {
    if (!SimProxyValid(g_limbo[i])) {
      SimProxyDrop(g_limbo[i]);
      g_limbo.erase(g_limbo.begin() + i);
    } else {
      ++i;
    }
  }
  if (session != g_enemy_session) {  // a new session: the host announces everything again
    g_enemy_session = session;
    for (auto& kv : proxies) FreeProxy(kv.second);
    g_enemy_client.clear();
  }
  std::vector<std::vector<uint8_t>> events;
  std::vector<uint8_t> snap;
  uint64_t snap_ms = 0;
  EnterCriticalSection(&g_lock);
  events.swap(g_enemy_events_in);
  snap.swap(g_enemy_snap_in);
  snap_ms = g_enemy_snap_ms;
  LeaveCriticalSection(&g_lock);
  std::vector<uint32_t> gone;
  for (const auto& ev : events) {
    if (!ev.empty() && ev[0] == sowenemy::kOpMemberData) {
      MemberDataGot(ev.data(), ev.size());
      continue;
    }
    sowenemy::NemesisMsg nm;
    if (sowenemy::ParseNemesis(ev.data(), ev.size(), &nm)) {
      NemesisGot(nm);
      continue;
    }
    sowenemy::HitMsg dm;
    if (sowenemy::ParseDamage(ev.data(), ev.size(), &dm)) {
      auto it = proxies.find(dm.id);
      if ((it == proxies.end() || !it->second.handle) && g_cfg.dmg_borrow) {
        // No proxy of that enemy here (the host shows at most 16; with a camp joining in, 27 of 76 swings in 30 s had none,
        // session cm): the swing is applied with the nearest proxy as the attacker, so the attack (its record: the damage) lands.
        auto best = proxies.end();
        float best_d = 1e30f;
        for (auto p = proxies.begin(); p != proxies.end(); ++p) {
          float at[3];
          if (!p->second.handle || !ReadFloatsAt(p->second.handle + 0xAC, at)) continue;
          const float d = (at[0] - dm.pos[0]) * (at[0] - dm.pos[0]) + (at[2] - dm.pos[2]) * (at[2] - dm.pos[2]);
          if (d < best_d) best_d = d, best = p;
        }
        float hp_b = 0, top_b = 0;
        const uintptr_t me_b = SimLocalPlayer(g_cfg.gameclient_rva);
        const bool had_b = me_b && SimHealthNow(me_b, &hp_b, &top_b);
        if (best != proxies.end() &&
            SimApplyDamage(g_cfg.hit_rva, g_cfg.attack_table_rva, best->second.handle, dm.pos, dm.attack_id, g_cfg.gameclient_rva)) {
          if (had_b) g_last_swing_by[dm.id] = LastSwing{GetTickCount64(), hp_b};
          InterlockedIncrement(&g_enemy_dmg_borrowed);
          DmgLog(best->second.handle, dm.id, dm.attack_id, std::sqrt(best_d), 'b');
          continue;
        }
      }
      float hp_a = 0, top_a = 0;  // this player's health before the swing (for a counter, below)
      if (const uintptr_t me_a = SimLocalPlayer(g_cfg.gameclient_rva)) SimHealthNow(me_a, &hp_a, &top_a);
      if (it == proxies.end() || !it->second.handle) {
        InterlockedIncrement(&g_enemy_dmg_stale);
        DmgLog(0, dm.id, dm.attack_id, -1.0f, 'x');
      } else if (SimApplyDamage(g_cfg.hit_rva, g_cfg.attack_table_rva, it->second.handle, dm.pos, dm.attack_id, g_cfg.gameclient_rva)) {
        if (hp_a > 0) g_last_swing_by[dm.id] = LastSwing{GetTickCount64(), hp_a};
        if (InterlockedIncrement(&g_enemy_dmg_applied) <= 5)
          SowLog(L"coop: enemy %u (%S) hit this player (attack %08X): applied", dm.id, it->second.type, dm.attack_id);
        DmgLog(it->second.handle, dm.id, dm.attack_id, -1.0f, 'a');
      } else {
        InterlockedIncrement(&g_enemy_dmg_failed);
        DmgLog(it->second.handle, dm.id, dm.attack_id, -1.0f, 'f');
      }
      continue;
    }
    // GONE: free the character before the tracker forgets it
    if (ev.size() == 5 && ev[0] == sowenemy::kOpGone) {
      uint32_t id;
      memcpy(&id, &ev[1], 4);
      auto it = proxies.find(id);
      if (it != proxies.end()) FreeProxy(it->second);
    }
    g_enemy_client.onEvent(ev.data(), ev.size(), &gone);
  }
  if (!snap.empty()) g_enemy_client.onSnapshot(snap.data(), snap.size(), snap_ms);
  bool away = false;
  EnterCriticalSection(&g_lock);
  away = g_have && now > g_last_at + kHostAwayMs;
  LeaveCriticalSection(&g_lock);
  static bool was_away = false;
  if (away != was_away) {
    was_away = away;
    if (away) {
      InterlockedIncrement(&g_enemy_away);
      for (auto& kv : proxies) {
        sowenemy::Proxy& p = kv.second;
        FreeProxy(p);
        p.have = false;  // shown again once the host's states carry it again
        p.applied_record = 0;
        p.applied_attack = p.applied_stamp = 0;
        p.applied_loco = 255;
        p.shown_set = false;
      }
    }
    SowLog(away ? L"coop: no state from the host for %llu s: its enemies are taken away until it is back"
                : L"coop: the host is back: its enemies are shown again", (unsigned long long)(kHostAwayMs / 1000));
  }
  // (The away check above comes first: the transport reports the host lost after a short silence, and this returns then.)
  if (status != sownet::Status::kConnected && status != sownet::Status::kSyncing) return;
  // Strikes of this player on proxies, to the host.
  {
    SimHitReport hr[16];
    const size_t n = SimHitReports(hr, 16);
    static std::vector<std::vector<uint8_t>> outbox;
    for (size_t i = 0; i < n; ++i) {
      bool matched = false;
      for (auto& kv : proxies)
        if (kv.second.handle == hr[i].owner) {
          outbox.push_back(sowenemy::HitEvent(kv.first, hr[i].attack_id, hr[i].pos));
          DmgLog(hr[i].owner, kv.first, hr[i].attack_id, -1.0f, 'h');
          if (g_cfg.counter_heal && SimIsCounterAttack(g_cfg.attack_table_rva, hr[i].attack_id)) {
            InterlockedIncrement(&g_counter_strikes);
            auto ls = g_last_swing_by.find(kv.first);
            const uintptr_t me = SimLocalPlayer(g_cfg.gameclient_rva);
            float cur = 0, top = 0;
            const bool have_h = me && SimHealthNow(me, &cur, &top) && top > 0;
            if (InterlockedIncrement(&g_counter_logged) <= 12)
              SowLog(L"coop: counter strike on enemy %u: its last swing here %lld ms ago, health then %.0f, now %.0f",
                     kv.first, ls != g_last_swing_by.end() ? (long long)(now - ls->second.at) : -1LL,
                     ls != g_last_swing_by.end() ? ls->second.before : -1.0f, have_h ? cur : -1.0f);
            if (ls != g_last_swing_by.end() && have_h && cooprules::CounterGivesBack(now, ls->second.at, ls->second.before, cur)) {
              SimProxyHealth(me, ls->second.before / top);
              if (InterlockedIncrement(&g_counter_heals) <= 10)
                SowLog(L"coop: this player countered enemy %u: the swing it took %llu ms before is given back (%.0f -> %.0f)",
                       kv.first, (unsigned long long)(now - ls->second.at), cur, ls->second.before);
              g_last_swing_by.erase(ls);
            }
          }
          kv.second.hit_ms = now;  // the game plays its reaction (a counter, a finisher) on this proxy: let it be for a moment
          kv.second.strike_ms = now;
          matched = true;
          break;
        }
      if (!matched) InterlockedIncrement(&g_enemy_hits_unmatched);  // a proxy already freed (dead, waiting to go)
    }
    if (!outbox.empty() && TryEnterCriticalSection(&g_net_lock)) {
      for (const auto& ev : outbox) g_net->sendEvent(ev.data(), ev.size());
      InterlockedAdd(&g_enemy_hits_sent, (LONG)outbox.size());
      outbox.clear();
      LeaveCriticalSection(&g_net_lock);
    }
    if (outbox.size() > 64) outbox.clear();
  }
  unsigned spawns_left = 2;
  for (auto& kv : proxies) {
    sowenemy::Proxy& p = kv.second;
    if (!p.have) continue;
    if (p.handle && !SimProxyValid(p.handle)) {  // freed by the game (unloaded, killed and removed)
      SimProxyDrop(p.handle);
      p.handle = 0;
      p.applied_record = 0;
      p.applied_attack = p.applied_stamp = 0;
      p.applied_loco = 255;
      p.next_spawn_ms = now + 1500;
    }
    if (p.handle && g_cfg.captain_look && !p.member_failed && p.member_id != sowenemy::kNoMember && p.member_id != p.built_member) {
      // the captain behind this enemy was announced after the proxy was built: build it again from his member
      FreeProxy(p);
      p.applied_record = 0;
      p.applied_attack = p.applied_stamp = 0;
      p.applied_loco = 255;
      p.shown_set = false;
      p.next_spawn_ms = now;
    }
    if (!p.handle) {
      if (p.spawn_failed || spawns_left == 0 || now < p.next_spawn_ms || !(p.cur.flags & sowenemy::kAlive) || SimSpawnBusy()) continue;
      --spawns_left;
      SimSpawnRvas rv;
      rv.gameclient_rva = g_cfg.gameclient_rva;
      std::string report;
      uintptr_t obj = 0, member = 0;
      if (g_cfg.captain_look && !p.member_failed && p.member_id != sowenemy::kNoMember) {
        char found[40];
        member = SimFindMember(p.faction_hash, p.member_id, found);
        if (!member || strncmp(found, p.name, sizeof found) != 0) {  // not the same member here: the type's generic orc
          SowLog(L"coop: enemy %u: no member %u named %S in this copy (here: %S)", p.id, p.member_id, p.name, member ? found : "none");
          member = 0;
          p.member_failed = true;
          InterlockedIncrement(&g_enemy_captains_missing);
        }
      }
      bool made = SimSpawn(rv, p.type, p.cur.pos, nullptr, &report, &obj, true, member) && obj && SimProxyAdopt(obj, member == 0);
      if (!made && member) {  // the member could not be spawned: the type's own
        SowLog(L"coop: no proxy for enemy %u (%S) from member %S: %S; trying the type's own", p.id, p.type, p.name, report.c_str());
        p.member_failed = true;
        member = 0;
        report.clear();
        obj = 0;
        made = SimSpawn(rv, p.type, p.cur.pos, nullptr, &report, &obj, true) && obj && SimProxyAdopt(obj);
      }
      if (made) {
        p.handle = obj;
        p.shown_set = false;
        p.built_member = member ? p.member_id : sowenemy::kNoMember;
        InterlockedIncrement(&g_enemy_spawned);
        if (member) InterlockedIncrement(&g_enemy_captains);
      } else {
        p.spawn_failed = true;
        InterlockedIncrement(&g_enemy_spawn_failed);
        SowLog(L"coop: no proxy for enemy %u (%S): %S", p.id, p.type, report.c_str());
      }
      if (!p.handle) continue;
    }
    if (p.built_member != sowenemy::kNoMember) SimMemberLink(p.faction_hash, p.built_member, p.handle, 0);  // the speech's camera looks at it
    float target[3];
    sowenemy::ClientTracker::Predict(p, now, target);
    if (!p.shown_set || DistXZ(p.shown, target) > 400.0f) {
      memcpy(p.shown, target, sizeof p.shown);
      p.shown_set = true;
    } else {
      for (int i = 0; i < 3; ++i) p.shown[i] += (target[i] - p.shown[i]) * 0.5f;
    }
    if (!(p.cur.flags & sowenemy::kAlive)) {
      // Its enemy died on the host: the proxy dies the game's way (the death animation, the body the game removes later) and is
      // left where it fell; health 0 written by hand left standing bodies the game never noticed.
      if (!p.killed) {
        p.killed = true;
        if (p.built_member != sowenemy::kNoMember) {
          SimMemberLink(p.faction_hash, p.built_member, 0, p.handle);
          SimProxyDetachMember(p.handle);
        }
        if (SimKillCharacter(g_cfg.hit_rva, g_cfg.attack_table_rva, p.handle, g_cfg.gameclient_rva, "PC_NT_L1_D1_F_LL_RL_04")) {
          if (p.applied_record) SimProxyStopAttack(p.handle, p.applied_record);
          p.applied_record = 0;
          InterlockedIncrement(&g_enemy_killed);
        } else {
          p.killed = false;
          SimProxyHealth(p.handle, 0.0f);
          InterlockedIncrement(&g_enemy_kill_failed);
        }
      }
      if (p.killed) continue;
    }
    const float h = p.cur.yaw * 0.5f, rot[4] = {0.0f, std::sin(h), 0.0f, std::cos(h)};
    const bool busy = p.hit_ms && now - p.hit_ms < 1800;  // struck by this player a moment ago: the game's reaction plays
    if (!busy) SimProxyMove(g_cfg.teleport_rva, p.handle, p.shown, rot, g_cfg.enemy_direct);
    {
      // Damage done to this proxy here by a way other than the game's hit function (an arrow): the health is lower than what was
      // written last frame. It is not kept (the host's health is written again) but told to the host as a hit.
      float cur = 0, mx = 0;
      // Not within 2.5 s of a reported strike of this player on it (session cm): a counter kill's damage lands frames after the
      // strike is reported, and was told to the host again as 3-4 stock strikes (a non-lethal counter would hit the host's enemy
      // twice).
      const bool after_strike = !cooprules::LocalLossReported(now, p.strike_ms, false);
      // Nor for the host's allies and bystanders (session cm: two tamed wargs and a prisoner lost 60.5 each near the tower, not by
      // the client's hand, and the host was told the client struck them).
      const bool bystander = cooprules::BystanderKind(p.type, sizeof p.type);
      // Nor for a proxy that has just fallen: the host's enemy falls too and loses its own health there.
      float here[3];
      bool fell = false;
      if (ReadFloatsAt(p.handle + 0xAC, here)) {
        fell = cooprules::RecentFall(p.peak_y, p.peak_ms, here[1]);
        cooprules::TrackPeak(&p.peak_y, &p.peak_ms, here[1], now);
      }
      if (after_strike && p.hp_written >= 0.0f && SimHealthNow(p.handle, &cur, &mx) && cur < p.hp_written - 0.5f)
        InterlockedIncrement(&g_enemy_local_after_strike);
      if (bystander && p.hp_written >= 0.0f && SimHealthNow(p.handle, &cur, &mx) && cur < p.hp_written - 0.5f)
        InterlockedIncrement(&g_enemy_local_bystander);
      if (fell && !bystander && p.hp_written >= 0.0f && SimHealthNow(p.handle, &cur, &mx) && cur < p.hp_written - 0.5f)
        InterlockedIncrement(&g_enemy_local_fall);
      if (!after_strike && !bystander && !fell && p.hp_written >= 0.0f && SimHealthNow(p.handle, &cur, &mx) && cur < p.hp_written - 0.5f &&
          p.hit_ms != now) {
        InterlockedIncrement(&g_enemy_local_damage);
        static LONG logged = 0;
        if (logged < 30) {
          ++logged;
          SowLog(L"coop: local damage on proxy %u (%S): health %.1f -> %.1f of %.1f", p.id, p.type, p.hp_written, cur, mx);
        }
        static uint32_t arrow_id = 0;
        if (!arrow_id) arrow_id = SimAttackIdByName("PC_NT_L1_D1_F_LL_RL_04");
        static std::vector<std::vector<uint8_t>> local_out;
        p.hit_ms = now;
        if (g_cfg.exact_losses && g_cfg.local_damage_report) {
          float at[3] = {p.shown[0], p.shown[1], p.shown[2]};
          local_out.push_back(sowenemy::LossEvent(kv.first, p.hp_written - cur, at));
          if (TryEnterCriticalSection(&g_net_lock)) {
            for (const auto& ev : local_out) g_net->sendEvent(ev.data(), ev.size());
            InterlockedAdd(&g_enemy_local_sent, (LONG)local_out.size());
            local_out.clear();
            LeaveCriticalSection(&g_net_lock);
          }
          if (local_out.size() > 32) local_out.clear();
        } else if (arrow_id && g_cfg.local_damage_report) {
          float at[3] = {p.shown[0], p.shown[1], p.shown[2]};
          // The stock strike takes about 96 of an orc's 300: as many of them as the arrow took (at least one, at most six).
          const int strikes = cooprules::StockStrikes(p.hp_written - cur);
          for (int k = 0; k < strikes; ++k) local_out.push_back(sowenemy::HitEvent(kv.first, arrow_id, at));
          if (TryEnterCriticalSection(&g_net_lock)) {
            for (const auto& ev : local_out) g_net->sendEvent(ev.data(), ev.size());
            InterlockedAdd(&g_enemy_local_sent, (LONG)local_out.size());
            local_out.clear();
            LeaveCriticalSection(&g_net_lock);
          }
          if (local_out.size() > 32) local_out.clear();
        }
      }
      const float want = (p.cur.flags & sowenemy::kAlive) ? p.cur.hp / 65535.0f : 0.0f;
      SimProxyHealth(p.handle, want);
      p.hp_written = SimHealthNow(p.handle, &cur, &mx) ? cur : -1.0f;
    }
    if (p.cur.attack_id != p.applied_attack || p.cur.attack_stamp != p.applied_stamp) {
      if (p.applied_record) SimProxyStopAttack(p.handle, p.applied_record);
      p.applied_record = 0;
      if (p.cur.attack_id) {
        p.applied_record = SimProxyAttack(g_cfg.attack_thunk_rva, g_cfg.attack_stop_rva, g_cfg.attack_table_rva, p.handle, p.cur.attack_id,
                                          (p.cur.flags & sowenemy::kTargetsClient) != 0, g_cfg.gameclient_rva);
        InterlockedIncrement(p.applied_record ? &g_enemy_attack_ok : &g_enemy_attack_fail);
        {
          static LONG attack_logged = 0;
          Packet pl{};
          if (attack_logged < 600 && ReadPlayer(&pl)) {
            InterlockedIncrement(&attack_logged);
            SowLog(L"coop: attack %08X of proxy %u aimed at this player=%d, %.0f units away, this player's health %.0f, started=%d", p.cur.attack_id, p.id,
                   (p.cur.flags & sowenemy::kTargetsClient) != 0 ? 1 : 0, DistXZ(p.shown, pl.pos), pl.health, p.applied_record ? 1 : 0);
          }
        }
      }
      if (!p.cur.attack_id) p.applied_loco = 255;  // the attack ended: show how it moves again
      p.applied_attack = p.cur.attack_id;
      p.applied_stamp = p.cur.attack_stamp;
    }
    if (busy) {
      // nothing is played over the game's reaction; what the host does meanwhile is caught up with afterwards
      p.applied_loco = 255;
      p.applied_anim_seq = p.anim_seq;
    } else if (g_cfg.anim_mirror && p.anim_seq != p.applied_anim_seq) {  // the host's enemy started an action: the proxy starts the same
      p.applied_anim_seq = p.anim_seq;
      p.anim_ms = now;
      if (SimProxyPlay(g_cfg.play_rva, p.handle, p.anim)) InterlockedIncrement(&g_enemy_anims_played);
      else InterlockedIncrement(&g_enemy_anims_refused);
    }
    const bool mirrored = g_cfg.anim_mirror && p.anim_ms && now - p.anim_ms < 1500;  // actions are coming: no guessing from the speed
    if (!busy && !mirrored && !p.cur.attack_id && p.cur.loco != p.applied_loco) {
      static const char* const kLoco[] = {"Idle", "WalkForward", "Run"};
      if (p.cur.loco < 3 && SimProxyPlay(g_cfg.play_rva, p.handle, kLoco[p.cur.loco])) {
        p.applied_loco = p.cur.loco;
        InterlockedIncrement(&g_enemy_play_ok);
      } else {
        p.applied_loco = p.cur.loco;  // not played (no such state): do not retry every frame
        InterlockedIncrement(&g_enemy_play_fail);
      }
    }
  }
  if (g_test_dmg_owner) {
    const uintptr_t who = (uintptr_t)InterlockedExchange64(&g_test_dmg_owner, 0);
    float at[3] = {};
    Packet me{};
    if (ReadPlayer(&me)) memcpy(at, me.pos, sizeof at);
    g_test_dmg_result = SimApplyDamage(g_cfg.hit_rva, g_cfg.attack_table_rva, who, at, (uint32_t)g_test_dmg_attack, g_cfg.gameclient_rva) ? 1 : -1;
  }
  // The client's own population: whatever its tree ticks and is not a proxy is asked to go (once it is out of sight).
  static ULONGLONG last_despawn = 0;
  if (now - last_despawn >= 1500) {
    last_despawn = now;
    uintptr_t list[256];
    const size_t n = SimEnemyList(list, 256);
    for (size_t i = 0; i < n; ++i)
      if (SimDespawnNow(g_cfg.despawn_rva, list[i])) InterlockedIncrement(&g_enemy_despawn_asked);
  }
}

// Test aid (probe "coopset keephealth on"): this copy's player is put back at full health every frame. An unattended test run
// left the host's player among orcs between measurements; it died, and the death and army screens stopped its world (session cg).
volatile LONG g_keep_health = 0;

void EnemyFrame() {
  if (g_keep_health) {
    const uintptr_t me = SimLocalPlayer(g_cfg.gameclient_rva);
    if (me) SimProxyHealth(me, 1.0f);
  }
  if (g_cfg.net_role == "host") EnemyHostFrame();
  else if (g_cfg.net_role == "client") EnemyClientFrame();
}

// Fast travel follow (below, IngestPacket): the pending destination.
constexpr float kTravelJump = 2500.0f;
bool g_travel_due = false;
float g_travel_to[3] = {};
ULONGLONG g_travel_at = 0;
volatile LONG g_travels = 0;

// The client: the host jumped (fast travel): this copy's player goes to where the host is, beside it.
void TravelFrame() {
  bool due = false;
  float to[3];
  // Once, when this player is in the world and the host's position is known: a client that starts where the host's
  // save left it, far from where the host is now, is taken to the host.
  static bool join_checked = false;
  if (!join_checked) {
    Packet me{};
    EnterCriticalSection(&g_lock);
    const bool host_there = g_have && (g_last.flags & 1);
    float hp[3] = {g_last.pos[0], g_last.pos[1], g_last.pos[2]};
    LeaveCriticalSection(&g_lock);
    if (host_there && ReadPlayer(&me) && (me.pos[0] != 0.0f || me.pos[2] != 0.0f)) {
      join_checked = true;
      const float dx = me.pos[0] - hp[0], dz = me.pos[2] - hp[2];
      if (std::sqrt(dx * dx + dz * dz) > 1500.0f) {
        EnterCriticalSection(&g_lock);
        memcpy(g_travel_to, hp, sizeof g_travel_to);
        g_travel_at = GetTickCount64();
        g_travel_due = true;
        LeaveCriticalSection(&g_lock);
      }
    }
  }
  // This player's own long jump (the game's fast travel, a respawn) is not the host's: after the load it is taken back
  // to the host, so only the host's travel moves both ("the host decides"). The player's own teleport below is not one.
  static float prev[3] = {};
  static bool have_prev = false;
  static ULONGLONG ignore_until = 0;
  {
    Packet me{};
    if (ReadPlayer(&me) && (me.pos[0] != 0.0f || me.pos[2] != 0.0f)) {
      const ULONGLONG now = GetTickCount64();
      const float dx = me.pos[0] - prev[0], dy = me.pos[1] - prev[1], dz = me.pos[2] - prev[2];
      if (have_prev && now >= ignore_until && std::sqrt(dx * dx + dy * dy + dz * dz) > kTravelJump) {
        EnterCriticalSection(&g_lock);
        if (!g_travel_due && g_have && (g_last.flags & 1)) {
          memcpy(g_travel_to, g_last.pos, sizeof g_travel_to);
          g_travel_at = now + 3000;
          g_travel_due = true;
          SowLog(L"coop: this player travelled alone (%.0f units); back to the host in 3 s", std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        LeaveCriticalSection(&g_lock);
      }
      memcpy(prev, me.pos, sizeof prev);
      have_prev = true;
    }
  }
  EnterCriticalSection(&g_lock);
  if (g_travel_due && GetTickCount64() >= g_travel_at) {
    due = true;
    memcpy(to, g_travel_to, sizeof to);
    g_travel_due = false;
  }
  LeaveCriticalSection(&g_lock);
  if (!due) return;
  to[0] += 100.0f;
  std::string e;
  if (SimRequestPlayerTeleport(g_cfg.teleport_rva, g_cfg.gameclient_rva, to, &e)) {
    ignore_until = GetTickCount64() + 5000;
    InterlockedIncrement(&g_travels);
    SowLog(L"coop: this player is taken to the host at %.0f,%.0f,%.0f", to[0], to[1], to[2]);
  } else {
    SowLog(L"coop: cannot follow the host's travel: %S", e.c_str());
  }
}

// The stand-in's spawn, on the simulation thread (see PickPuppet).
void PuppetSpawnFrame() {
  bool due;
  float pos[3];
  EnterCriticalSection(&g_lock);
  due = g_puppet_spawn_due;
  memcpy(pos, g_puppet_spawn_at, sizeof pos);
  LeaveCriticalSection(&g_lock);
  if (!due || SimHasPuppet() || SimSpawnBusy()) return;
  EnterCriticalSection(&g_lock);
  g_puppet_spawn_due = false;
  LeaveCriticalSection(&g_lock);
  SimSpawnRvas rv;
  rv.gameclient_rva = g_cfg.gameclient_rva;
  std::string r;
  const bool spawned = SimSpawnPuppet(rv, g_cfg.spawn_type.c_str(), pos, &r, true);
  if (!spawned) ++g_puppet_spawn_tries;
  SowLog(L"coop: %S%S", spawned ? "" : "no spawned puppet yet: ", r.c_str());
}

// The simulation thread, once per frame: send this copy's player, move the puppet.
bool g_slowmo_requested = false;
bool g_unlink_requested = false;

void CoopFrame() {
  if ((g_cfg.send_port || !g_cfg.net_role.empty()) && !g_inputs_requested) {
    g_inputs_requested = true;
    std::string e;
    if (!SimAnimInputsInstall(g_cfg.anim_var_rva, g_cfg.gameclient_rva, &e))
      SowLog(L"coop: the player's animation inputs are not sent: %S", e.c_str());
  }
  if (!g_cfg.net_role.empty() && g_cfg.slowmo_off && !g_slowmo_requested) {
    g_slowmo_requested = true;
    std::string e;
    if (!SimSlowMoOff(g_cfg.slowmo_rva, &e)) SowLog(L"coop: slow motion stays on: %S", e.c_str());
  }
  if ((g_cfg.send_port && g_send_socket != INVALID_SOCKET) || g_net) SendSample();
  if (g_cfg.net_role == "client" && !g_cfg.slowmo_off) SimSlowMoCap(g_cfg.slowmo_request_rva, g_cfg.slowmo_cap_ms);
  SimApplyRetargets();
  if (!g_cfg.net_role.empty() && !g_unlink_requested) {  // on this (the simulation) thread: see SimUnlinkFixInstall
    g_unlink_requested = true;
    SimRetargetRefs(g_cfg.retarget_refs);
    std::string e;
    if (g_cfg.unlink_fix && !SimUnlinkFixInstall(&e)) SowLog(L"coop: the game's list destructor can still hang: %S", e.c_str());
    if (g_cfg.ai_param_guard && !SimAiParamGuardInstall(&e)) SowLog(L"coop: the AI's attack-parameter lookup is not guarded: %S", e.c_str());
    if (g_cfg.ai_param_guard && !SimAiTickGuardInstall(&e)) SowLog(L"coop: the AI tick's look at a component's owner is not guarded: %S", e.c_str());
    if (!g_cfg.audio_keys_fatal) {
      if (SimAudioKeysNonFatal(&e)) SowLog(L"coop: a full audio key queue no longer crashes the game (GameAudioManager.FatalErrorOnFutureKeyFull off)");
      else SowLog(L"coop: a full audio key queue still crashes the game: %S", e.c_str());
    }
  }
  if (g_cfg.net_role == "client" && g_cfg.follow_travel) TravelFrame();
  if (g_cfg.puppet && !g_cfg.spawn_type.empty()) PuppetSpawnFrame();
  if (g_cfg.nemesis && g_net && (g_cfg.net_role == "host" || g_cfg.net_role == "client")) NemesisFrame();
  if (g_cfg.enemies && g_net) EnemyFrame();
  if (!g_cfg.puppet) return;
  if (!g_cfg.puppet_ai && !g_ai_requested) {
    g_ai_requested = true;
    std::string e;
    if (SimRequestBrainHook(g_cfg.brain_site_rva, g_cfg.tree_rva, &e)) SimSetPuppetAi(false);
    else SowLog(L"coop: puppet AI stays on: %S", e.c_str());
  }
  if (!g_target_requested) {
    g_target_requested = true;
    std::string e;
    if (!SimAttackHooksInstall(g_cfg.target_check_rva, g_cfg.hit_rva, g_cfg.aoe_rva, &e))
      SowLog(L"coop: attacks can still pick and hit the puppet: %S", e.c_str());
  }
  if (!g_reset_requested) {
    g_reset_requested = true;
    std::string e;
    if (!SimAnimResetHookInstall(g_cfg.anim_reset_rva, &e))
      SowLog(L"coop: put-backs restart the puppet's animation: %S", e.c_str());
  }
  const int follow = g_follow;
  // The game frees a spawned stand-in it unloads (seen when this copy's player went ~3000
  // units away from it), and this copy's player can kill it (session al): drop it, so
  // PickPuppet makes a new one.
  SimForgetGonePuppet();
  if (!g_cfg.net_role.empty()) ProxyTableFrame(GetTickCount64());  // both roles: the stand-in; the client: its proxies
  SimKeepPuppetUntargeted(g_targetable_on == 0);
  SimKeepPuppetAnimOnTeleport(g_resets_on == 0);
  if (!SimHasPuppet()) return;
  // Out of the local player's combat: a stranger beside it puts the player in combat stance.
  {
    std::string e;
    if (!SimKeepPuppetFollower(g_cfg.follower_rva, g_follower_on != 0, &e) && e != g_follower_err) {
      SowLog(L"coop: the puppet is not marked as the player's follower: %S", e.c_str());
      g_follower_err = e;
    }
  }
  float pos[3], rot[4], prev[3], prev_rot[4], inputs[kAnimInputs], root_speed = -1;
  SimAction actions[kMaxActions];
  int action_count = 0;
  uint32_t input_mask = 0;
  EnterCriticalSection(&g_lock);
  const uint64_t t = (uint64_t)((int64_t)NowUs() - g_offset_us) - (uint64_t)g_cfg.delay_ms * 1000;
  const bool ok = g_have_offset && Interpolate(t, pos, rot) && Interpolate(t - 200000, prev, prev_rot);
  if (ok && g_inputs_on) input_mask = InputsAt(t, inputs, &root_speed, actions, &action_count);
  const bool off_ground = ok && OffGroundAt(t);
  uint32_t attack_id = 0, attack_stamp = 0;
  if (ok) AttackAt(t, &attack_id, &attack_stamp);
  LeaveCriticalSection(&g_lock);
  // The sender's attacks: the stand-in gets no attack input of its own (session aq), so each one
  // the sender starts is started on it through the game's own start, and stopped when the
  // sender's ends.
  if (ok && g_attacks_on) {
    std::string e;
    if (!SimFollowAttack(g_cfg.attack_thunk_rva, g_cfg.attack_stop_rva, g_cfg.attack_table_rva, attack_id, attack_stamp,
                         &e)) {
      if (e != g_attack_err) SowLog(L"coop: the puppet does not play the sender's attack: %S", e.c_str());
      g_attack_err = e;
    } else if (!g_attack_logged && SimAttacksStarted() > 0) {
      SowLog(L"coop: the puppet plays the sender's attacks");
      g_attack_logged = true;
    }
  }
  // Anim mode without the sender's buttons: the puppet's root motion keeps the sender's speed
  // (its graph walks where the sender free-runs). With the buttons it free-runs itself, and
  // scaling it as well made turns worse (session aj).
  const bool speed = ok && input_mask && follow == 4 && g_speed_on && !g_actions_on;
  SimSetPuppetRootSpeed(g_cfg.mover_step_rva, speed ? root_speed : -1.0f, speed ? RootCatchUp(pos) : 0.0f);
  if (!ok) {
    InterlockedIncrement(&g_frames_not_ok);
    return;
  }
  // The sender's animation inputs drive the puppet's graph (a Talion stand-in has the
  // player's graph): it walks, runs and turns with the sender's own gait. With follow
  // "none" they are all that moves it (its own root motion: probe "puppet follow none").
  if (input_mask) {
    std::string e;
    if (SimApplyPuppetInputs(g_cfg.anim_var_rva, inputs, input_mask, &e)) {
      if (InterlockedIncrement(&g_inputs_given) == 1)
        SowLog(L"coop: the puppet gets the sender's animation inputs (mask %08X)", input_mask);
    } else if (!e.empty() && e != g_inputs_err) {
      SowLog(L"coop: the puppet gets no animation inputs: %S", e.c_str());
      g_inputs_err = e;
    }
  }
  // And the sender's buttons (game actions), so a stand-in with the player's graph free-runs
  // when the sender does instead of walking.
  if (input_mask && g_actions_on) {
    std::string e;
    if (SimApplyPuppetActions(actions, action_count, &e)) {
      if (SimPuppetActionFrames() == 1) SowLog(L"coop: the puppet gets the sender's buttons (game actions)");
    } else if (e != g_actions_err) {
      SowLog(L"coop: the puppet gets no buttons: %S", e.c_str());
      g_actions_err = e;
    }
  } else {
    SimReleasePuppetActions();
  }
  if (follow == 0) return;
  if (follow == 4) {
    // In the air the sender is moved by the jump itself, not by its animation (an assisted
    // running jump throws it forward at 1000+ units/s): the puppet, whose graph plays the
    // same jump from the buttons, stayed on the ledge. Until the sender lands, the puppet's
    // own movement step is skipped and its transform is set to the sender's every frame (the
    // game's teleport would put it on the ground below); on landing one teleport puts it
    // back on the ground there and its own movement takes over again (session an). The
    // same on a wall: a climb's modes are 2, 3, 4 and 1, but its on-ground flag stays 0
    // (session ao).
    const bool air = off_ground && g_air_on;
    SimAssistJump(air);  // before the sender's air flag arrives: the approach chain takes ~30 ms
    SimSkipPuppetStep(g_cfg.mover_step_rva, air);  // not held during the approach: it stood still while the sender ran to the ledge
    if (air || g_was_air) {
      std::string err;
      if (SimRequestPuppetMove(g_cfg.teleport_rva, pos, rot, &err, air))
        InterlockedIncrement(air ? &g_air_frames : &g_air_landings);
      else if (err != g_puppet_err) SowLog(L"coop: cannot move the puppet in the air: %S", err.c_str());
      g_puppet_err = err;
      g_was_air = air;
      return;
    }
    AnimFrame(pos, rot, prev, (input_mask & 1) && inputs[0] < 0.05f);  // inputs[0]: RawMoveMag
    if (InterlockedIncrement(&g_frames_logged) == 1)
      SowLog(L"coop: puppet moved by the sender's animation inputs, %u ms behind, put back when it drifts",
             g_cfg.delay_ms);
    return;
  }
  if (follow >= 2) {
    WalkFrame(pos, rot, prev, follow == 3);
    if (InterlockedIncrement(&g_frames_logged) == 1)
      SowLog(L"coop: puppet walks after the sender with its own locomotion, %u ms behind", g_cfg.delay_ms);
    return;
  }
  std::string err;
  if (!SimRequestPuppetMove(g_cfg.teleport_rva, pos, rot, &err) && err != g_puppet_err)
    SowLog(L"coop: cannot move the puppet: %S", err.c_str());
  g_puppet_err = err;
  if (err.empty() && InterlockedIncrement(&g_frames_logged) == 1)
    SowLog(L"coop: puppet moved every frame, %u ms behind the sender, interpolated", g_cfg.delay_ms);
}

// A received player state, from either transport (the loopback pair or the host/client link).
// Fast travel (and a respawn) show as a jump of the host's player: no walk, ride or fall covers this much in the time
// between two of its states. The client follows (it is moved to the host's new place once the host has been there for a
// moment), so "the host decides, both go". Only a client does, only for packets that carry a player.

void IngestPacket(const Packet& p, uint64_t recv_us, unsigned from_port) {
  EnterCriticalSection(&g_lock);
  const ULONGLONG now = GetTickCount64();
  if (g_cfg.net_role == "client" && g_cfg.follow_travel && g_have && (g_last.flags & 1) && (p.flags & 1)) {
    const float dx = p.pos[0] - g_last.pos[0], dy = p.pos[1] - g_last.pos[1], dz = p.pos[2] - g_last.pos[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kTravelJump) {
      memcpy(g_travel_to, p.pos, sizeof g_travel_to);
      g_travel_at = now + 1500;
      g_travel_due = true;
    } else if (g_travel_due) {
      memcpy(g_travel_to, p.pos, sizeof g_travel_to);  // still settling: follow where it stands now
    }
  }
  if (g_have && p.seq > g_last.seq + 1) g_lost += p.seq - g_last.seq - 1;
  if (!g_have) g_first_at = now;
  g_last = p;
  g_last_at = now;
  g_have = true;
  ++g_packets;
  Remember(p, recv_us);
  LeaveCriticalSection(&g_lock);
  static bool logged_first = false;
  if (!logged_first) {
    SowLog(L"coop: first packet (seq %u) from port %u", p.seq, from_port);
    logged_first = true;
  }
  MaybeShow(p);
}

DWORD WINAPI ListenThread(void*) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in at = Loopback(g_cfg.listen_port);
  if (s == INVALID_SOCKET || bind(s, reinterpret_cast<const sockaddr*>(&at), sizeof at) != 0) {
    SowLog(L"coop: cannot listen on 127.0.0.1:%u (%d)", g_cfg.listen_port, WSAGetLastError());
    return 0;
  }
  SowLog(L"coop: listening on 127.0.0.1:%u%s", g_cfg.listen_port,
         g_cfg.puppet ? L", moving a puppet" : g_cfg.show ? L", showing the remote player" : L"");
  char buf[sizeof(Packet) + 64];  // a larger datagram is cut short, so it is counted as bad
  bool logged_first = false;
  for (;;) {
    sockaddr_in from{};
    int from_len = sizeof from;
    int n = recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) {
      Sleep(10);
      continue;
    }
    const uint64_t recv_us = NowUs();
    Packet p{};
    if (n == (int)sizeof p) memcpy(&p, buf, sizeof p);
    const bool good = n == (int)sizeof p && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
                      memcmp(p.magic, "SOWC", 4) == 0 && p.version == kVersion;
    EnterCriticalSection(&g_lock);
    if (!good) {
      ++g_bad;
      LeaveCriticalSection(&g_lock);
      continue;
    }
    LeaveCriticalSection(&g_lock);
    (void)logged_first;
    IngestPacket(p, recv_us, ntohs(from.sin_port));
  }
}

// ---- the host/client transport ----------------------------------------------------------------------------

SOCKET g_net_sock = INVALID_SOCKET;
sockaddr_in g_net_peer{};  // the client's host

uint64_t PackAddr(const sockaddr_in& a) { return ((uint64_t)ntohl(a.sin_addr.s_addr) << 16) | ntohs(a.sin_port); }
sockaddr_in UnpackAddr(uint64_t k) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl((uint32_t)(k >> 16));
  a.sin_port = htons((u_short)(k & 0xFFFF));
  return a;
}

bool ParseHostPort(const std::string& s, sockaddr_in* out) {
  const size_t colon = s.rfind(':');
  if (colon == std::string::npos) return false;
  const unsigned long port = strtoul(s.c_str() + colon + 1, nullptr, 10);
  if (port == 0 || port > 65535) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((u_short)port);
  if (inet_pton(AF_INET, s.substr(0, colon).c_str(), &a.sin_addr) != 1) return false;
  *out = a;
  return true;
}

const wchar_t* NetStatusName(sownet::Status s) {
  switch (s) {
    case sownet::Status::kIdle: return L"waiting";
    case sownet::Status::kConnecting: return L"connecting";
    case sownet::Status::kSyncing: return L"syncing";
    case sownet::Status::kConnected: return L"connected";
    case sownet::Status::kLost: return L"lost";
    case sownet::Status::kRejected: return L"rejected";
    default: return L"closed";
  }
}

// ---- joining: the client plays in the host's save ----------------------------------------------------------
// The client's mod connects as soon as it starts. The game's first look at its save in the sandbox cloud (a gate
// in steamguard.cpp) waits until the host's save has arrived and checked out, then puts it there under the name
// the game asks for, and replaces the local copy too. A join that fails (no host, wrong password or build, no
// save, a damaged or interrupted transfer) ends the process: a client that went on with its own save would play
// in a different world from the host's.

bool g_join_active = false;
HANDLE g_join_event = nullptr;
volatile LONG g_join_state = 0;  // 0 waiting, 1 the host's save is in hand
ULONGLONG g_join_deadline = 0;
std::vector<uint8_t> g_join_save;

[[noreturn]] void JoinFail(const wchar_t* why) {
  SowLog(L"coop: cannot join: %s", why);
  if (g_cfg.join_message) {
    wchar_t text[700];
    swprintf_s(text, L"Co-op: %s\n\nThe game will close.", why);
    MessageBoxW(nullptr, text, L"Shadow of War co-op", MB_OK | MB_ICONERROR | MB_TOPMOST);
  }
  SowFatal(kExitCoopJoin, L"coop: cannot join: %s", why);
}

const wchar_t* RejectText(uint8_t reason) {
  switch (reason) {
    case sownet::kRejectProto: return L"The host runs a different game build or co-op protocol.";
    case sownet::kRejectBuild: return L"The host runs a different version of this mod.";
    case sownet::kRejectPassword: return L"The co-op password is wrong.";
    case sownet::kRejectBusy: return L"The host already has a player.";
    case sownet::kRejectSave: return L"The host's save could not be received (too big, or damaged on the way).";
    default: return L"The host refused the connection.";
  }
}

// Called by the net thread: moves the join along, or ends it.
void JoinTick() {
  if (!g_join_active || g_join_state != 0) return;
  EnterCriticalSection(&g_net_lock);
  const sownet::Status st = g_net->status();
  const uint8_t reason = g_net->rejectReason();
  const size_t want = g_net->saveSize();
  if (st == sownet::Status::kConnected && g_net->receivedSave().size() == want) g_join_save = g_net->receivedSave();
  LeaveCriticalSection(&g_net_lock);
  if (st == sownet::Status::kRejected) JoinFail(RejectText(reason));
  if (st == sownet::Status::kClosed) JoinFail(L"The host closed the connection.");
  if (st == sownet::Status::kConnected) {
    if (want == 0)
      JoinFail(L"The host has no save to share. Start the game once on the host, save, and try again.");
    if (g_join_save.size() != want) JoinFail(L"The host's save did not arrive complete.");
    InterlockedExchange(&g_join_state, 1);
    SetEvent(g_join_event);
    return;
  }
  if (GetTickCount64() > g_join_deadline)
    JoinFail(st == sownet::Status::kSyncing ? L"The host's save did not arrive in time."
              : reason == sownet::kRejectBusy ? RejectText(reason)
                                              : L"The host did not answer (is it running, and is the address and port right?).");
}

// The steamguard gate: the game is about to look at its save.
void JoinGate(const char* name) {
  if (!g_join_active) return;
  SowLog(L"coop: the game asks for its save (%S); waiting for the host's", name);
  if (WaitForSingleObject(g_join_event, 10 * 60 * 1000) != WAIT_OBJECT_0) JoinFail(L"The host's save never arrived.");
  if (!SteamCloudPlace(name, g_join_save.data(), g_join_save.size()))
    JoinFail(L"The host's save could not be written into this copy's save folder.");
  if (!g_cfg.save_local_dir.empty()) {
    const char* base = name;
    for (const char* p = name; *p; ++p)
      if (*p == '\\' || *p == '/') base = p + 1;
    std::wstring path = g_cfg.save_local_dir + L"\\";
    for (const char* p = base; *p; ++p) path += (wchar_t)*p;  // SowIsSaveName: hex digits and a fixed suffix
    const std::wstring tmp = path + L".sowtmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    bool ok = f != INVALID_HANDLE_VALUE &&
              WriteFile(f, g_join_save.data(), (DWORD)g_join_save.size(), &wrote, nullptr) && wrote == g_join_save.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
      DeleteFileW(tmp.c_str());
      JoinFail(L"The host's save could not be written into this copy's save folder.");
    }
  }
  uint8_t sha[32];
  sownet::Sha256Of(g_join_save.data(), g_join_save.size(), sha);
  SowLog(L"coop: joined: the host's save (%u bytes, sha256 %02x%02x%02x%02x...) is in place as %S",
         (unsigned)g_join_save.size(), sha[0], sha[1], sha[2], sha[3], name);
  std::vector<uint8_t>().swap(g_join_save);
}

// The host: offers its newest main save to a joining client. Re-read when it changes while nobody is joined.
FILETIME g_offered_time{};
unsigned long long g_offered_size = ~0ull;
ULONGLONG g_offer_checked = 0;

void HostOfferSave() {
  if (g_cfg.net_role != "host" || g_cfg.save_dir.empty()) return;
  const ULONGLONG now = GetTickCount64();
  if (g_offer_checked && now - g_offer_checked < 2000) return;
  g_offer_checked = now;
  EnterCriticalSection(&g_net_lock);
  const sownet::Status st = g_net->status();
  LeaveCriticalSection(&g_net_lock);
  if (st == sownet::Status::kSyncing || st == sownet::Status::kConnected) return;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_cfg.save_dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  WIN32_FIND_DATAW best{};
  bool have = false;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    char narrow[MAX_PATH];
    size_t n = 0;
    for (; fd.cFileName[n] && n + 1 < sizeof narrow; ++n) narrow[n] = fd.cFileName[n] < 128 ? (char)fd.cFileName[n] : '?';
    narrow[n] = 0;
    if (!SowIsSaveName(narrow)) continue;
    if (!have || CompareFileTime(&fd.ftLastWriteTime, &best.ftLastWriteTime) > 0) {
      best = fd;
      have = true;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  if (!have) {
    if (g_offered_size != ~0ull - 1) SowLog(L"coop: no save of the host's to offer yet (%s)", g_cfg.save_dir.c_str());
    g_offered_size = ~0ull - 1;
    return;
  }
  const unsigned long long size = ((unsigned long long)best.nFileSizeHigh << 32) | best.nFileSizeLow;
  if (size == g_offered_size && CompareFileTime(&best.ftLastWriteTime, &g_offered_time) == 0) return;
  if (size == 0 || size > sownet::kMaxSave) return;
  HANDLE f = CreateFileW((g_cfg.save_dir + L"\\" + best.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  std::vector<uint8_t> data((size_t)size);
  DWORD got = 0;
  const bool ok = ReadFile(f, data.data(), (DWORD)size, &got, nullptr) && got == size;
  CloseHandle(f);
  if (!ok) return;
  EnterCriticalSection(&g_net_lock);
  g_net->offerSave(std::move(data));
  LeaveCriticalSection(&g_net_lock);
  g_offered_time = best.ftLastWriteTime;
  g_offered_size = size;
  SowLog(L"coop: offering the host's save %s (%llu bytes) to a joining client", best.cFileName, size);
}

DWORD WINAPI NetThread(void*) {
  char buf[sownet::kMaxDatagram + 64];
  for (;;) {
    bool any = false;
    for (int i = 0; i < 64; ++i) {
      sockaddr_in from{};
      int from_len = sizeof from;
      const int n = recvfrom(g_net_sock, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        InterlockedIncrement(&g_net_recv_errors);
        continue;  // an error of an earlier send, not the end of the queue
      }
      if (n <= 0) break;
      any = true;
      EnterCriticalSection(&g_net_lock);
      g_net->receive(reinterpret_cast<const uint8_t*>(buf), (size_t)n, NowUs() / 1000, PackAddr(from));
      LeaveCriticalSection(&g_net_lock);
    }
    EnterCriticalSection(&g_net_lock);
    g_net->tick(NowUs() / 1000);
    LeaveCriticalSection(&g_net_lock);
    JoinTick();
    HostOfferSave();
    if (!any) Sleep(1);
  }
}

// Starts the transport for coop_role=host|client. False (and logged) when it cannot start.
bool StartNet(const CoopConfig& cfg) {
  const bool host = cfg.net_role == "host";
  sownet::Config nc;
  nc.role = host ? sownet::Role::kHost : sownet::Role::kClient;
  for (size_t i = 0; i < 32 && cfg.exe_sha_hex.size() >= 64; ++i)
    nc.exe_sha[i] = (uint8_t)strtoul(cfg.exe_sha_hex.substr(2 * i, 2).c_str(), nullptr, 16);
  nc.mod_build = kVersion;
  nc.password = cfg.net_password;
  nc.session_seed = (uint32_t)(NowUs() ^ GetCurrentProcessId());
  sockaddr_in bind_to{};
  bind_to.sin_family = AF_INET;
  if (host) {
    if (inet_pton(AF_INET, cfg.net_bind.c_str(), &bind_to.sin_addr) != 1) {
      SowLog(L"coop: coop_bind %S is not an IPv4 address; coop off", cfg.net_bind.c_str());
      return false;
    }
    bind_to.sin_port = htons((u_short)cfg.net_port);
  } else if (!ParseHostPort(cfg.net_peer, &g_net_peer)) {
    SowLog(L"coop: coop_host %S is not <ipv4>:<port>; coop off", cfg.net_peer.c_str());
    return false;
  } else {
    bind_to.sin_addr.s_addr = htonl(INADDR_ANY);  // the client's own port is chosen by the system
  }
  g_net_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  u_long nonblocking = 1;
  // Windows reports an ICMP "port unreachable" (a datagram sent to a peer that is gone) as an error of the next
  // recvfrom, one per datagram sent: a host sending a save to a client that died flooded its own socket with them and the
  // next client's HELLOs waited behind (session bo). Off, as the hub's relay does.
  if (g_net_sock != INVALID_SOCKET) {
    DWORD ignored = 0;
    BOOL no_reset = FALSE;
    WSAIoctl(g_net_sock, (DWORD)0x9800000C /* SIO_UDP_CONNRESET */, &no_reset, sizeof no_reset, nullptr, 0, &ignored, nullptr, nullptr);
  }
  if (g_net_sock == INVALID_SOCKET || ioctlsocket(g_net_sock, FIONBIO, &nonblocking) != 0 ||
      bind(g_net_sock, reinterpret_cast<const sockaddr*>(&bind_to), sizeof bind_to) != 0) {
    SowLog(L"coop: cannot open the %S socket (%d); coop off", cfg.net_role.c_str(), WSAGetLastError());
    return false;
  }
  // network=coop_peer: this socket, and only it, may send to the peer (a host: to whoever its valid packets came from).
  SowAllowPeerSocket((uintptr_t)g_net_sock, host ? 0 : g_net_peer.sin_addr.s_addr, host ? 0 : ntohs(g_net_peer.sin_port), host);
  InitializeCriticalSection(&g_net_lock);
  g_net = new sownet::Endpoint(nc, [host](const uint8_t* d, size_t n, uint64_t dst) {
    const sockaddr_in to = host ? UnpackAddr(dst) : g_net_peer;
    sendto(g_net_sock, reinterpret_cast<const char*>(d), (int)n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
  });
  g_net->onState([](const uint8_t* p, size_t n) {
    Packet pk{};
    if (n != sizeof pk) return;
    memcpy(&pk, p, sizeof pk);
    if (memcmp(pk.magic, "SOWC", 4) != 0 || pk.version != kVersion) return;
    IngestPacket(pk, NowUs(), 0);
  });
  g_net->onEvent([](const uint8_t* p, size_t n) { QueueEnemyEvent(p, n); });
  g_net->onState2([](const uint8_t* p, size_t n) { QueueEnemySnapshot(p, n); });
  g_net->onStatus([](sownet::Status s) { SowLog(L"coop: connection %s", NetStatusName(s)); });
  if (host) {
    SowLog(L"coop: hosting on %S:%u", cfg.net_bind.c_str(), cfg.net_port);
    // The addresses this PC has (a VPN adapter such as Tailscale or ZeroTier shows up here too): the friend's
    // coop_host is one of them plus the port.
    char name[256] = {};
    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof name) == 0 && getaddrinfo(name, nullptr, &hints, &list) == 0) {
      for (addrinfo* a = list; a; a = a->ai_next) {
        char text[32] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, text, sizeof text);
        if (strncmp(text, "127.", 4) != 0 && strncmp(text, "169.254.", 8) != 0)
          SowLog(L"coop: a client on this network or VPN joins with coop_host=%S:%u", text, cfg.net_port);
      }
      freeaddrinfo(list);
    }
    if (cfg.save_dir.empty()) SowLog(L"coop: no sandbox cloud folder (steam_cloud=sandbox): this host offers no save");
    HostOfferSave();
  } else {
    SowLog(L"coop: joining %S", cfg.net_peer.c_str());
    if (cfg.join && !cfg.save_dir.empty()) {
      g_join_active = true;
      g_join_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      g_join_deadline = GetTickCount64() + (ULONGLONG)cfg.join_timeout_sec * 1000;
      SteamSetBeforeSaveRead(&JoinGate);
      SowLog(L"coop: this client plays in the host's save: it is taken before the game reads its own (%u s allowed)",
             cfg.join_timeout_sec);
    } else {
      SowLog(L"coop: this client keeps its own save (coop_join=off)");
    }
  }
  HANDLE t = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  return true;
}

}  // namespace

void StartCoop(const CoopConfig& cfg) {
  g_cfg = cfg;
  InitializeCriticalSection(&g_lock);
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    SowLog(L"coop: WSAStartup failed; coop off");
    return;
  }
  g_started = true;
  if (!cfg.net_role.empty()) {
    if (!StartNet(cfg)) return;
  }
  if (cfg.send_port) {
    g_send_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_send_socket == INVALID_SOCKET) {
      SowLog(L"coop: send socket failed (%d)", WSAGetLastError());
    } else {
      SowLog(L"coop: sending this copy's player to 127.0.0.1:%u (%u times a second until the game's frames take over)",
             cfg.send_port, cfg.rate_hz);
      HANDLE t = CreateThread(nullptr, 0, SendThread, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
    }
  }
  if (cfg.listen_port) {
    HANDLE t = CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  g_follow = cfg.follow;
  g_inputs_on = cfg.puppet_inputs ? 1 : 0;
  g_speed_on = cfg.anim_speed ? 1 : 0;
  g_actions_on = cfg.puppet_actions ? 1 : 0;
  g_follower_on = cfg.puppet_follower ? 1 : 0;
  g_targetable_on = cfg.puppet_targetable ? 1 : 0;
  g_air_on = cfg.air_follow ? 1 : 0;
  g_attacks_on = cfg.puppet_attacks ? 1 : 0;
  g_anim_snap = cfg.anim_snap;
  if (cfg.send_port || cfg.puppet || !cfg.net_role.empty()) SimSetFrameCallback(&CoopFrame);
}

std::string CoopTestStandinHit(uintptr_t enemy, uint32_t attack_id) {
  g_test_dmg_attack = (LONG)attack_id;
  g_test_standin_result = 0;
  InterlockedExchange64(&g_test_standin_owner, (LONG64)enemy);
  for (int i = 0; i < 100 && !g_test_standin_result; ++i) Sleep(20);
  return g_test_standin_result == 1 ? "ok the swing was made\n" : "err not made\n";
}

std::string CoopSet(const std::string& key, const std::string& value) {
  if (key == "decoy" && (value == "on" || value == "off")) g_cfg.decoy = value == "on";
  else if (key == "hunt" && (value == "on" || value == "off")) g_cfg.hunt_retarget = value == "on";
  else if (key == "wake") g_cfg.wake_radius = (float)atof(value.c_str());
  else if (key == "hitlog") SimHitLog(atol(value.c_str()));
  else if (key == "stuck") g_cfg.stuck_sec = atoi(value.c_str());
  else if (key == "decoystall") g_cfg.decoy_stall = atoi(value.c_str());
  else if (key == "despawnonce" && (value == "on" || value == "off")) SimDespawnOnce(value == "on");
  else if (key == "retargetrefs" && (value == "on" || value == "off")) SimRetargetRefs(value == "on");
  else if (key == "decoylimbo" && (value == "on" || value == "off")) g_cfg.decoy_limbo = value == "on";
  else if (key == "dmgborrow" && (value == "on" || value == "off")) g_cfg.dmg_borrow = value == "on";
  else if (key == "decoyfollow" && (value == "remote" || value == "standin")) g_cfg.decoy_follow_remote = value == "remote";
  else if (key == "reanchor") g_cfg.decoy_reanchor = (float)atof(value.c_str());
  else if (key == "remake") g_cfg.decoy_remake = (float)atof(value.c_str());
  else if (key == "decoystill") g_cfg.decoy_still = (float)atof(value.c_str());
  else if (key == "proxytablemax") g_cfg.proxy_table_max = atoi(value.c_str());
  else if (key == "huntstrict" && (value == "on" || value == "off")) g_cfg.hunt_strict = value == "on";
  else if (key == "decoyhide" && (value == "on" || value == "off")) InterlockedExchange(&g_decoy_hide, value == "on" ? 1 : 0);
  else if (key == "keephealth" && (value == "on" || value == "off")) InterlockedExchange(&g_keep_health, value == "on" ? 1 : 0);
  else if (key == "hunttoplayer" && (value == "on" || value == "off")) g_cfg.hunt_to_player = value == "on";
  else if (key == "react" && (value == "on" || value == "off")) {
    g_cfg.proxy_react = value == "on";
    SimProxyReact(g_cfg.proxy_react);
  }
  else if (key == "hitby" && (value == "decoy" || value == "poke" || value == "player"))
    g_cfg.hit_by = value == "decoy" ? 1 : value == "poke" ? 2 : 0;
  else if (key == "standinhits" && (value == "on" || value == "off")) {
    g_cfg.standin_hits = value == "on";
    SimDropStandinHits(!g_cfg.standin_hits);
  }
  else
    return "err usage: coopset decoy|decoyhide|hunt|huntstrict|react|standinhits|keephealth on|off, coopset hitby decoy|poke|player, coopset wake <radius>, coopset stuck <seconds>, coopset reanchor <units>, "
           "coopset hitlog <hits>" + std::string(1, (char)10);
  return "ok " + key + "=" + value + std::string(1, (char)10);
}

std::string CoopTestKill(uintptr_t who, uintptr_t by) {
  auto result = std::make_shared<std::string>();
  const bool ran = SimCall(
      [result, who, by]() {
        *result = SimKillCharacter(g_cfg.hit_rva, g_cfg.attack_table_rva, who, g_cfg.gameclient_rva, "PC_NT_L1_D1_F_LL_RL_04", by) ? "ok killed" : "err not killed";
      },
      5000);
  return (ran ? *result : std::string("err not run in time")) + "\n";
}

std::string CoopTestDamage(uintptr_t proxy, uint32_t attack_id) {
  g_test_dmg_attack = (LONG)attack_id;
  g_test_dmg_result = 0;
  InterlockedExchange64(&g_test_dmg_owner, (LONG64)proxy);
  for (int i = 0; i < 100 && !g_test_dmg_result; ++i) Sleep(20);
  return g_test_dmg_result == 1 ? "ok applied\n" : g_test_dmg_result == -1 ? "err the hit was not applied\n" : "err not run (no enemy sharing on this client?)\n";
}

std::string CoopDamageLog() { return DamageLogText(); }

// Test (client): a host damage message for enemy `id` (one with no proxy here, for the dropped-hit fallback) is queued as if it
// had come from the host, at this player's place; the frame handles it like any other.
std::string CoopTestInjectDamage(uint32_t id, uint32_t attack_id) {
  if (g_cfg.net_role != "client") return "err only on a client\n";
  Packet me{};
  if (!ReadPlayer(&me)) return "err no player\n";
  const std::vector<uint8_t> ev = sowenemy::DamageEvent(id, attack_id, me.pos);
  QueueEnemyEvent(ev.data(), ev.size());
  return "ok queued\n";
}

std::string CoopEnemyStatus() {
  char b[2400];
  long ul_installed = 0, ul_repaired = 0, ul_left = 0, ul_faults = 0, rt_refs = 0, rt_raw = 0;
  SimRetargetRefStats(&rt_refs, &rt_raw);
  long ml_assigned = 0, ml_raw = 0;
  SimMemberLinkStats(&ml_assigned, &ml_raw);
  SimUnlinkStats(&ul_installed, &ul_repaired, &ul_left, &ul_faults);
  long ap_installed = 0, ap_guarded = 0, at_installed = 0, at_skipped = 0;
  SimAiParamStats(&ap_installed, &ap_guarded);
  SimAiTickStats(&at_installed, &at_skipped);
  long dsp_calls = 0, dsp_repeats = 0, dsp_skipped = 0, dsp_faults = 0, dsp_repeat_faults = 0, dsp_refused = 0;
  SimDespawnStats(&dsp_calls, &dsp_repeats, &dsp_skipped, &dsp_faults, &dsp_repeat_faults, &dsp_refused);
  size_t spawned_now = 0, with_pos = 0;
  if (g_cfg.net_role == "client")
    for (auto& kv : g_enemy_client.proxies()) {
      if (kv.second.handle) ++spawned_now;
      if (kv.second.have) ++with_pos;
    }
  snprintf(b, sizeof b,
           "ok enemies=%s role=%s host_known=%zu host_announced=%ld host_snapshots=%ld client_proxies=%zu with_pos=%zu "
           "standing=%zu spawned=%ld failed=%ld despawn_asked=%ld hits_sent=%ld hits_on_proxies=%ld hits_applied=%ld hits_failed=%ld hits_stale=%ld travels=%ld hits_unmatched=%ld dmg_sent=%ld dmg_applied=%ld dmg_failed=%ld dmg_stale=%ld away=%ld nemesis_sent=%ld nemesis_got=%ld nemesis_skipped=%ld nemesis_applied=%ld nemesis_failed=%ld captains_sent=%ld captains_built=%ld captains_missing=%ld members_sent=%ld members_got=%ld members_skipped=%ld members_built=%ld members_failed=%ld attacks_ok=%ld attacks_fail=%ld plays_ok=%ld plays_fail=%ld hunt_engaged=%ld hunt_retargets=%ld hunt_woken=%ld hunt_wake_failed=%ld anims_sent=%ld anims_played=%ld anims_refused=%ld killed=%ld kill_failed=%ld local_damage=%ld local_sent=%ld moves_ok=%ld moves_failed=%ld hits_by_decoy=%ld hits_poked=%ld standin_drops=%ld retarget_left=%ld retarget_skipped=%ld hunt_unstuck=%ld decoy_reanchors=%ld decoy_hurt=%ld decoy_hurt_amount=%ld decoy_remade=%ld decoy_stall_remade=%ld despawn_calls=%ld despawn_repeats=%ld despawn_skipped=%ld despawn_faults=%ld despawn_repeat_faults=%ld despawn_refused=%ld unlink_fix=%ld unlink_repaired=%ld unlink_left=%ld unlink_faults=%ld retarget_refs=%ld retarget_raw=%ld member_links=%ld member_links_raw=%ld decoy_limbo=%zu decoy_limbo_gone=%ld decoy_killed=%ld dmg_borrowed=%ld decoy_by_remote=%ld decoy_unwritten=%ld decoy_hurt_sent=%ld standin_hurt=%ld standin_hurt_sent=%ld proxy_recycled=%ld puppet_recycled=%ld ai_table_max=%ld ai_list_max=%ld ai_table_scans=%ld ai_param_guard=%ld/%ld ai_tick_guard=%ld/%ld hunt_player_left=%ld dmg_dups=%ld counter_strikes=%ld counter_heals=%ld local_after_strike=%ld local_bystander=%ld local_fall=%ld losses_applied=%ld losses_killed=%ld losses_stale=%ld decoy=abs:%llX\n",
           g_cfg.enemies ? "on" : "off", g_cfg.net_role.c_str(), g_enemy_host.known(), g_enemy_announced, g_enemy_sent,
           g_cfg.net_role == "client" ? g_enemy_client.proxies().size() : (size_t)0, with_pos, spawned_now, g_enemy_spawned,
           g_enemy_spawn_failed, g_enemy_despawn_asked, g_enemy_hits_sent, SimProxyHits(), g_enemy_hits_applied, g_enemy_hits_failed,
           g_enemy_hits_stale, g_travels, g_enemy_hits_unmatched, g_enemy_dmg_sent, g_enemy_dmg_applied, g_enemy_dmg_failed, g_enemy_dmg_stale, g_enemy_away, g_nemesis_sent, g_nemesis_got, g_nemesis_skipped, g_nemesis_applied, g_nemesis_failed, g_enemy_members_sent, g_enemy_captains, g_enemy_captains_missing, g_member_sent, g_member_got, g_member_skipped, g_member_built, g_member_failed,
           g_enemy_attack_ok, g_enemy_attack_fail, g_enemy_play_ok, g_enemy_play_fail, g_hunt_engaged, g_hunt_retargets, g_hunt_woken, g_hunt_wake_failed, g_enemy_anims_sent, g_enemy_anims_played, g_enemy_anims_refused, g_enemy_killed, g_enemy_kill_failed, g_enemy_local_damage, g_enemy_local_sent, SimProxyMovesOk(), SimProxyMovesFailed(), g_enemy_hits_by_decoy,
           g_enemy_hits_poked, SimStandinHitDrops(), SimRetargetLeft(), SimRetargetSkipped(), g_hunt_unstuck, g_decoy_reanchors, g_decoy_hurt, g_decoy_hurt_amount, g_decoy_remade, g_decoy_stall_remade, dsp_calls, dsp_repeats, dsp_skipped,
           dsp_faults, dsp_repeat_faults, dsp_refused, ul_installed, ul_repaired, ul_left, ul_faults, rt_refs, rt_raw, ml_assigned, ml_raw, g_decoy_limbo.size(), g_decoy_limbo_gone, g_decoy_killed, g_enemy_dmg_borrowed, g_decoy_by_remote, g_decoy_unwritten, g_decoy_hurt_sent, g_standin_hurt, g_standin_hurt_sent, g_proxy_recycled, g_puppet_recycled, g_ai_table_max_seen, g_ai_list_max_seen, g_ai_table_scans, ap_installed, ap_guarded, at_installed, at_skipped, g_hunt_player_left, SimDamageDups(), g_counter_strikes, g_counter_heals, g_enemy_local_after_strike, g_enemy_local_bystander, g_enemy_local_fall, g_enemy_losses_applied, g_enemy_losses_killed, g_enemy_losses_stale, (unsigned long long)g_decoy_owner);
  return b;
}

void CoopSetAnimSnap(float units) { g_anim_snap = units; }

void CoopSetInputs(bool on) { InterlockedExchange(&g_inputs_on, on ? 1 : 0); }

void CoopSetSpeed(bool on) { InterlockedExchange(&g_speed_on, on ? 1 : 0); }

void CoopSetActions(bool on) { InterlockedExchange(&g_actions_on, on ? 1 : 0); }

void CoopSetFollower(bool on) { InterlockedExchange(&g_follower_on, on ? 1 : 0); }

void CoopSetTargetable(bool on) { InterlockedExchange(&g_targetable_on, on ? 1 : 0); }

void CoopSetAirFollow(bool on) { InterlockedExchange(&g_air_on, on ? 1 : 0); }

void CoopSetAttacks(bool on) { InterlockedExchange(&g_attacks_on, on ? 1 : 0); }

void CoopSetPutBackResets(bool on) { InterlockedExchange(&g_resets_on, on ? 1 : 0); }


void CoopSetFollow(int follow) {
  InterlockedExchange(&g_follow, follow);
  SowLog(L"coop: puppet follows by %s", follow == 4   ? L"the sender's animation inputs (put back when it drifts)"
                                         : follow == 3 ? L"teleporting, with its locomotion asked to walk or run"
                                         : follow == 2 ? L"walking"
                                         : follow == 1 ? L"teleporting"
                                                       : L"nothing (paused)");
}

std::string CoopStatus() {
  if (!g_started || (!g_cfg.listen_port && !g_net)) return "err coop receiver off (no coop_listen or coop_role)\n";
  std::string net;
  if (g_net) {
    EnterCriticalSection(&g_net_lock);
    const sownet::Stats& st = g_net->stats();
    char nb[420];
    snprintf(nb, sizeof nb, " net=%s/%S rtt=%.0f jitter=%.0f in=%llu out=%llu bad=%llu resends=%llu hellos=%llu unknown_session=%llu unknown_src=%llu recv_errors=%ld [%s]",
             g_cfg.net_role.c_str(), NetStatusName(g_net->status()), st.rtt_ms, st.jitter_ms, (unsigned long long)st.datagrams_in,
             (unsigned long long)st.datagrams_out, (unsigned long long)(st.bad_crc + st.bad_format),
             (unsigned long long)st.resends, (unsigned long long)st.hellos, (unsigned long long)st.unknown_session,
             (unsigned long long)st.unknown_src, g_net_recv_errors, g_net->debug().c_str());
    LeaveCriticalSection(&g_net_lock);
    net = nb;
  }
  Packet local{};
  const bool have_local = ReadPlayer(&local);
  char b[640];
  EnterCriticalSection(&g_lock);
  if (!g_have) {
    LeaveCriticalSection(&g_lock);
    return "ok packets=0 (nothing received yet)" + net + "\n";
  }
  const ULONGLONG now = GetTickCount64();
  const double span = (double)(g_last_at - g_first_at) / 1000.0;
  const double rate = span > 0 ? (g_packets - 1) / span : 0.0;
  const Packet& p = g_last;
  int n = snprintf(b, sizeof b,
                   "ok packets=%u lost=%u bad=%u rate=%.1f age_ms=%llu seq=%u present=%d pos=%.3f,%.3f,%.3f "
                   "rot=%.4f,%.4f,%.4f,%.4f health=%.2f max=%.2f shown=%u",
                   g_packets, g_lost, g_bad, rate, now - g_last_at, p.seq, p.flags & 1, p.pos[0], p.pos[1], p.pos[2],
                   p.rot[0], p.rot[1], p.rot[2], p.rot[3], p.health, p.max_health, SimLkpShown());
  if (have_local && n > 0 && n < (int)sizeof b) {
    float dx = p.pos[0] - local.pos[0], dy = p.pos[1] - local.pos[1], dz = p.pos[2] - local.pos[2];
    snprintf(b + n, sizeof b - n, " local=%.3f,%.3f,%.3f distance=%.1f", local.pos[0], local.pos[1], local.pos[2],
             std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  LeaveCriticalSection(&g_lock);
  char w[448];
  const LONG f = g_follow;
  snprintf(w, sizeof w,
           " follow=%s put_back=%u inputs=%s/%lu actions=%s/%ld follower=%s/%ld targetable=%s/%ld/%ld/%ld air=%s/%ld/%ld/%ld attacks=%s/%ld/%ld resets=%s/%ld anim_frames=%ld no_xform=%ld stuck=%ld "
           "refused=%ld not_ok=%ld off=%.1f turn=%.1f sender_speed=%.1f standing_ms=%llu frozen=%s/%ld/%ld",
           f == 4 ? "anim" : f == 3 ? "both" : f == 2 ? "walk" : f == 1 ? "teleport" : "none",
           f == 4 ? g_anim_snaps : g_walk_teleports, g_inputs_on ? "on" : "off", (unsigned long)g_inputs_given,
           g_actions_on ? "on" : "off", SimPuppetActionFrames(), g_follower_on ? "on" : "off", SimFollowerMarks(),
           g_targetable_on ? "on" : "off", SimTargetRejects(), SimHitDrops(), SimAoeSkips(), g_air_on ? "on" : "off",
           g_air_frames, SimSkippedSteps(), g_air_landings, g_attacks_on ? "on" : "off", SimAttacksStarted(),
           SimAttacksStopped(), g_resets_on ? "on" : "off", SimAnimResetsSkipped(),
           g_anim_frames, g_anim_no_xform, g_anim_stuck, g_anim_refused, g_frames_not_ok, g_anim_off, g_anim_turn,
           g_anim_sender_speed, now - g_sender_moving_at, g_puppet_frozen ? "yes" : "no", g_frozen_spells, g_frozen_writes);
  return std::string(b) + net +
         (g_cfg.puppet ? " " + SimPuppetStatus() + w + " " + SimRootSpeedReport() + " " + SimPuppetDetail()
                       : std::string()) +
         "\n";
}
