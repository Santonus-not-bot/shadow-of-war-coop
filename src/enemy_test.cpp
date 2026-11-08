// Tests of enemy_proto.h (no game): what the host announces and streams about its enemies, and what the client
// does with it. enemy_test.exe must print RESULT: PASS.
#include <cstdio>
#include <cstdlib>
#include <random>

#include "enemy_proto.h"
#include "coop_rules.h"
#include "listfix.h"

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


static bool ClientTrackerAcceptsNemesis() {
  ClientTracker c;
  NemesisMsg n{};
  strcpy_s(n.event, "Promote");
  const std::vector<uint8_t> ev = NemesisEvent(n);
  std::vector<uint32_t> gone;
  return c.onEvent(ev.data(), ev.size(), &gone) || !c.proxies().empty();
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

  printf("[captains: the member behind an enemy]\n");
  {
    HostTracker h;
    ClientTracker c;
    Seen cap = Orc(0x1000, 0, 0, "OlogHai_Infantry");
    cap.member_id = 99;
    cap.faction_hash = 0xA1B2C3D4;
    CopyName(cap.name, "Ur-Gram");
    auto o = h.update({cap, Orc(0x2000, 500, 0, "Orc_Archer")}, 1000);
    int members = 0, news = 0;
    for (const auto& e : o.events) {
      members += e[0] == kOpMember;
      news += e[0] == kOpNew;
    }
    Check(news == 2 && members == 1, "a captain gets a MEMBER after his NEW, an ordinary orc none");
    Check(o.events.size() == 3 && o.events[1][0] == kOpMember && o.events[1].size() == kMemberBytes, "right after his NEW, in one fixed size");
    std::vector<uint32_t> gone;
    for (const auto& e : o.events) c.onEvent(e.data(), e.size(), &gone);
    const Proxy* pc = nullptr;
    for (auto& kv : c.proxies())
      if (strcmp(kv.second.type, "OlogHai_Infantry") == 0) pc = &kv.second;
    Check(pc && pc->member_id == 99 && pc->faction_hash == 0xA1B2C3D4 && strcmp(pc->name, "Ur-Gram") == 0,
          "the client reads the member's id, faction and name");
    o = h.update({cap, Orc(0x2000, 500, 0, "Orc_Archer")}, 1100);
    Check(o.events.empty(), "it is said once, not every tick");
    cap.member_id = 100;
    o = h.update({cap, Orc(0x2000, 500, 0, "Orc_Archer")}, 1200);
    Check(o.events.size() == 1 && o.events[0][0] == kOpMember, "and again if the character is another member");
    h.forgetAll();
    o = h.update({cap}, 1300);
    Check(o.events.size() == 2 && o.events[1][0] == kOpMember, "a new session announces the captain again");
    // bad input
    std::vector<uint8_t> shortm(kMemberBytes - 1, 0);
    shortm[0] = kOpMember;
    Check(!c.onEvent(shortm.data(), shortm.size(), &gone), "a MEMBER of the wrong length is refused");
    std::vector<uint8_t> orphan = MemberEvent(777, 1, 2, "Nobody");
    Check(c.onEvent(orphan.data(), orphan.size(), &gone) && c.proxies().size() == 2, "a MEMBER for an enemy the client does not know changes nothing");
    std::vector<uint8_t> unterminated(kMemberBytes, 'A');
    unterminated[0] = kOpMember;
    const uint32_t known = c.proxies().begin()->first;
    memcpy(&unterminated[1], &known, 4);
    c.onEvent(unterminated.data(), unterminated.size(), &gone);
    Check(c.proxies()[known].name[kNameBytes - 1] == 0, "a name without an end is cut");
  }

  printf("[members: the game's text for a member, in chunks]\n");
  {
    std::string text;
    for (int i = 0; i < 8048; ++i) text.push_back((char)('a' + i % 23));  // about the size of one member's text
    const auto ev = MemberDataEvents(0xAABBCCDD, 99, 0x1122334455667788ull, text);
    Check(ev.size() == 9 && ev[0][0] == kOpMemberData, "8,048 bytes go in 9 messages");
    bool fits = true;
    for (const auto& e : ev) fits = fits && e.size() <= sizeof(MemberDataHead) + kMemberChunk && e.size() < 1000;
    Check(fits, "each under a datagram");
    MemberAssembler a;
    uint32_t hash = 0, id = 0;
    uint64_t seed = 0;
    std::string back;
    bool done = false;
    for (size_t i = 0; i < ev.size(); ++i) {
      done = a.add(ev[i].data(), ev[i].size(), &hash, &id, &seed, &back);
      Check(done == (i + 1 == ev.size()), "the member is complete at the last chunk only") ;
      if (done != (i + 1 == ev.size())) break;
    }
    Check(done && hash == 0xAABBCCDD && id == 99 && seed == 0x1122334455667788ull && back == text, "and reads back whole");
    // a chunk missing: nothing comes out, the next member is read fine
    MemberAssembler b;
    b.add(ev[0].data(), ev[0].size(), &hash, &id, &seed, &back);
    Check(!b.add(ev[2].data(), ev[2].size(), &hash, &id, &seed, &back), "a chunk out of order is dropped");
    bool again = false;
    for (const auto& e : ev) again = b.add(e.data(), e.size(), &hash, &id, &seed, &back);
    Check(again && back == text, "and the member sent again is read");
    // another member's chunk in the middle
    const auto other = MemberDataEvents(1, 2, 3, text);
    MemberAssembler c;
    c.add(ev[0].data(), ev[0].size(), &hash, &id, &seed, &back);
    Check(!c.add(other[1].data(), other[1].size(), &hash, &id, &seed, &back), "a chunk of another member in the middle is dropped");
    // bad input
    uint8_t junk[5] = {kOpMemberData, 1, 2, 3, 4};
    Check(!c.add(junk, sizeof junk, &hash, &id, &seed, &back), "a short message is refused");
    MemberDataHead bad{};
    bad.op = kOpMemberData;
    bad.count = 65;
    std::vector<uint8_t> toomany(reinterpret_cast<uint8_t*>(&bad), reinterpret_cast<uint8_t*>(&bad) + sizeof bad);
    Check(!c.add(toomany.data(), toomany.size(), &hash, &id, &seed, &back), "more than 64 chunks is refused");
    bad.count = 2;
    bad.index = 2;
    std::vector<uint8_t> past(reinterpret_cast<uint8_t*>(&bad), reinterpret_cast<uint8_t*>(&bad) + sizeof bad);
    Check(!c.add(past.data(), past.size(), &hash, &id, &seed, &back), "a chunk index past the count is refused");
    Check(MemberDataEvents(1, 1, 1, std::string(kMemberChunk * kMemberMaxChunks + 1, 'x')).empty(), "a text over 64 chunks is not sent");
    const auto empty = MemberDataEvents(5, 6, 7, "");
    MemberAssembler d;
    Check(empty.size() == 1 && d.add(empty[0].data(), empty[0].size(), &hash, &id, &seed, &back) && back.empty(), "an empty text is one empty chunk");
    // the host's list of what it sent
    MemberAnnounced m;
    Check(m.needs(1, 2, 3) && !m.needs(1, 2, 3), "a member is sent once");
    Check(m.needs(1, 2, 4) && !m.needs(1, 2, 4), "and again when the slot holds another member (another seed)");
    Check(m.needs(1, 3, 3) && m.needs(2, 2, 3), "another id or another faction is another member");
    m.forgetAll();
    Check(m.needs(1, 2, 4), "a new session sends everything again");
    Check(!ClientTrackerAcceptsNemesis(), "the enemy tracker does not take member data as an enemy message (checked with the Nemesis message)");
    ClientTracker ct;
    std::vector<uint32_t> gone;
    Check(!ct.onEvent(ev[0].data(), ev[0].size(), &gone) && ct.proxies().empty(), "member data is not an enemy event either");
  }

  printf("[animation: the host's graph actions]\n");
  {
    HostTracker h;
    ClientTracker c;
    auto o = h.update({Orc(0x1000, 0, 0)}, 1000);
    std::vector<uint32_t> gone;
    for (const auto& e : o.events) c.onEvent(e.data(), e.size(), &gone);
    const uint32_t id = c.proxies().begin()->first;
    const std::vector<uint8_t> a = AnimEvent(id, "WalkForward");
    Check(a.size() == kAnimBytes && a[0] == kOpAnim, "an animation message is one fixed size");
    Check(c.onEvent(a.data(), a.size(), &gone) && strcmp(c.proxies()[id].anim, "WalkForward") == 0 && c.proxies()[id].anim_seq == 1,
          "the client keeps the action's name and counts it");
    const std::vector<uint8_t> b = AnimEvent(id, "WalkForward");
    c.onEvent(b.data(), b.size(), &gone);
    Check(c.proxies()[id].anim_seq == 2, "each message counts (the same action started again is played again)");
    const std::vector<uint8_t> long_name = AnimEvent(id, "ThisActionNameIsFarLongerThanThirtyOneBytesForSure");
    Check(strlen(reinterpret_cast<const char*>(&long_name[5])) == 31, "a long name is cut to fit");
    const std::vector<uint8_t> other = AnimEvent(9999, "Run");
    Check(c.onEvent(other.data(), other.size(), &gone) && c.proxies().size() == 1, "a message for an enemy the client does not know changes nothing");
    std::vector<uint8_t> shortmsg(kAnimBytes - 1, 0);
    shortmsg[0] = kOpAnim;
    Check(!c.onEvent(shortmsg.data(), shortmsg.size(), &gone), "a short one is refused");
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

  {
    printf("\n  Nemesis events (host to client)\n");
    NemesisMsg n{};
    for (int i = 0; i < 16; ++i) n.subj[i] = (uint8_t)(0x10 + i), n.other[i] = (uint8_t)(0x30 + i);
    n.event_hash = 0xABCD1234u;
    n.region_hash = 0x7927EEB6u;
    n.flags[0] = 1;
    n.flags[1] = 0;
    strcpy_s(n.event, "Promote");
    strcpy_s(n.subj_name, "Kruk");
    strcpy_s(n.other_name, "Pushkrimp");
    const std::vector<uint8_t> ev = NemesisEvent(n);
    NemesisMsg back{};
    Check(ev.size() == sizeof(NemesisMsg) && ev[0] == kOpNemesis && ParseNemesis(ev.data(), ev.size(), &back), "a Nemesis event is one fixed-size message that reads back");
    Check(memcmp(back.subj, n.subj, 16) == 0 && memcmp(back.other, n.other, 16) == 0 && back.event_hash == n.event_hash &&
              back.region_hash == n.region_hash && back.flags[0] == 1 && strcmp(back.event, "Promote") == 0 &&
              strcmp(back.subj_name, "Kruk") == 0 && strcmp(back.other_name, "Pushkrimp") == 0,
          "with both references, the event and region hashes, the flags and the names intact");
    Check(!ParseNemesis(ev.data(), ev.size() - 1, &back) && !ParseNemesis(ev.data(), ev.size() + 1, &back), "a message of another size is refused");
    std::vector<uint8_t> wrong = ev;
    wrong[0] = kOpHit;
    Check(!ParseNemesis(wrong.data(), wrong.size(), &back), "so is one with another op");
    NemesisMsg unnamed = n;
    unnamed.event[0] = 0;
    const std::vector<uint8_t> noname = NemesisEvent(unnamed);
    Check(!ParseNemesis(noname.data(), noname.size(), &back), "and one without an event name");
    NemesisMsg longname = n;
    memset(longname.event, 'x', sizeof longname.event);
    const std::vector<uint8_t> ln = NemesisEvent(longname);
    Check(ParseNemesis(ln.data(), ln.size(), &back) && strlen(back.event) == sizeof back.event - 1, "a name without its end is cut, not read past");
    Check(!NemesisShared("FactionMember_RoleSelected_Standard") && !NemesisShared("DEBUG_AddPIS") && !NemesisShared("IsPlayerFollowerApply") &&
              NemesisShared("Promote") && NemesisShared("ActiveGeneric_Death"),
          "the events a world load fires are not shared; promotions and deaths are");
    Check(!ClientTrackerAcceptsNemesis(), "a Nemesis message is not an enemy message (the enemy tracker ignores it)");
  }

  printf("[the game's reference lists: the destructor's unlink and its repair (listfix.h)]\n");
  {
    // A target object (its list head at +8) and n references { vtable, next, prev, self, target, pad }, linked at the tail
    // the way the game's attach (18D80C) does.
    struct Ref {
      uintptr_t vt, next, prev, self, target, pad;
    };
    struct Target {
      uintptr_t vt, next, prev;
    };
    auto make = [](Target* t, Ref* refs, int n) {
      const uintptr_t head = reinterpret_cast<uintptr_t>(&t->next);
      t->next = t->prev = head;
      for (int i = 0; i < n; ++i) {
        Ref& r = refs[i];
        const uintptr_t node = reinterpret_cast<uintptr_t>(&r.next);
        r.self = reinterpret_cast<uintptr_t>(&r);
        r.next = head;
        r.prev = t->prev;
        *reinterpret_cast<uintptr_t*>(t->prev) = node;  // old tail's next (or the head's)
        t->prev = node;
        r.target = reinterpret_cast<uintptr_t>(t);
      }
    };
    // The destructor loop (4DD1AC): the last node's owner is unlinked until the list is empty; `fixed` picks the repair or the
    // game's rule (unlink only with the target set). Returns the rounds taken (cap: did not end).
    auto drain = [](Target* t, bool fixed, int cap) {
      const uintptr_t head = reinterpret_cast<uintptr_t>(&t->next);
      int rounds = 0;
      while (t->next != head && rounds < cap) {
        const uintptr_t ref = *reinterpret_cast<uintptr_t*>(t->prev + 0x10);
        if (fixed) {
          listfix::ReferenceUnlink(ref);
        } else if (*reinterpret_cast<uintptr_t*>(ref + 0x20)) {
          listfix::ReferenceUnlink(ref);  // the same as 18D264 when the target is set
        }
        ++rounds;
      }
      return rounds;
    };
    Target t{};
    Ref refs[10]{};
    make(&t, refs, 10);
    Check(listfix::ReferenceUnlink(reinterpret_cast<uintptr_t>(&refs[4])) == listfix::Unlink::kNormal && refs[4].target == 0 &&
              refs[4].next == reinterpret_cast<uintptr_t>(&refs[4].next) && refs[3].next == reinterpret_cast<uintptr_t>(&refs[5].next) &&
              refs[5].prev == reinterpret_cast<uintptr_t>(&refs[3].next),
          "a reference with its target set is unlinked and its target cleared (what 18D264 does)");
    Check(listfix::ReferenceUnlink(reinterpret_cast<uintptr_t>(&refs[4])) == listfix::Unlink::kLeft &&
              refs[4].next == reinterpret_cast<uintptr_t>(&refs[4].next),
          "an unlinked reference with no target is left as it is");
    make(&t, refs, 10);
    refs[9].target = 0;  // the hung client's list (session cm): the last of ten linked with no target
    Check(drain(&t, false, 1000) == 1000 && t.next != reinterpret_cast<uintptr_t>(&t.next),
          "negative control: with the game's rule the loop never empties that list");
    make(&t, refs, 10);
    refs[9].target = 0;
    Check(drain(&t, true, 1000) == 10 && t.next == reinterpret_cast<uintptr_t>(&t.next) && t.prev == t.next &&
              refs[9].next == reinterpret_cast<uintptr_t>(&refs[9].next) && refs[9].prev == refs[9].next,
          "with the repair the loop ends after ten rounds, the stuck reference linked to itself");
    make(&t, refs, 10);
    refs[3].target = 0;
    refs[2].next = reinterpret_cast<uintptr_t>(&refs[5].next);  // its neighbour does not point back at it
    const uintptr_t before[2] = {refs[3].next, refs[3].prev};
    Check(listfix::ReferenceUnlink(reinterpret_cast<uintptr_t>(&refs[3])) == listfix::Unlink::kLeft && refs[3].next == before[0] &&
              refs[3].prev == before[1] && refs[4].prev == reinterpret_cast<uintptr_t>(&refs[3].next),
          "a reference with no target whose neighbours do not point back at it is not touched");
  }

  printf("[exact losses (session cm)]\n");
  {
    const float at[3] = {1, 2, 3};
    const std::vector<uint8_t> e = LossEvent(42, 60.5f, at);
    uint32_t id = 0;
    float amount = 0, pos[3] = {};
    Check(e.size() == sizeof(HitMsg) && e[0] == kOpLoss, "a loss event has a hit's size and its own op");
    Check(ParseLoss(e.data(), e.size(), &id, &amount, pos) && id == 42 && amount == 60.5f && pos[2] == 3,
          "a loss event reads back: enemy, amount, place");
    HitMsg h;
    Check(!ParseHit(e.data(), e.size(), &h), "a loss event is not taken for a hit");
    Check(!ParseLoss(HitEvent(42, 7, at).data(), sizeof(HitMsg), &id, &amount, pos), "a hit is not taken for a loss");
    const std::vector<uint8_t> neg = LossEvent(42, -5.0f, at), huge = LossEvent(42, 1e9f, at);
    Check(!ParseLoss(neg.data(), neg.size(), &id, &amount, pos) && !ParseLoss(huge.data(), huge.size(), &id, &amount, pos),
          "a negative or absurd loss is refused");
  }
  printf("[co-op rules (coop_rules.h, session cm)]\n");
  {
    using namespace cooprules;
    SwingSeen last;
    const uintptr_t orc = 0x1000, other = 0x2000, standin = 0x3000, decoy = 0x4000;
    Check(!SameSwingAgain(&last, orc, 7, standin, 1000), "a first report of a swing is sent");
    Check(SameSwingAgain(&last, orc, 7, decoy, 1040), "the same attacker and attack on the other victim 40 ms later is the same swing");
    Check(!SameSwingAgain(&last, orc, 7, decoy, 1400), "the same swing reaching it again 400 ms after the first report is sent");
    Check(!SameSwingAgain(&last, orc, 7, decoy, 1450), "a second hit on the same victim is not dropped as a duplicate");
    Check(!SameSwingAgain(&last, other, 7, standin, 1460), "another attacker's swing a moment later is sent");
    Check(!SameSwingAgain(&last, other, 8, decoy, 1470), "another attack of that attacker is sent");
    Check(BystanderKind("Warg_Tamed", 10), "a tamed warg is a bystander");
    Check(BystanderKind("Orc_Prisoner", 12), "a prisoner is a bystander");
    Check(!BystanderKind("Orc_Infantry", 12), "a grunt is not a bystander");
    char full[24];
    memcpy(full, "Orc_Slave_Without_A_Zero", sizeof full);  // a type that fills its field, no terminating zero
    Check(BystanderKind(full, sizeof full), "a type without a terminating zero is read safely");
    Check(LocalLossReported(10000, 0, false), "a loss with no strike before it is reported");
    Check(!LocalLossReported(10000, 8000, false), "a loss 2 s after a reported strike is not (a counter kill lands late)");
    Check(LocalLossReported(10000, 7000, false), "a loss 3 s after a strike is reported");
    Check(!LocalLossReported(10000, 0, true), "a bystander's loss is never reported");
    {
      float peak = 0;
      uint64_t peak_ms = 0;
      TrackPeak(&peak, &peak_ms, 500.0f, 1000);
      TrackPeak(&peak, &peak_ms, 480.0f, 1500);
      Check(!RecentFall(peak, peak_ms, 480.0f), "a proxy 20 units below its peak has not fallen");
      Check(RecentFall(peak, peak_ms, 340.0f), "a proxy 160 units below its peak a moment later has fallen");
      TrackPeak(&peak, &peak_ms, 340.0f, 3600);
      Check(!RecentFall(peak, peak_ms, 340.0f), "after two seconds the peak is where it stands now");
      Check(!RecentFall(0.0f, 0, -500.0f), "no peak yet: no fall");
    }
    Check(StockStrikes(50.0f) == 1 && StockStrikes(181.0f) == 3 && StockStrikes(5000.0f) == 6, "stock strikes: one per 90, 1 to 6");
    int no_land = 0;
    for (int i = 0; i < kNoLandFrames; ++i) no_land = NoLandStep(no_land, true, 2000 + 16 * i, 1990 + 16 * i, 0.0f);
    Check(no_land == kNoLandFrames, "put-backs that leave the stand-in in place count up to the direct writes");
    Check(NoLandStep(no_land, true, 3000, 2990, 25.0f) == 0, "a put-back that moved it starts the count again");
    Check(NoLandStep(no_land, false, 3000, 2990, 0.0f) == 0, "a stand-in close enough clears the count");
    Check(NoLandStep(5, true, 5000, 3000, 0.0f) == 0, "a put-back older than a second does not count");
    Check(CounterGivesBack(10000, 9200, 400.0f, 300.0f), "a counter 0.8 s after the swing gives the health back");
    Check(!CounterGivesBack(10000, 8000, 400.0f, 300.0f), "a counter 2 s after the swing does not");
    Check(!CounterGivesBack(10000, 9200, 400.0f, 400.0f), "nothing is given back when nothing was lost");
    Check(!CounterGivesBack(10000, 0, 400.0f, 300.0f), "nor without a swing of that enemy");
  }
  printf("\nRESULT: %s (%d of %d checks passed)\n", g_bad == 0 ? "PASS" : "FAIL", g_ok, g_ok + g_bad);
  return g_bad == 0 ? 0 : 1;
}
