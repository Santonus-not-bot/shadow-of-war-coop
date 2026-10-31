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
#include "input.h"
#include "probe.h"
#include "simhook.h"

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
    SimSpawnRvas rv;
    rv.gameclient_rva = g_cfg.gameclient_rva;
    const bool spawned = SimSpawnPuppet(rv, g_cfg.spawn_type.c_str(), pos, &r);
    if (!spawned) ++tries;
    SowLog(L"coop: %S%S", spawned ? "" : "no spawned puppet yet: ", r.c_str());
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

// The simulation thread, once per frame: send this copy's player, move the puppet.
void CoopFrame() {
  if (g_cfg.send_port && !g_inputs_requested) {
    g_inputs_requested = true;
    std::string e;
    if (!SimAnimInputsInstall(g_cfg.anim_var_rva, g_cfg.gameclient_rva, &e))
      SowLog(L"coop: the player's animation inputs are not sent: %S", e.c_str());
  }
  if (g_cfg.send_port && g_send_socket != INVALID_SOCKET) SendSample();
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
    const ULONGLONG now = GetTickCount64();
    if (g_have && p.seq > g_last.seq + 1) g_lost += p.seq - g_last.seq - 1;
    if (!g_have) g_first_at = now;
    g_last = p;
    g_last_at = now;
    g_have = true;
    ++g_packets;
    Remember(p, recv_us);
    LeaveCriticalSection(&g_lock);
    if (!logged_first) {
      SowLog(L"coop: first packet (seq %u) from 127.0.0.1:%u", p.seq, ntohs(from.sin_port));
      logged_first = true;
    }
    MaybeShow(p);
  }
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
  if (cfg.send_port || cfg.puppet) SimSetFrameCallback(&CoopFrame);
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
  if (!g_started || !g_cfg.listen_port) return "err coop receiver off (no coop_listen)\n";
  Packet local{};
  const bool have_local = ReadPlayer(&local);
  char b[640];
  EnterCriticalSection(&g_lock);
  if (!g_have) {
    LeaveCriticalSection(&g_lock);
    return "ok packets=0 (nothing received yet)\n";
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
  return std::string(b) +
         (g_cfg.puppet ? " " + SimPuppetStatus() + w + " " + SimRootSpeedReport() + " " + SimPuppetDetail()
                       : std::string()) +
         "\n";
}
