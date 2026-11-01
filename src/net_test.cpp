// Simulator tests for the co-op transport (src/net_proto.h): two endpoints joined by a virtual link that
// loses, delays, reorders, duplicates and corrupts datagrams from a seeded generator, in virtual time (one
// step = 1 ms). No sockets, no clocks, no game. Build: build.cmd (net_test.exe); a negative-control build
// (SOWNET_MUTANT: reliable messages not ordered or de-duplicated) must fail.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "net_proto.h"

using namespace sownet;

namespace {

int g_checks = 0, g_failed = 0;
double g_loss = 0.08;  // net_test.exe [seeds] [loss]: a harder sweep than the committed one
int g_seeds = 40;

bool Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    printf("    FAIL: %s\n", what);
  }
  return ok;
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
  double unit() { return (double)(next() >> 11) / (double)(1ull << 53); }
};

struct LinkParams {
  double latency = 0, jitter = 0, loss = 0, burst_enter = 0, burst_exit = 0.4, reorder = 0, dup = 0, corrupt = 0;
};

struct Delivery {
  int to;  // 0 host, 1 client
  std::vector<uint8_t> data;
};

struct Link {
  LinkParams p;
  Rng rng;
  bool bad = false;
  bool cut = false;  // a partition: everything is lost
  explicit Link(uint64_t seed) : rng(seed) {}
  // Returns the delays (ms) of the copies to deliver: none when lost.
  std::vector<std::vector<uint8_t>> mutated;
  std::vector<double> decide(std::vector<uint8_t>* data) {
    mutated.clear();
    if (cut) return {};
    if (bad) { if (rng.unit() < p.burst_exit) bad = false; }
    else if (rng.unit() < p.burst_enter) bad = true;
    if (rng.unit() < (bad ? 0.9 : p.loss)) return {};
    std::vector<double> d;
    const int copies = rng.unit() < p.dup ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
      double delay = p.latency + (rng.unit() * 2 - 1) * p.jitter;
      if (rng.unit() < p.reorder) delay = rng.unit() * p.latency * 0.5;
      d.push_back(delay < 0 ? 0 : delay);
    }
    if (rng.unit() < p.corrupt && !data->empty()) (*data)[(size_t)(rng.next() % data->size())] ^= (uint8_t)(1u << (rng.next() % 8));
    return d;
  }
};

Config MakeConfig(Role r, const char* password = "pw", uint8_t exe = 7, uint32_t build = 1) {
  Config c;
  c.role = r;
  memset(c.exe_sha, exe, 32);
  c.mod_build = build;
  c.password = password;
  c.session_seed = 12345;
  return c;
}

struct World {
  uint64_t now = 0;
  Link h2c, c2h;
  std::multimap<uint64_t, Delivery> inflight;
  Endpoint host, client;
  std::vector<uint32_t> host_events, client_events;
  uint32_t host_states = 0, client_states = 0, host_sent_events = 0, client_sent_events = 0;
  std::vector<Status> client_status, host_status;
  std::vector<std::vector<uint8_t>> to_stranger;  // what the host sends to source 77

  World(uint64_t seed, const LinkParams& lp, const Config& hc, const Config& cc)
      : h2c(seed * 2 + 1), c2h(seed * 2 + 2),
        host(hc, [this](const uint8_t* d, size_t n, uint64_t dst) {
          if (dst == 77) to_stranger.emplace_back(d, d + n);
          else push(h2c, 1, d, n);
        }),
        client(cc, [this](const uint8_t* d, size_t n, uint64_t) { push(c2h, 0, d, n); }) {
    h2c.p = c2h.p = lp;
    host.onEvent([this](const uint8_t* p, size_t n) { if (n == 4) { uint32_t v; memcpy(&v, p, 4); host_events.push_back(v); } });
    client.onEvent([this](const uint8_t* p, size_t n) { if (n == 4) { uint32_t v; memcpy(&v, p, 4); client_events.push_back(v); } });
    host.onState([this](const uint8_t*, size_t) { ++host_states; });
    client.onState([this](const uint8_t*, size_t) { ++client_states; });
    host.onStatus([this](Status s) { host_status.push_back(s); });
    client.onStatus([this](Status s) { client_status.push_back(s); });
  }

  void push(Link& l, int to, const uint8_t* d, size_t n) {
    std::vector<uint8_t> data(d, d + n);
    for (double delay : l.decide(&data)) inflight.insert({now + (uint64_t)(delay + 0.5), {to, data}});
  }

