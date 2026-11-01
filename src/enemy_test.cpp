// Tests of enemy_proto.h (no game): what the host announces and streams about its enemies, and what the client
// does with it. enemy_test.exe must print RESULT: PASS.
#include <cstdio>
#include <cstdlib>
#include <random>

#include "enemy_proto.h"

using namespace sowenemy;

static int g_ok = 0, g_bad = 0;
static void Check(bool ok, const char* what) {
  printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
  (ok ? g_ok : g_bad)++;
}

static Seen Orc(uintptr_t key, float x, float z, const char* type = "Orc_Infantry") {
  Seen s;
  s.key = key;
  s.pos[0] = x;
  s.pos[1] = 100;
  s.pos[2] = z;
  CopyType(s.type, type);
  return s;
}

static const Snapshot* AsSnap(const std::vector<uint8_t>& v) { return reinterpret_cast<const Snapshot*>(v.data()); }

int main() {
  printf("[host: announcing, streaming and forgetting]\n");
  {
    HostTracker h;
    auto o = h.update({Orc(0x1000, 0, 0), Orc(0x2000, 500, 0, "Orc_Archer")}, 1000);
    Check(o.events.size() == 2 && o.events[0][0] == kOpNew && o.events[0].size() == 1 + 4 + kTypeBytes,
          "two new enemies: two NEW events with their type");
    Check(!o.snapshot.empty() && AsSnap(o.snapshot)->count == 2 && o.snapshot.size() == SnapshotBytes(2) &&
              memcmp(AsSnap(o.snapshot)->magic, "SOWE", 4) == 0,
          "and a snapshot of both, only as long as its records");
    const uint32_t id0 = AsSnap(o.snapshot)->rec[0].id, id1 = AsSnap(o.snapshot)->rec[1].id;
    Check(id0 != id1 && id0 != 0 && id1 != 0, "with different non-zero ids");
    o = h.update({Orc(0x1000, 0, 0), Orc(0x2000, 500, 0, "Orc_Archer")}, 1100);
    Check(o.events.empty() && AsSnap(o.snapshot)->rec[0].id == id0, "the next tick announces nothing and keeps the ids");
    // 0x1000 walks 25 units in 100 ms (250 a second), 0x2000 runs 60 units (600 a second).
    o = h.update({Orc(0x1000, 25, 0), Orc(0x2000, 560, 0, "Orc_Archer")}, 1200);
    o = h.update({Orc(0x1000, 50, 0), Orc(0x2000, 620, 0, "Orc_Archer")}, 1300);
    const Snapshot* s = AsSnap(o.snapshot);
    Check(s->rec[0].loco == kWalk && s->rec[1].loco == kRun, "250 units a second is a walk, 600 a run");
    for (uint64_t t = 1400; t <= 2000; t += 100) o = h.update({Orc(0x1000, 50, 0), Orc(0x2000, 620, 0, "Orc_Archer")}, t);
    Check(AsSnap(o.snapshot)->rec[0].loco == kIdle, "standing still is idle again");
    o = h.update({Orc(0x1000, 50, 0)}, 2100);
    Check(o.events.size() == 1 && o.events[0][0] == kOpGone && o.events[0].size() == 5, "an enemy that is no longer chosen gets a GONE");
    Check(h.known() == 1, "and is forgotten");
    // the address of 0x1000 is reused by another kind of character
    o = h.update({Orc(0x1000, 50, 0, "Warg_Pack")}, 2200);
    uint32_t gone = 0, fresh = 0;
    memcpy(&gone, &o.events[0][1], 4);
    memcpy(&fresh, &o.events[1][1], 4);
    Check(o.events.size() == 2 && o.events[0][0] == kOpGone && o.events[1][0] == kOpNew && gone == id0 && fresh != id0,
          "a reused address with another type is a GONE and a NEW with a new id");
    h.forgetAll();
    o = h.update({Orc(0x1000, 50, 0, "Warg_Pack")}, 2300);
    Check(o.events.size() == 1 && o.events[0][0] == kOpNew, "after a new session everything is announced again");
    std::vector<Seen> many;
    for (int i = 0; i < 40; ++i) many.push_back(Orc(0x10000 + i * 16, (float)i, 0));
    o = h.update(many, 2400);
    Check(AsSnap(o.snapshot)->count == kMaxEnemies && o.snapshot.size() == SnapshotBytes(kMaxEnemies) && o.snapshot.size() < 600,
          "a snapshot holds at most 16 enemies and stays under 600 bytes");
  }

  printf("[client: events, snapshots, bad input]\n");
  {
    HostTracker h;
    ClientTracker c;
    auto o = h.update({Orc(0x1000, 100, 200)}, 1000);
    std::vector<uint32_t> gone;
    Check(c.onSnapshot(o.snapshot.data(), o.snapshot.size(), 1010) == 0, "a snapshot before the NEW is ignored (no proxy yet)");
    Check(c.onEvent(o.events[0].data(), o.events[0].size(), &gone) && c.proxies().size() == 1, "the NEW makes a proxy");
    const uint32_t id = c.proxies().begin()->first;
    Check(strcmp(c.proxies()[id].type, "Orc_Infantry") == 0 && !c.proxies()[id].have, "of the announced type, with no position yet");
    o = h.update({Orc(0x1000, 100, 200)}, 1066);
    Check(c.onSnapshot(o.snapshot.data(), o.snapshot.size(), 1070) == 1 && c.proxies()[id].have, "the next snapshot is taken");
    const std::vector<uint8_t> old = o.snapshot;
    o = h.update({Orc(0x1000, 120, 200)}, 1132);
    Check(c.onSnapshot(o.snapshot.data(), o.snapshot.size(), 1136) == 1, "a newer one is taken");
    Check(c.onSnapshot(old.data(), old.size(), 1140) == 0 && c.proxies()[id].cur.pos[0] == 120, "an older one that arrives late is dropped");
    float at[3];
    ClientTracker::Predict(c.proxies()[id], 1136, at);
    Check(std::fabs(at[0] - 120) < 0.01f, "predicted at the moment of the snapshot: where it says");
    ClientTracker::Predict(c.proxies()[id], 1136 + 66, at);
    Check(at[0] > 125 && at[0] < 141, "66 ms later: carried forward at its speed (about 20 units per 66 ms)");
    ClientTracker::Predict(c.proxies()[id], 1136 + 5000, at);
    Check(at[0] < 170, "carried forward for 150 ms at most (45 units here), not 5 s");
    o = h.update({}, 1200);
    Check(c.onEvent(o.events[0].data(), o.events[0].size(), &gone) && gone.size() == 1 && gone[0] == id && c.proxies().empty(),
          "a GONE frees the proxy");

    // bad input: nothing changes, nothing crashes
    uint8_t shortev[3] = {kOpNew, 1, 0};
    Check(!c.onEvent(shortev, sizeof shortev, &gone), "a short event is refused");
    std::vector<uint8_t> wrong(1 + 4 + kTypeBytes - 1, 0);
    wrong[0] = kOpNew;
    Check(!c.onEvent(wrong.data(), wrong.size(), &gone) && c.proxies().empty(), "a NEW of the wrong length is refused");
    uint8_t unknown[5] = {9, 1, 0, 0, 0};
    Check(!c.onEvent(unknown, sizeof unknown, &gone), "an unknown opcode is refused");
    Snapshot bad;
    memset(&bad, 0, sizeof bad);
    memcpy(bad.magic, "SOWE", 4);
    bad.count = 17;
    Check(c.onSnapshot(reinterpret_cast<uint8_t*>(&bad), sizeof bad, 5000) == 0, "a snapshot of 17 records is refused");
    bad.count = 3;
    Check(c.onSnapshot(reinterpret_cast<uint8_t*>(&bad), SnapshotBytes(2), 5000) == 0, "a snapshot shorter than its count is refused");
    memcpy(bad.magic, "XXXX", 4);
    Check(c.onSnapshot(reinterpret_cast<uint8_t*>(&bad), sizeof bad, 5000) == 0, "wrong magic is refused");

    // a hostile host: NaN positions, random bytes, many ids
    std::mt19937 rng(7);
    uint8_t newev[1 + 4 + kTypeBytes] = {kOpNew, 7, 0, 0, 0};
    c.onEvent(newev, sizeof newev, &gone);
    Snapshot nan;
    memset(&nan, 0, sizeof nan);
    memcpy(nan.magic, "SOWE", 4);
    nan.seq = 1000;
    nan.count = 1;
    nan.rec[0].id = 7;
    nan.rec[0].pos[0] = std::nanf("");
    Check(c.onSnapshot(reinterpret_cast<uint8_t*>(&nan), SnapshotBytes(1), 6000) == 0 && !c.proxies()[7].have, "a NaN position is refused");
    int took = 0;
    for (int i = 0; i < 200000; ++i) {
      std::vector<uint8_t> junk(rng() % 700);
      for (auto& b : junk) b = (uint8_t)rng();
      if (i % 3 == 0 && junk.size() > 12) memcpy(junk.data(), "SOWE", 4);
      if (i % 5 == 0 && !junk.empty()) junk[0] = (uint8_t)(1 + rng() % 2);
      took += c.onSnapshot(junk.data(), junk.size(), 7000 + i) + (c.onEvent(junk.data(), junk.size(), &gone) ? 1 : 0);
    }
    Check(c.proxies().size() < 100000, "200,000 junk messages are survived");
    (void)took;
  }

  printf("[a client's hit: the message and the host's lookup]\n");
  {
    HostTracker h;
    auto o = h.update({Orc(0xAAAA, 1, 2), Orc(0xBBBB, 3, 4)}, 1000);
    const uint32_t id1 = AsSnap(o.snapshot)->rec[0].id, id2 = AsSnap(o.snapshot)->rec[1].id;
    Check(h.keyOf(id1) == 0xAAAA && h.keyOf(id2) == 0xBBBB && h.keyOf(999) == 0, "the host finds the character an announced id stands for");
    const float pos[3] = {10, 20, 30};
    const std::vector<uint8_t> ev = HitEvent(id2, 0x1234ABCDu, pos);
    HitMsg m;
    Check(ev.size() == 21 && ParseHit(ev.data(), ev.size(), &m) && m.id == id2 && m.attack_id == 0x1234ABCDu && m.pos[2] == 30,
          "a hit message is 21 bytes and reads back");
    o = h.update({Orc(0xAAAA, 1, 2)}, 1100);
    Check(h.keyOf(id2) == 0, "once the enemy is gone a late hit for it finds nothing");
    std::vector<uint8_t> bad = ev;
    bad.pop_back();
    Check(!ParseHit(bad.data(), bad.size(), &m), "a short hit message is refused");
    bad = ev;
    bad[0] = kOpGone;
    Check(!ParseHit(bad.data(), bad.size(), &m), "another opcode is not a hit");
    const float nanpos[3] = {std::nanf(""), 0, 0};
    const std::vector<uint8_t> nanev = HitEvent(id1, 1, nanpos);
    Check(!ParseHit(nanev.data(), nanev.size(), &m), "a NaN position is refused");
    const std::vector<uint8_t> dm = DamageEvent(id1, 0xCAFEF00Du, pos);
    Check(dm.size() == 21 && dm[0] == kOpDamage && ParseDamage(dm.data(), dm.size(), &m) && m.id == id1 && m.attack_id == 0xCAFEF00Du &&
              !ParseHit(dm.data(), dm.size(), &m),
          "a damage message (an enemy's swing, host to client) reads back as damage and is not taken for a hit");
    Check(!ParseDamage(ev.data(), ev.size(), &m), "and a hit message is not taken for damage");
    Check(h.idOf(0xAAAA) == id1 && h.idOf(0xBBBB) == 0 && h.idOf(0x1) == 0, "the host finds the id of an announced character, 0 for any other");
    const std::vector<uint8_t> zero = HitEvent(0, 1, pos);
    Check(!ParseHit(zero.data(), zero.size(), &m), "enemy id 0 is refused");
  }

  printf("\nRESULT: %s (%d of %d checks passed)\n", g_bad == 0 ? "PASS" : "FAIL", g_ok, g_ok + g_bad);
  return g_bad == 0 ? 0 : 1;
}
