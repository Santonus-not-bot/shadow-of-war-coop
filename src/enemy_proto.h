// What the host tells the client about its enemies (STAGE2-DESIGN.md), without any game code, so it can be tested
// alone. Two kinds of message over the transport (net_proto.h):
//   - EVENT (reliable, ordered): NEW {id, type name} when an enemy comes into range, GONE {id} when it leaves or
//     is freed. The client spawns a proxy of that type for each NEW and frees it at GONE.
//   - STATE2 (newest wins, lossy): a snapshot of up to 16 enemies: position, heading, health, the attack record
//     it is in, and how it moves (idle, walk, run). An id the client has not heard NEW for is ignored.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace sowenemy {

constexpr size_t kMaxEnemies = 16;
constexpr size_t kTypeBytes = 32;
constexpr size_t kNameBytes = 40;
constexpr uint32_t kNoMember = 0xFFFFFFFFu;
enum Op : uint8_t { kOpNew = 1, kOpGone = 2, kOpHit = 3, kOpDamage = 4, kOpNemesis = 5, kOpMember = 6, kOpMemberData = 7 };
enum Loco : uint8_t { kIdle = 0, kWalk = 1, kRun = 2 };
enum Flag : uint8_t { kAlive = 1 };

#pragma pack(push, 1)
struct Rec {
  uint32_t id;
  float pos[3];
  float yaw;             // radians
  uint32_t attack_id;    // the attack record the enemy is in (0: none)
  uint32_t attack_stamp; // its start time: a repeated record has a new stamp
  uint16_t hp;           // health as a fraction of its maximum, 0..65535
  uint8_t flags;
  uint8_t loco;
};
struct Snapshot {
  char magic[4];  // "SOWE"
  uint32_t seq;
  uint8_t count;
  uint8_t pad[3];
  Rec rec[kMaxEnemies];
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 32, "Rec is 32 bytes");
constexpr size_t kSnapshotHead = 12;

inline size_t SnapshotBytes(unsigned count) { return kSnapshotHead + count * sizeof(Rec); }

// The client's strike on a proxy, to the host (reliable, client to host): the enemy's id, the attack record that hit
// (its name hash, the same in every copy) and where.
#pragma pack(push, 1)
struct HitMsg {
  uint8_t op;
  uint32_t id;
  uint32_t attack_id;
  float pos[3];
};
#pragma pack(pop)
static_assert(sizeof(HitMsg) == 21, "HitMsg is 21 bytes");

inline std::vector<uint8_t> HitEvent(uint32_t id, uint32_t attack_id, const float pos[3]) {
  HitMsg m;
  m.op = kOpHit;
  m.id = id;
  m.attack_id = attack_id;
  memcpy(m.pos, pos, sizeof m.pos);
  return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&m), reinterpret_cast<uint8_t*>(&m) + sizeof m);
}

// An enemy's swing that reached the client's stand-in on the host, to the client (reliable, host to client): the enemy's
// id, the attack record and where. Same layout as a hit.
inline std::vector<uint8_t> DamageEvent(uint32_t id, uint32_t attack_id, const float pos[3]) {
  std::vector<uint8_t> e = HitEvent(id, attack_id, pos);
  e[0] = kOpDamage;
  return e;
}

inline bool ParseHitOp(const uint8_t* p, size_t n, uint8_t op, HitMsg* out) {
  if (n != sizeof(HitMsg) || p[0] != op) return false;
  memcpy(out, p, sizeof *out);
  for (float v : out->pos)
    if (!std::isfinite(v)) return false;
  return out->id != 0;
}

inline bool ParseHit(const uint8_t* p, size_t n, HitMsg* out) { return ParseHitOp(p, n, kOpHit, out); }
inline bool ParseDamage(const uint8_t* p, size_t n, HitMsg* out) { return ParseHitOp(p, n, kOpDamage, out); }

// A Nemesis event the host's game applied (HandleFactionEventAction), to the client (reliable, host to client): the two
// member references as the game holds them (16 bytes each), the two flag bytes, the event record's name and the members'
// names (for logs and for finding the member again).
#pragma pack(push, 1)
struct NemesisMsg {
  uint8_t op;
  uint8_t subj[16], other[16];  // portable references (SimFactionEvent)
  uint32_t event_hash, region_hash;
  uint8_t flags[2];
  char event[48], subj_name[40], other_name[40];
};
#pragma pack(pop)
static_assert(sizeof(NemesisMsg) == 171, "NemesisMsg is 171 bytes");