  void step() {
    ++now;
    while (!inflight.empty() && inflight.begin()->first <= now) {
      Delivery dv = std::move(inflight.begin()->second);
      inflight.erase(inflight.begin());
      if (dv.to == 0) host.receive(dv.data.data(), dv.data.size(), now, 1);
      else client.receive(dv.data.data(), dv.data.size(), now, 0);
    }
    host.tick(now);
    client.tick(now);
  }

  bool runUntil(const std::function<bool()>& done, uint64_t max_ms) {
    for (uint64_t i = 0; i < max_ms; ++i) {
      step();
      if (done()) return true;
    }
    return false;
  }

  void sendEvents(int n) {
    for (int i = 0; i < n; ++i) {
      uint32_t a = host_sent_events++, b = client_sent_events++;
      host.sendEvent(reinterpret_cast<uint8_t*>(&a), 4);
      client.sendEvent(reinterpret_cast<uint8_t*>(&b), 4);
    }
  }
  bool eventsComplete() const {
    if (host_events.size() != client_sent_events || client_events.size() != host_sent_events) return false;
    return true;
  }
  bool eventsInOrder() const {
    for (size_t i = 0; i < host_events.size(); ++i) if (host_events[i] != i) return false;
    for (size_t i = 0; i < client_events.size(); ++i) if (client_events[i] != i) return false;
    return true;
  }
};

std::vector<uint8_t> MakeSave(size_t n, uint64_t seed) {
  Rng r(seed);
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = (uint8_t)r.next();
  return v;
}

bool Connected(const World& w) {
  return w.host.status() == Status::kConnected && w.client.status() == Status::kConnected;
}

LinkParams Clean() { LinkParams p; p.latency = 25; return p; }
LinkParams Bad() {
  LinkParams p;
  p.latency = 45; p.jitter = 30; p.loss = g_loss; p.burst_enter = 0.01; p.burst_exit = 0.3;
  p.reorder = 0.06; p.dup = 0.05; p.corrupt = 0.03;
  return p;
}

