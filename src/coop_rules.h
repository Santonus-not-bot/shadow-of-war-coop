// Small decisions of the co-op code, kept free of the game so enemy_test.exe can check them (session cm).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace cooprules {

// ---- One swing that reaches both the stand-in and the decoy (the host) -------------------------------------------------------
// The decoy stands where the stand-in stands, so one swing can reach both; it was reported twice and the client took it twice.
// A report of the same attacker and attack on the other of the two within kSameSwingMs is the same swing.
constexpr uint64_t kSameSwingMs = 300;
struct SwingSeen {
  uintptr_t attacker = 0, victim = 0;
  uint32_t attack = 0;
  uint64_t at = 0;
};
// The last report of each of the most recent attackers. One slot was not enough: two orcs landing the same attack in the same
// frames reported orc 1 on the stand-in, orc 2 on the stand-in, orc 1 on the decoy, orc 2 on the decoy, and both second reports
// were sent (session cm: 3 pairs 31-109 ms apart in a regression run).
constexpr int kSwingSlots = 8;
struct SwingsSeen {
  SwingSeen slot[kSwingSlots];
};
// True when this report is the second of one swing (not to be sent); otherwise it becomes its attacker's last report (in that
// attacker's slot, else the oldest).
inline bool SameSwingAgain(SwingsSeen* seen, uintptr_t attacker, uint32_t attack, uintptr_t victim, uint64_t now) {
  SwingSeen* mine = nullptr;
  for (SwingSeen& e : seen->slot) {
    if (e.at && e.attacker == attacker) {
      if (attack == e.attack && victim != e.victim && now - e.at < kSameSwingMs) return true;
      mine = &e;
      break;
    }
  }
  if (!mine) {
    mine = &seen->slot[0];
    for (SwingSeen& e : seen->slot)
      if (e.at < mine->at) mine = &e;
  }
  *mine = SwingSeen{attacker, victim, attack, now};
  return false;
}

// ---- Health a proxy loses on the client outside the hit function (the client) ---------------------------------------------
// Told to the host as stock strikes (the client's arrows), but not within kAfterStrikeMs of a reported strike of the client on it
// (a counter kill's damage lands frames later), and never for the host's allies and bystanders (their losses here are not the
// client's doing).
constexpr uint64_t kAfterStrikeMs = 2500;
inline bool BystanderKind(const char* type, size_t n) {
  char kind[64] = {};
  memcpy(kind, type, n < sizeof kind - 1 ? n : sizeof kind - 1);
  return strstr(kind, "Tamed") || strstr(kind, "Prisoner") || strstr(kind, "Slave") || strstr(kind, "Captive") ||
         strstr(kind, "Follower");
}
inline bool LocalLossReported(uint64_t now, uint64_t strike_ms, bool bystander) {
  const bool after_strike = strike_ms && now - strike_ms < kAfterStrikeMs;
  return !after_strike && !bystander;
}
// Stock strikes for a loss: one per 90 health (a stock strike takes about 96 of an orc's 300), at least one, at most six.
inline int StockStrikes(float lost) {
  int n = (int)std::ceil(lost / 90.0f);
  return n < 1 ? 1 : n > 6 ? 6 : n;
}

// A proxy's highest point over the last kFallWindowMs: a fall since then (more than kFallUnits down) explains a loss of health
// there (session cm: proxies dropped 150-163 units near the tower and a wild warg lost 100.8, told to the host as the client's
// strikes). The host's own enemy falls too and loses its own health there.
constexpr uint64_t kFallWindowMs = 2000;
constexpr float kFallUnits = 100.0f;
inline void TrackPeak(float* peak_y, uint64_t* peak_ms, float y, uint64_t now) {
  if (!*peak_ms || y >= *peak_y || now - *peak_ms > kFallWindowMs) {
    *peak_y = y;
    *peak_ms = now;
  }
}
inline bool RecentFall(float peak_y, uint64_t peak_ms, float y) { return peak_ms && peak_y - y > kFallUnits; }

// ---- A stand-in the game does not move (both roles) --------------------------------------------------------------------------
// Way-off put-backs that left it where it was are counted; at kNoLandFrames in a row it is moved by direct writes.
constexpr int kNoLandFrames = 30;
inline int NoLandStep(int no_land, bool way_off, uint64_t now, uint64_t putback_at, float moved_sq) {
  if (!way_off) return 0;
  return putback_at && now - putback_at < 1000 && moved_sq < 1.0f ? no_land + 1 : 0;
}

// ---- A counter of the client (the client, coop_counter_heal) ------------------------------------------------------------
// A counter strike within kCounterWindowMs of that enemy's last swing gives back the health the client had before it (never more).
constexpr uint64_t kCounterWindowMs = 1500;
inline bool CounterGivesBack(uint64_t now, uint64_t swing_at, float before, float cur) {
  return swing_at && now - swing_at < kCounterWindowMs && cur < before;
}

}  // namespace cooprules