inline std::vector<uint8_t> NemesisEvent(const NemesisMsg& in) {
  NemesisMsg m = in;
  m.op = kOpNemesis;
  m.event[sizeof m.event - 1] = m.subj_name[sizeof m.subj_name - 1] = m.other_name[sizeof m.other_name - 1] = 0;
  return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&m), reinterpret_cast<uint8_t*>(&m) + sizeof m);
}

inline bool ParseNemesis(const uint8_t* p, size_t n, NemesisMsg* out) {
  if (n != sizeof(NemesisMsg) || p[0] != kOpNemesis) return false;
  memcpy(out, p, sizeof *out);
  out->event[sizeof out->event - 1] = out->subj_name[sizeof out->subj_name - 1] = out->other_name[sizeof out->other_name - 1] = 0;
  return out->event[0] != 0;
}

// MEMBERDATA (reliable, host to client): a Nemesis member as the game's own text (what FactionMember.Save writes), in
// chunks of up to kMemberChunk bytes, for a member the client may not have (one the host's game made after the client
// joined, or one that took another's slot). The host sends it before the first message that names the member and again
// if the member in that slot is another one (its seed differs); the client builds the member in that slot unless it
// already has one with that seed.
constexpr size_t kMemberChunk = 900;
constexpr size_t kMemberMaxChunks = 64;
#pragma pack(push, 1)
struct MemberDataHead {
  uint8_t op;
  uint32_t faction_hash, id;
  uint64_t seed;
  uint16_t index, count;
};
#pragma pack(pop)
static_assert(sizeof(MemberDataHead) == 21, "MemberDataHead is 21 bytes");

inline std::vector<std::vector<uint8_t>> MemberDataEvents(uint32_t faction_hash, uint32_t id, uint64_t seed, const std::string& text) {
  std::vector<std::vector<uint8_t>> out;
  const size_t count = std::max<size_t>(1, (text.size() + kMemberChunk - 1) / kMemberChunk);
  if (count > kMemberMaxChunks) return out;
  for (size_t i = 0; i < count; ++i) {
    MemberDataHead h{};
    h.op = kOpMemberData;
    h.faction_hash = faction_hash;
    h.id = id;
    h.seed = seed;
    h.index = (uint16_t)i;
    h.count = (uint16_t)count;
    const size_t from = i * kMemberChunk, len = std::min(kMemberChunk, text.size() - std::min(text.size(), from));
    std::vector<uint8_t> e(sizeof h + len);
    memcpy(e.data(), &h, sizeof h);
    if (len) memcpy(e.data() + sizeof h, text.data() + from, len);
    out.push_back(std::move(e));
  }
  return out;
}

// The client's side: puts the chunks of one member together (they arrive in order). add() is true when the last chunk of a
// member came; the member is then in the out parameters. A chunk out of order drops what was gathered.
class MemberAssembler {
 public:
  bool add(const uint8_t* p, size_t n, uint32_t* faction_hash, uint32_t* id, uint64_t* seed, std::string* text) {
    if (n < sizeof(MemberDataHead) || p[0] != kOpMemberData) return false;
    MemberDataHead h;
    memcpy(&h, p, sizeof h);
    if (h.count == 0 || h.count > kMemberMaxChunks || h.index >= h.count || n - sizeof h > kMemberChunk) {
      reset();
      return false;
    }
    if (h.index == 0) {
      reset();
      hash_ = h.faction_hash;
      id_ = h.id;
      seed_ = h.seed;
      count_ = h.count;
    } else if (!active_ || h.faction_hash != hash_ || h.id != id_ || h.seed != seed_ || h.count != count_ || h.index != next_) {
      reset();
      return false;
    }
    active_ = true;
    text_.append(reinterpret_cast<const char*>(p + sizeof h), n - sizeof h);
    next_ = h.index + 1;
    if (next_ < count_) return false;
    *faction_hash = hash_;
    *id = id_;
    *seed = seed_;
    *text = std::move(text_);
    reset();
    return true;
  }
  void reset() {
    active_ = false;
    text_.clear();
    next_ = 0;
  }