// Join, download the save, exchange events and states, all over the given link.
bool JoinAndTalk(uint64_t seed, const LinkParams& lp, size_t save_size, uint64_t budget_ms, std::string* why) {
  World w(seed, lp, MakeConfig(Role::kHost), MakeConfig(Role::kClient));
  const std::vector<uint8_t> save = MakeSave(save_size, seed);
  w.host.offerSave(save);
  if (!w.runUntil([&] { return Connected(w); }, budget_ms)) { *why = "never connected"; return false; }
  if (w.client.receivedSave() != save) { *why = "the save differs"; return false; }
  w.sendEvents(200);
  for (int i = 0; i < 300; ++i) {
    uint8_t st[40] = {(uint8_t)i};
    w.host.sendState(st, sizeof st);
    w.client.sendState(st, sizeof st);
    for (int k = 0; k < 33; ++k) w.step();
  }
  if (!w.runUntil([&] { return w.eventsComplete(); }, budget_ms)) { *why = "events incomplete"; return false; }
  if (!w.eventsInOrder()) { *why = "events out of order, lost or doubled"; return false; }
  if (w.host_events.size() != 200 || w.client_events.size() != 200) { *why = "wrong event count"; return false; }
  if (w.host_states == 0 || w.client_states == 0) { *why = "no state arrived"; return false; }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) g_seeds = atoi(argv[1]);
  if (argc > 2) g_loss = atof(argv[2]);
  printf("[clean link: join, a 2.3 MB save, events, states]\n");
  {
    std::string why;
    Check(JoinAndTalk(1, Clean(), 2300000, 20000, &why), why.c_str());
    World w(1, Clean(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    w.host.offerSave(MakeSave(2300000, 1));
    uint64_t at = 0;
    w.runUntil([&] { return Connected(w); }, 20000);
    at = w.now;
    Check(at < 6000, "the save of 2.3 MB arrives in under 6 s at 50 ms round trip");
    Check(w.client.saveSize() == 2300000, "the client knows the save size");
  }

  printf("[bad link: loss, bursts, reordering, duplicates, corruption, 40 seeds]\n");
  {
    int ok = 0;
    std::string first_why;
    for (uint64_t seed = 1; seed <= (uint64_t)g_seeds; ++seed) {
      std::string why;
      if (JoinAndTalk(seed, Bad(), 400000, 120000, &why)) ++ok;
      else if (first_why.empty()) first_why = "seed " + std::to_string(seed) + ": " + why;
    }
    char b[160];
    snprintf(b, sizeof b, "all %d seeds joined and delivered every event once, in order (%d did; %s)", g_seeds, ok, first_why.c_str());
    Check(ok == g_seeds, b);
  }

  printf("[an outage: 3 s keeps the connection, 10 s loses it, the client resumes without the save]\n");
  {
    World w(5, Clean(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    const std::vector<uint8_t> save = MakeSave(300000, 5);
    w.host.offerSave(save);
    Check(w.runUntil([&] { return Connected(w); }, 20000), "connected");
    w.sendEvents(20);
    w.runUntil([&] { return w.eventsComplete(); }, 5000);
    // 3 s: events sent during it arrive after it
    w.h2c.cut = w.c2h.cut = true;
    w.sendEvents(10);
    for (int i = 0; i < 3000; ++i) w.step();
    Check(Connected(w), "a 3 s outage does not drop the connection");
    w.h2c.cut = w.c2h.cut = false;
    Check(w.runUntil([&] { return w.eventsComplete(); }, 10000), "events sent during the outage arrive after it");
    // 10 s
    const size_t downloads = w.client.saveProgress();
    w.h2c.cut = w.c2h.cut = true;
    w.sendEvents(10);
    for (int i = 0; i < 10000; ++i) w.step();
    Check(w.client.status() == Status::kLost && w.host.status() == Status::kLost, "a 10 s outage makes both sides Lost");
    w.h2c.cut = w.c2h.cut = false;
    Check(w.runUntil([&] { return Connected(w); }, 10000), "both resume when the link is back");
    Check(w.client.saveProgress() == downloads, "the save is not downloaded again");
    Check(w.runUntil([&] { return w.eventsComplete(); }, 10000), "events sent during the long outage arrive after the resume");
    Check(w.eventsInOrder(), "all of them, once and in order");
  }

  printf("[rejections: wrong password, other game build, other mod build, a second client]\n");
  {
    auto attempt = [&](const Config& cc, Status want, const char* what, Reject reason) {
      World w(9, Clean(), MakeConfig(Role::kHost), cc);
      w.host.offerSave(MakeSave(1000, 9));
      w.runUntil([&] { return w.client.status() == Status::kRejected; }, 3000);
      char b[120];
      snprintf(b, sizeof b, "%s is rejected", what);
      Check(w.client.status() == want && w.client.rejectReason() == reason, b);
      Check(w.host.status() == Status::kIdle, "and the host is still waiting");
    };
    attempt(MakeConfig(Role::kClient, "other"), Status::kRejected, "a wrong password", kRejectPassword);
    attempt(MakeConfig(Role::kClient, "pw", 9), Status::kRejected, "another game build", kRejectProto);
    attempt(MakeConfig(Role::kClient, "pw", 7, 2), Status::kRejected, "another mod build", kRejectBuild);
    // A second client while the first is connected is told "busy" and does not disturb the first.
    World w(10, Clean(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    w.host.offerSave(MakeSave(1000, 10));
    w.runUntil([&] { return Connected(w); }, 5000);
    Endpoint stranger(MakeConfig(Role::kClient), [&](const uint8_t* d, size_t n, uint64_t) { w.host.receive(d, n, w.now, 77); });
    stranger.tick(w.now);
    for (const auto& dg : w.to_stranger) stranger.receive(dg.data(), dg.size(), w.now, 0);
    Check(stranger.status() == Status::kConnecting && stranger.rejectReason() == kRejectBusy,
          "a second client is told the host is busy, and keeps asking");
    w.sendEvents(20);
    Check(w.runUntil([&] { return w.eventsComplete(); }, 5000) && Connected(w), "the first client's connection carries on");
  }

  printf("[a restarted client takes the place of its dead predecessor]\n");
  {
    World w(12, Clean(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    const std::vector<uint8_t> save = MakeSave(30000, 12);
    w.host.offerSave(save);
    Check(w.runUntil([&] { return Connected(w); }, 5000), "the first client is connected");
    // Its game dies (nothing more from it); the replacement comes from another socket.
    Endpoint fresh(MakeConfig(Role::kClient), [&](const uint8_t* d, size_t n, uint64_t) { w.host.receive(d, n, w.now, 77); });
    bool busy_seen = false;
    for (uint64_t i = 0; i < 20000 && fresh.status() != Status::kConnected; ++i) {
      ++w.now;
      w.host.tick(w.now);
      fresh.tick(w.now);
      std::vector<std::vector<uint8_t>> replies;
      replies.swap(w.to_stranger);
      for (const auto& dg : replies) fresh.receive(dg.data(), dg.size(), w.now, 0);
      busy_seen |= fresh.rejectReason() == kRejectBusy;
    }
    Check(busy_seen, "while the host still thinks the first client is there, the newcomer is told it is busy");
    Check(fresh.status() == Status::kConnected && fresh.receivedSave() == save,
          "once the host has heard nothing from the first client for its timeout, the newcomer joins and gets the save");
  }

  printf("[a client that dies half way through joining does not block the next one]\n");
  {
    World w(14, Clean(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    const std::vector<uint8_t> save = MakeSave(200000, 14);
    w.host.offerSave(save);
    // The first client says HELLO, gets WELCOME and asks for the save, then its game dies (no more from it).
    w.runUntil([&] { return w.client.status() == Status::kSyncing; }, 5000);
    Check(w.client.status() == Status::kSyncing && w.host.status() == Status::kSyncing, "the first client is part way through joining");
    // Let the host give it up (5 s of silence), then another client joins from another socket.
    Endpoint fresh(MakeConfig(Role::kClient), [&](const uint8_t* d, size_t n, uint64_t) { w.host.receive(d, n, w.now, 77); });
    bool connected = false;
    for (uint64_t i = 0; i < 30000 && !connected; ++i) {
      ++w.now;
      w.host.tick(w.now);
      fresh.tick(w.now);
      std::vector<std::vector<uint8_t>> replies;
      replies.swap(w.to_stranger);
      for (const auto& dg : replies) fresh.receive(dg.data(), dg.size(), w.now, 0);
      connected = fresh.status() == Status::kConnected;
    }
    Check(connected && fresh.receivedSave() == save, "the next client joins and gets the whole save");
  }

  printf("[a host that has forgotten its client after a minute takes a new one]\n");
  {
    World w(15, Clean(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    const std::vector<uint8_t> save = MakeSave(300000, 15);
    w.host.offerSave(save);
    Check(w.runUntil([&] { return Connected(w); }, 20000), "the first client joined (ready, ids moving)");
    w.sendEvents(30);
    w.runUntil([&] { return w.eventsComplete(); }, 5000);
    // Its game dies: nothing more from it for 70 s, the host loses it at 5 s and forgets it at 60 s.
    Endpoint fresh(MakeConfig(Role::kClient), [&](const uint8_t* d, size_t n, uint64_t) { w.host.receive(d, n, w.now, 77); });
    for (uint64_t i = 0; i < 70000; ++i) {
      ++w.now;
      w.host.tick(w.now);
    }
    Check(w.host.status() == Status::kLost, "the host lost the first client");
    bool connected = false;
    for (uint64_t i = 0; i < 20000 && !connected; ++i) {
      ++w.now;
      w.host.tick(w.now);
      fresh.tick(w.now);
      std::vector<std::vector<uint8_t>> replies;
      replies.swap(w.to_stranger);
      for (const auto& dg : replies) fresh.receive(dg.data(), dg.size(), w.now, 0);
      connected = fresh.status() == Status::kConnected;
    }
    Check(connected && fresh.receivedSave() == save, "a new client then joins and gets the whole save");
  }

  printf("[a second state channel carries the enemies next to the player state]\n");
  {
    World w(13, Bad(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    w.host.offerSave(MakeSave(1000, 13));
    Check(w.runUntil([&] { return Connected(w); }, 20000), "connected over a bad link");
    std::vector<std::vector<uint8_t>> got2;
    w.client.onState2([&](const uint8_t* p, size_t n) { got2.emplace_back(p, p + n); });
    const uint32_t before = w.client_states;
    for (int i = 0; i < 60; ++i) {
      std::vector<uint8_t> a(564, (uint8_t)i), b(600, (uint8_t)i);
      w.host.sendState(a.data(), a.size());
      w.host.sendState2(b.data(), b.size());
      w.runUntil([] { return false; }, 33);
    }
    w.runUntil([] { return false; }, 500);
    Check(!got2.empty() && got2.back().size() == 600 && got2.back()[0] >= 50 && got2.back()[599] == got2.back()[0],
          "a recent second-channel state arrives whole (564 + 600 bytes sent together every 33 ms, over a lossy link)");
    Check(w.client_states - before > 20 && got2.size() > 20, "both channels keep delivering, each newest-wins and lossy");
  }

  printf("[garbage: random bytes, truncated and oversized messages reach both ends]\n");
  {
    World w(11, Clean(), MakeConfig(Role::kHost), MakeConfig(Role::kClient));
    w.host.offerSave(MakeSave(50000, 11));
    Check(w.runUntil([&] { return Connected(w); }, 10000), "connected");
    Rng r(99);
    for (int i = 0; i < 20000; ++i) {
      std::vector<uint8_t> junk((size_t)(r.next() % 1300));
      for (auto& b : junk) b = (uint8_t)r.next();
      if (i % 3 == 0 && junk.size() > 8) memcpy(junk.data(), "SOWN", 4);
      w.host.receive(junk.data(), junk.size(), w.now, 1);
      w.client.receive(junk.data(), junk.size(), w.now, 0);
    }
    // Valid framing, valid CRC, hostile contents: a message longer than its datagram, and unknown types.
    for (int i = 0; i < 500; ++i) {
      std::vector<uint8_t> d;
      Writer wr{d};
      wr.bytes(reinterpret_cast<const uint8_t*>("SOWN"), 4);
      wr.u16(kProto);
      wr.u32(w.client.session());
      wr.u16(0);
      wr.u32(0);
      wr.u32(0);
      const int kind = i % 4;
      if (kind == 0) { wr.u8(kEvent); wr.u16(60000); wr.u8(1); }
      else if (kind == 1) { wr.u8(200); wr.u16(3); wr.u8(1); wr.u8(2); wr.u8(3); }
      else if (kind == 2) { wr.u8(kSaveChunk); wr.u16(2); wr.u8(1); wr.u8(2); }
      else { wr.u8(kPong); wr.u16(0); }
      const uint32_t crc = Crc32(d.data(), d.size());
      wr.u32(crc);
      w.host.receive(d.data(), d.size(), w.now, 1);
      w.client.receive(d.data(), d.size(), w.now, 0);
    }
    Check(w.host.stats().bad_crc + w.host.stats().bad_format > 10000, "most garbage is counted and dropped");
    w.sendEvents(50);
    Check(w.runUntil([&] { return w.eventsComplete(); }, 5000) && w.eventsInOrder(), "the connection carries on afterwards");
  }

  printf("[a corrupted save is refused by its hash]\n");
  {
    // Forge a host: it sends a WELCOME that promises one hash and chunks that hold other data.
    std::vector<Delivery> to_client;
    Endpoint client(MakeConfig(Role::kClient), [&](const uint8_t*, size_t, uint64_t) {});
    client.tick(1);
    uint8_t sha[32] = {};  // not the hash of what follows
    std::vector<uint8_t> body;
    Writer w{body};
    w.u32(4242);
    w.u64(1000);
    w.u32(2000);
    w.bytes(sha, 32);
    w.u8(0);
    std::vector<uint8_t> d;
    Writer dw{d};
    dw.bytes(reinterpret_cast<const uint8_t*>("SOWN"), 4);
    dw.u16(kProto);
    dw.u32(4242);
    dw.u16(0);
    dw.u32(0);
    dw.u32(0);
    dw.u8(kWelcome);
    dw.u16((uint16_t)body.size());
    dw.bytes(body.data(), body.size());
    dw.u32(Crc32(d.data(), d.size()));
    client.receive(d.data(), d.size(), 2, 0);
    Check(client.status() == Status::kSyncing, "the client starts the download after WELCOME");
    for (uint32_t k = 0; k < 2; ++k) {
      std::vector<uint8_t> c;
      Writer cw{c};
      cw.bytes(reinterpret_cast<const uint8_t*>("SOWN"), 4);
      cw.u16(kProto);
      cw.u32(4242);
      cw.u16(0);
      cw.u32(0);
      cw.u32(0);
      std::vector<uint8_t> chunk;
      Writer kw{chunk};
      kw.u32(k);          // reliable id
      kw.u32(k * 1000);   // offset
      for (int i = 0; i < 1000; ++i) kw.u8((uint8_t)i);
      cw.u8(kSaveChunk);
      cw.u16((uint16_t)chunk.size());
      cw.bytes(chunk.data(), chunk.size());
      cw.u32(Crc32(c.data(), c.size()));
      client.receive(c.data(), c.size(), 3, 0);
    }
    Check(client.status() == Status::kRejected && client.rejectReason() == kRejectSave,
          "a save that does not match the promised hash is refused");
  }

  printf("RESULT: %s (%d of %d checks passed)\n", g_failed ? "FAIL" : "PASS", g_checks - g_failed, g_checks);
  return g_failed ? 1 : 0;
}
