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

#include "coop.h"
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
volatile LONG g_enemy_dmg_sent = 0, g_enemy_dmg_applied = 0, g_enemy_dmg_failed = 0, g_enemy_dmg_stale = 0;
volatile LONG g_enemy_hits_unmatched = 0, g_enemy_hits_sent = 0, g_enemy_hits_applied = 0, g_enemy_hits_failed = 0, g_enemy_hits_stale = 0;
volatile LONG g_enemy_sent = 0, g_enemy_announced = 0, g_enemy_spawned = 0, g_enemy_spawn_failed = 0,
              g_enemy_despawn_asked = 0, g_enemy_away = 0;
// No state from the host for this long: its world is stopped (a death screen, a load, its pause menu), so its
// enemies' pictures would stand frozen. They are taken away, and shown again where the host's next states put them.
constexpr ULONGLONG kHostAwayMs = 5000;
std::string g_enemy_last_error;

void QueueEnemyEvent(const uint8_t* p, size_t n) {
  if (n < 5 || n > 64) return;
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
    sowenemy::HitMsg m;
    if (!sowenemy::ParseHit(ev.data(), ev.size(), &m)) continue;
    const uintptr_t owner = g_enemy_host.keyOf(m.id);
    if (!owner) {
      InterlockedIncrement(&g_enemy_hits_stale);  // the enemy is gone or was never announced in this session
      continue;
    }
    if (++applied_now > 16) break;
    if (SimApplyHit(g_cfg.hit_rva, g_cfg.attack_table_rva, owner, m.pos, m.attack_id, g_cfg.gameclient_rva)) {
      if (InterlockedIncrement(&g_enemy_hits_applied) <= 5)
        SowLog(L"coop: the client hit enemy %u (attack %08X) at %.0f,%.0f,%.0f: applied", m.id, m.attack_id, m.pos[0], m.pos[1], m.pos[2]);
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

void DecoyFrame() {
  const ULONGLONG now = GetTickCount64();
  float at[3], rot[4];
  if (!SimPuppetXform(at, rot) || !SimHasPuppet()) {
    if (g_decoy_owner) {
      SimSetDecoy(0);
      SimProxyHealth(g_decoy_owner, 0.0f);
      SimDespawnNow(g_cfg.despawn_rva, g_decoy_owner);
      SimProxyDrop(g_decoy_owner);
      g_decoy_owner = 0;
    }
    return;
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
  SimProxyMove(g_cfg.teleport_rva, g_decoy_owner, where, rot, true);
  SimProxyHealth(g_decoy_owner, 1.0f);
  SimHideCharacter(g_decoy_owner);
}

// The host: enemy swings that reached the client's stand-in go to the client, whose player takes them.
void EnemyHostDamage() {
  static std::vector<std::vector<uint8_t>> pending;
  SimHitReport rp[16];
  const size_t n = SimDamageReports(rp, 16);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t id = g_enemy_host.idOf(rp[i].owner);
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
    std::string e;
    if (!SimAttackHooksInstall(g_cfg.target_check_rva, g_cfg.hit_rva, g_cfg.aoe_rva, &e))
      SowLog(L"coop: enemy swings cannot be carried to the client: %S", e.c_str());
  }
  EnemyHostHits();
  EnemyHostDamage();
  if (g_cfg.decoy) DecoyFrame();
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
    if (!SimEnemyRegistryStart(g_cfg.brain_site_rva, g_cfg.tree_rva, &e)) SowLog(L"coop: the host cannot list its enemies: %S", e.c_str());
    else SowLog(L"coop: the host announces the enemies within %.0f units of either player", g_cfg.enemy_radius);
  }
  if (session != g_enemy_session) {
    g_enemy_session = session;
    g_enemy_host.forgetAll();
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
    sowenemy::CopyType(c.s.type, info.type);
    cand.push_back(c);
  }
  std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
  std::vector<sowenemy::Seen> chosen;
  for (size_t i = 0; i < cand.size() && i < sowenemy::kMaxEnemies; ++i) chosen.push_back(cand[i].s);
  const sowenemy::HostTracker::Out out = g_enemy_host.update(chosen, now);
  // Events are kept until the transport can take them (the lock may be busy: see SendSample); a snapshot is not.
  static std::vector<std::vector<uint8_t>> pending;
  for (const auto& ev : out.events) pending.push_back(ev);
  if (!TryEnterCriticalSection(&g_net_lock)) return;
  for (const auto& ev : pending) g_net->sendEvent(ev.data(), ev.size());
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
  SimProxyHealth(p.handle, 0.0f);
  SimDespawnNow(g_cfg.despawn_rva, p.handle);
  if (g_limbo.size() < 48) g_limbo.push_back(p.handle);
  else SimProxyDrop(p.handle);
  p.handle = 0;
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
    else SimHitWatch(g_cfg.gameclient_rva, true);
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
    sowenemy::HitMsg dm;
    if (sowenemy::ParseDamage(ev.data(), ev.size(), &dm)) {
      auto it = proxies.find(dm.id);
      if (it == proxies.end() || !it->second.handle) {
        InterlockedIncrement(&g_enemy_dmg_stale);
      } else if (SimApplyDamage(g_cfg.hit_rva, g_cfg.attack_table_rva, it->second.handle, dm.pos, dm.attack_id, g_cfg.gameclient_rva)) {
        if (InterlockedIncrement(&g_enemy_dmg_applied) <= 5)
          SowLog(L"coop: enemy %u (%S) hit this player (attack %08X): applied", dm.id, it->second.type, dm.attack_id);
      } else {
        InterlockedIncrement(&g_enemy_dmg_failed);
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
    if (!p.handle) {
      if (p.spawn_failed || spawns_left == 0 || now < p.next_spawn_ms || !(p.cur.flags & sowenemy::kAlive) || SimSpawnBusy()) continue;
      --spawns_left;
      SimSpawnRvas rv;
      rv.gameclient_rva = g_cfg.gameclient_rva;
      std::string report;
      uintptr_t obj = 0;
      if (SimSpawn(rv, p.type, p.cur.pos, nullptr, &report, &obj, true) && obj && SimProxyAdopt(obj)) {
        p.handle = obj;
        p.shown_set = false;
        InterlockedIncrement(&g_enemy_spawned);
      } else {
        p.spawn_failed = true;
        InterlockedIncrement(&g_enemy_spawn_failed);
        SowLog(L"coop: no proxy for enemy %u (%S): %S", p.id, p.type, report.c_str());
      }
      if (!p.handle) continue;
    }
    float target[3];
    sowenemy::ClientTracker::Predict(p, now, target);
    if (!p.shown_set || DistXZ(p.shown, target) > 400.0f) {
      memcpy(p.shown, target, sizeof p.shown);
      p.shown_set = true;
    } else {
      for (int i = 0; i < 3; ++i) p.shown[i] += (target[i] - p.shown[i]) * 0.5f;
    }
    const float h = p.cur.yaw * 0.5f, rot[4] = {0.0f, std::sin(h), 0.0f, std::cos(h)};
    SimProxyMove(g_cfg.teleport_rva, p.handle, p.shown, rot, g_cfg.enemy_direct);
    SimProxyHealth(p.handle, (p.cur.flags & sowenemy::kAlive) ? p.cur.hp / 65535.0f : 0.0f);
    if (p.cur.attack_id != p.applied_attack || p.cur.attack_stamp != p.applied_stamp) {
      if (p.applied_record) SimProxyStopAttack(p.handle, p.applied_record);
      p.applied_record = 0;
      if (p.cur.attack_id)
        p.applied_record = SimProxyAttack(g_cfg.attack_thunk_rva, g_cfg.attack_stop_rva, g_cfg.attack_table_rva, p.handle, p.cur.attack_id);
      if (!p.cur.attack_id) p.applied_loco = 255;  // the attack ended: show how it moves again
      p.applied_attack = p.cur.attack_id;
      p.applied_stamp = p.cur.attack_stamp;
    }
    if (!p.cur.attack_id && p.cur.loco != p.applied_loco) {
      static const char* const kLoco[] = {"Idle", "WalkForward", "Run"};
      if (p.cur.loco < 3 && SimProxyPlay(g_cfg.play_rva, p.handle, kLoco[p.cur.loco])) p.applied_loco = p.cur.loco;
      else p.applied_loco = p.cur.loco;  // not played (no such state): do not retry every frame
    }
  }
  if (g_test_dmg_owner) {
    const uintptr_t who = (uintptr_t)InterlockedExchange64(&g_test_dmg_owner, 0);
    float at[3] = {};
    Packet me{};
    if (ReadPlayer(&me)) memcpy(at, me.pos, sizeof at);
    g_test_dmg_result = SimApplyDamage(g_cfg.hit_rva, g_cfg.attack_table_rva, who, at, (uint32_t)g_test_dmg_attack, g_cfg.gameclient_rva) ? 1 : -1;
  }
  // Strikes of this player on proxies, to the host.
  {
    SimHitReport hr[16];
    const size_t n = SimHitReports(hr, 16);
    static std::vector<std::vector<uint8_t>> outbox;
    for (size_t i = 0; i < n; ++i) {
      bool matched = false;
      for (const auto& kv : proxies)
        if (kv.second.handle == hr[i].owner) {
          outbox.push_back(sowenemy::HitEvent(kv.first, hr[i].attack_id, hr[i].pos));
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

void EnemyFrame() {
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
  SimApplyRetargets();
  if (g_cfg.net_role == "client" && g_cfg.follow_travel) TravelFrame();
  if (g_cfg.puppet && !g_cfg.spawn_type.empty()) PuppetSpawnFrame();
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

std::string CoopTestDamage(uintptr_t proxy, uint32_t attack_id) {
  g_test_dmg_attack = (LONG)attack_id;
  g_test_dmg_result = 0;
  InterlockedExchange64(&g_test_dmg_owner, (LONG64)proxy);
  for (int i = 0; i < 100 && !g_test_dmg_result; ++i) Sleep(20);
  return g_test_dmg_result == 1 ? "ok applied\n" : g_test_dmg_result == -1 ? "err the hit was not applied\n" : "err not run (no enemy sharing on this client?)\n";
}

std::string CoopEnemyStatus() {
  char b[520];
  size_t spawned_now = 0, with_pos = 0;
  if (g_cfg.net_role == "client")
    for (auto& kv : g_enemy_client.proxies()) {
      if (kv.second.handle) ++spawned_now;
      if (kv.second.have) ++with_pos;
    }
  snprintf(b, sizeof b,
           "ok enemies=%s role=%s host_known=%zu host_announced=%ld host_snapshots=%ld client_proxies=%zu with_pos=%zu "
           "standing=%zu spawned=%ld failed=%ld despawn_asked=%ld hits_sent=%ld hits_on_proxies=%ld hits_applied=%ld hits_failed=%ld hits_stale=%ld travels=%ld hits_unmatched=%ld dmg_sent=%ld dmg_applied=%ld dmg_failed=%ld dmg_stale=%ld away=%ld\n",
           g_cfg.enemies ? "on" : "off", g_cfg.net_role.c_str(), g_enemy_host.known(), g_enemy_announced, g_enemy_sent,
           g_cfg.net_role == "client" ? g_enemy_client.proxies().size() : (size_t)0, with_pos, spawned_now, g_enemy_spawned,
           g_enemy_spawn_failed, g_enemy_despawn_asked, g_enemy_hits_sent, SimProxyHits(), g_enemy_hits_applied, g_enemy_hits_failed,
           g_enemy_hits_stale, g_travels, g_enemy_hits_unmatched, g_enemy_dmg_sent, g_enemy_dmg_applied, g_enemy_dmg_failed, g_enemy_dmg_stale, g_enemy_away);
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
  char w[384];
  const LONG f = g_follow;
  snprintf(w, sizeof w,
           " follow=%s put_back=%u inputs=%s/%lu actions=%s/%ld follower=%s/%ld targetable=%s/%ld/%ld/%ld air=%s/%ld/%ld/%ld attacks=%s/%ld/%ld resets=%s/%ld anim_frames=%ld no_xform=%ld stuck=%ld "
           "refused=%ld not_ok=%ld off=%.1f turn=%.1f sender_speed=%.1f standing_ms=%llu",
           f == 4 ? "anim" : f == 3 ? "both" : f == 2 ? "walk" : f == 1 ? "teleport" : "none",
           f == 4 ? g_anim_snaps : g_walk_teleports, g_inputs_on ? "on" : "off", (unsigned long)g_inputs_given,
           g_actions_on ? "on" : "off", SimPuppetActionFrames(), g_follower_on ? "on" : "off", SimFollowerMarks(),
           g_targetable_on ? "on" : "off", SimTargetRejects(), SimHitDrops(), SimAoeSkips(), g_air_on ? "on" : "off",
           g_air_frames, SimSkippedSteps(), g_air_landings, g_attacks_on ? "on" : "off", SimAttacksStarted(),
           SimAttacksStopped(), g_resets_on ? "on" : "off", SimAnimResetsSkipped(),
           g_anim_frames, g_anim_no_xform, g_anim_stuck, g_anim_refused, g_frames_not_ok, g_anim_off, g_anim_turn,
           g_anim_sender_speed, now - g_sender_moving_at);
  return std::string(b) + net +
         (g_cfg.puppet ? " " + SimPuppetStatus() + w + " " + SimRootSpeedReport() + " " + SimPuppetDetail()
                       : std::string()) +
         "\n";
}