 private:
  bool active_ = false;
  uint32_t hash_ = 0, id_ = 0;
  uint64_t seed_ = 0;
  uint16_t count_ = 0, next_ = 0;
  std::string text_;
};

// The host's side: which members it has told the client about, by faction and id, with the seed it told.
class MemberAnnounced {
 public:
  // True when the member (faction, id, seed) must be sent now (never sent, or the slot holds another member now).
  bool needs(uint32_t faction_hash, uint32_t id, uint64_t seed) {
    auto key = ((uint64_t)faction_hash << 32) | id;
    auto it = sent_.find(key);
    if (it != sent_.end() && it->second == seed) return false;
    sent_[key] = seed;
    return true;
  }
  void forgetAll() { sent_.clear(); }
  size_t size() const { return sent_.size(); }

 private:
  std::unordered_map<uint64_t, uint64_t> sent_;
};

// The events the game fires while a world loads (roles chosen for every member, followers set up) are not shared: the
// client loads the same save and does the same.
inline bool NemesisShared(const char* event) {
  static const char* const kSkip[] = {"FactionMember_RoleSelected", "Debug_", "DEBUG_", "IsPlayerFollowerApply", "DLC3_"};
  for (const char* s : kSkip)
    if (strncmp(event, s, strlen(s)) == 0) return false;
  return true;
}

// What the game side reads about one enemy.
struct Seen {
  uintptr_t key = 0;  // the character's address while it lives
  float pos[3] = {};
  float yaw = 0;
  uint32_t attack_id = 0, attack_stamp = 0;
  float hp_frac = 1.0f;
  bool alive = true;
  char type[kTypeBytes] = {};
  uint32_t member_id = kNoMember;  // a captain: his Nemesis member (the id in his faction's table), else kNoMember
  uint32_t faction_hash = 0;       // the name hash of that faction's record
  char name[kNameBytes] = {};      // his name
};

inline void CopyType(char out[kTypeBytes], const char* name) {
  memset(out, 0, kTypeBytes);
  for (size_t i = 0; name && name[i] && i + 1 < kTypeBytes; ++i) out[i] = name[i];
}
inline void CopyName(char out[kNameBytes], const char* name) {
  memset(out, 0, kNameBytes);
  for (size_t i = 0; name && name[i] && i + 1 < kNameBytes; ++i) out[i] = name[i];
}

// MEMBER (reliable, host to client): the Nemesis member behind an enemy, announced after its NEW (and again if it changes):
// the faction record's name hash and the member's id, which mean the same in a copy of the same save, and his name (for
// logs and to check it is the same member). The client builds the proxy from its own member with that id.
constexpr size_t kMemberBytes = 1 + 4 + 4 + 4 + kNameBytes;
inline std::vector<uint8_t> MemberEvent(uint32_t id, uint32_t faction_hash, uint32_t member_id, const char name[kNameBytes]) {
  std::vector<uint8_t> e(kMemberBytes);
  e[0] = kOpMember;
  memcpy(&e[1], &id, 4);
  memcpy(&e[5], &faction_hash, 4);
  memcpy(&e[9], &member_id, 4);
  memcpy(&e[13], name, kNameBytes);
  return e;
}

// The speeds (units a second) that separate idle, walk and run; an orc walks at about 200 and runs at about 500.
constexpr float kWalkSpeed = 40.0f, kRunSpeed = 350.0f;

// ---- host ---------------------------------------------------------------------------------------------------

class HostTracker {
 public:
  struct Out {
    std::vector<std::vector<uint8_t>> events;  // reliable, in order
    std::vector<uint8_t> snapshot;             // empty: nothing to send
  };

  // `chosen`: the enemies to show now (the caller has limited them to kMaxEnemies). Entries not in it are gone.
  Out update(const std::vector<Seen>& chosen, uint64_t now_ms) {
    Out out;
    std::unordered_map<uintptr_t, bool> present;
    Snapshot snap;
    memset(&snap, 0, sizeof snap);
    memcpy(snap.magic, "SOWE", 4);
    snap.seq = ++seq_;
    for (const Seen& s : chosen) {
      if (snap.count >= kMaxEnemies) break;
      present[s.key] = true;
      auto it = map_.find(s.key);
      if (it != map_.end() && strncmp(it->second.type, s.type, kTypeBytes) != 0) {  // the address was reused
        out.events.push_back(goneEvent(it->second.id));
        map_.erase(it);
        it = map_.end();
      }
      if (it == map_.end()) {
        Entry e;
        e.id = next_id_++;
        CopyType(e.type, s.type);
        memcpy(e.last, s.pos, sizeof e.last);
        e.last_ms = now_ms;
        e.loco = kIdle;
        it = map_.emplace(s.key, e).first;
        out.events.push_back(newEvent(e.id, e.type));
      }
      Entry& e = it->second;
      if (s.member_id != kNoMember && (e.member_id != s.member_id || e.faction_hash != s.faction_hash)) {
        e.member_id = s.member_id;
        e.faction_hash = s.faction_hash;
        out.events.push_back(MemberEvent(e.id, s.faction_hash, s.member_id, s.name));
      }
      const uint64_t dt = now_ms - e.last_ms;
      if (dt >= 60) {
        const float dx = s.pos[0] - e.last[0], dz = s.pos[2] - e.last[2];
        const float speed = std::sqrt(dx * dx + dz * dz) * 1000.0f / (float)dt;
        e.speed = e.speed * 0.5f + speed * 0.5f;
        e.loco = e.speed >= kRunSpeed ? kRun : e.speed >= kWalkSpeed ? kWalk : kIdle;
        memcpy(e.last, s.pos, sizeof e.last);
        e.last_ms = now_ms;
      }
      Rec& r = snap.rec[snap.count++];
      r.id = e.id;
      memcpy(r.pos, s.pos, sizeof r.pos);
      r.yaw = s.yaw;
      r.attack_id = s.attack_id;
      r.attack_stamp = s.attack_stamp;
      r.hp = (uint16_t)std::max(0.0f, std::min(65535.0f, s.hp_frac * 65535.0f));
      r.flags = s.alive ? kAlive : 0;
      r.loco = e.loco;
    }
    for (auto it = map_.begin(); it != map_.end();) {
      if (!present.count(it->first)) {
        out.events.push_back(goneEvent(it->second.id));
        it = map_.erase(it);
      } else {
        ++it;
      }
    }
    if (snap.count) {
      out.snapshot.assign(reinterpret_cast<const uint8_t*>(&snap), reinterpret_cast<const uint8_t*>(&snap) + SnapshotBytes(snap.count));
    }
    return out;
  }

  // The id announced for a character (0: not one of the enemies announced now).
  uint32_t idOf(uintptr_t key) const {
    auto it = map_.find(key);
    return it == map_.end() ? 0 : it->second.id;
  }

  // The character a client's hit is for (0: not one of the enemies announced now).
  uintptr_t keyOf(uint32_t id) const {
    for (const auto& kv : map_)
      if (kv.second.id == id) return kv.first;
    return 0;
  }

  // A new session: the client knows nothing, so every enemy is announced again (with new ids).
  void forgetAll() { map_.clear(); }
  size_t known() const { return map_.size(); }

 private:
  struct Entry {
    uint32_t id = 0;
    char type[kTypeBytes] = {};
    uint32_t member_id = kNoMember, faction_hash = 0;
    float last[3] = {};
    uint64_t last_ms = 0;
    float speed = 0;
    uint8_t loco = kIdle;
  };
  static std::vector<uint8_t> newEvent(uint32_t id, const char type[kTypeBytes]) {
    std::vector<uint8_t> e(1 + 4 + kTypeBytes);
    e[0] = kOpNew;
    memcpy(&e[1], &id, 4);
    memcpy(&e[5], type, kTypeBytes);
    return e;
  }
  static std::vector<uint8_t> goneEvent(uint32_t id) {
    std::vector<uint8_t> e(1 + 4);
    e[0] = kOpGone;
    memcpy(&e[1], &id, 4);
    return e;
  }
  std::unordered_map<uintptr_t, Entry> map_;
  uint32_t next_id_ = 1, seq_ = 0;
};

// ---- client -------------------------------------------------------------------------------------------------

// What the client knows of each enemy. The game side owns `handle` (the proxy character; 0 until spawned).
struct Proxy {
  uint32_t id = 0;
  char type[kTypeBytes] = {};
  uint32_t member_id = kNoMember, faction_hash = 0;  // the captain behind it, once the host has said
  char name[kNameBytes] = {};
  uint32_t built_member = kNoMember;  // the member the spawned proxy was built from (the game side)
  bool member_failed = false;         // this copy has no such member: the proxy is the type's generic orc
  uintptr_t handle = 0;
  bool spawn_failed = false;
  bool have = false;  // a snapshot record has arrived
  Rec cur{}, prev{};
  uint64_t cur_ms = 0, prev_ms = 0;
  uint32_t applied_attack = 0, applied_stamp = 0;
  uintptr_t applied_record = 0;  // the attack record started on the proxy (to stop it)
  uint64_t next_spawn_ms = 0;
  uint8_t applied_loco = 255;
  float shown[3] = {};
  bool shown_set = false;
};

class ClientTracker {
 public:
  std::unordered_map<uint32_t, Proxy>& proxies() { return map_; }

  // An EVENT body. Returns false when it is not ours or malformed (nothing changes then).
  bool onEvent(const uint8_t* p, size_t n, std::vector<uint32_t>* gone) {
    if (n < 5) return false;
    uint32_t id;
    memcpy(&id, p + 1, 4);
    if (p[0] == kOpNew && n == 1 + 4 + kTypeBytes) {
      Proxy& pr = map_[id];
      pr.id = id;
      memcpy(pr.type, p + 5, kTypeBytes);
      pr.type[kTypeBytes - 1] = 0;
      return true;
    }
    if (p[0] == kOpMember && n == kMemberBytes) {
      auto it = map_.find(id);
      if (it == map_.end()) return true;  // an enemy already gone: nothing to do, but it is ours
      Proxy& pr = it->second;
      memcpy(&pr.faction_hash, p + 5, 4);
      memcpy(&pr.member_id, p + 9, 4);
      memcpy(pr.name, p + 13, kNameBytes);
      pr.name[kNameBytes - 1] = 0;
      pr.member_failed = false;
      return true;
    }
    if (p[0] == kOpGone && n == 1 + 4) {
      if (map_.count(id) && gone) gone->push_back(id);
      map_.erase(id);
      return true;
    }
    return false;
  }

  // A STATE2 body; returns how many records were taken (unknown ids are ignored, a stale snapshot is dropped).
  unsigned onSnapshot(const uint8_t* p, size_t n, uint64_t now_ms) {
    if (n < kSnapshotHead || memcmp(p, "SOWE", 4) != 0) return 0;
    Snapshot s;
    memset(&s, 0, sizeof s);
    memcpy(&s, p, std::min(n, sizeof s));
    if (s.count > kMaxEnemies || n < SnapshotBytes(s.count)) return 0;
    if (have_seq_ && (int32_t)(s.seq - last_seq_) <= 0) return 0;  // older than one already taken
    have_seq_ = true;
    last_seq_ = s.seq;
    unsigned taken = 0;
    for (unsigned i = 0; i < s.count; ++i) {
      auto it = map_.find(s.rec[i].id);
      if (it == map_.end()) continue;
      Proxy& pr = it->second;
      for (float v : s.rec[i].pos)
        if (!std::isfinite(v)) goto next;
      if (pr.have) {
        pr.prev = pr.cur;
        pr.prev_ms = pr.cur_ms;
      }
      pr.cur = s.rec[i];
      pr.cur_ms = now_ms;
      if (!pr.have) {
        pr.prev = pr.cur;
        pr.prev_ms = now_ms;
      }
      pr.have = true;
      ++taken;
    next:;
    }
    return taken;
  }

  void clear() {
    map_.clear();
    have_seq_ = false;
  }

  // Where the proxy should be at now_ms: the newest record carried forward by the speed of the last two (up to
  // 150 ms), so a snapshot every 66 ms looks continuous.
  static void Predict(const Proxy& p, uint64_t now_ms, float out[3]) {
    memcpy(out, p.cur.pos, sizeof p.cur.pos);
    if (p.cur_ms <= p.prev_ms) return;
    const float span = (float)(p.cur_ms - p.prev_ms);
    const float ahead = (float)std::min<uint64_t>(now_ms > p.cur_ms ? now_ms - p.cur_ms : 0, 150);
    for (int i = 0; i < 3; ++i) out[i] += (p.cur.pos[i] - p.prev.pos[i]) / span * ahead;
  }

 private:
  std::unordered_map<uint32_t, Proxy> map_;
  uint32_t last_seq_ = 0;
  bool have_seq_ = false;
};

}  // namespace sowenemy
