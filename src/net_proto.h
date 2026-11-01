// The co-op transport (stage 1, see NETWORK-DESIGN.md): one UDP "connection" between a host and a client.
// Pure logic, no sockets, no clocks, no game code: the caller feeds it datagrams and the time and sends what
// it hands back, so it is tested in a simulator (src/net_test.cpp) over lossy, reordering links.
//
// Datagram: magic "SOWN", proto u16, session u32, seq u16, rack u32, rack_bits u32, messages, crc32 u32.
// A message is [type u8][len u16][body]. Reliable ones (EVENT, SAVE_*, READY, BYE) start their body with a
// u32 id; the receiver delivers them in order, once, and reports what it has in rack (every id below it
// arrived) and rack_bits (bit i: id rack+1+i arrived). Everything else is sent once (STATE: the newest only).
// Everything from the network is untrusted: lengths are bounds-checked and a bad datagram is counted and dropped.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sownet {

constexpr uint16_t kProto = 1;
constexpr size_t kHeaderSize = 4 + 2 + 4 + 2 + 4 + 4;
constexpr size_t kMaxDatagram = 1200;
constexpr size_t kSaveChunkBytes = 1000;
constexpr size_t kWindow = 64;           // reliable messages in flight
constexpr uint32_t kRecvAhead = 128;     // ids buffered beyond the next expected one
constexpr size_t kMaxSave = 16u << 20;   // refuse a bigger save

enum MsgType : uint8_t {
  kHello = 1, kWelcome, kReject, kState, kEvent, kSaveReq, kSaveChunk, kReady, kPing, kPong, kBye,
  kState2 = 12,  // a second newest-wins channel (the enemies), independent of kState
};
enum Reject : uint8_t { kRejectProto = 1, kRejectBuild = 2, kRejectPassword = 3, kRejectBusy = 4, kRejectSave = 5 };
enum class Role { kHost, kClient };
enum class Status {
  kIdle,        // host: listening, nobody yet; client: not started
  kConnecting,  // client: sending HELLO
  kSyncing,     // handshake done, the save is being sent
  kConnected,
  kLost,        // nothing heard for a while; the client keeps trying to resume
  kRejected,    // client: the host said no (reason in rejectReason())
  kClosed,      // BYE
};

// ---- small helpers -------------------------------------------------------------------------------------

inline uint32_t Crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

struct Sha256 {  // FIPS 180-4
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t len = 0;
  size_t fill = 0;
  Sha256() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, init, sizeof h);
  }
  static uint32_t R(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = hh + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t* p, size_t n) {
    len += n;
    while (n) {
      size_t take = std::min(n, 64 - fill);
      memcpy(buf + fill, p, take);
      fill += take; p += take; n -= take;
      if (fill == 64) { block(buf); fill = 0; }
    }
  }
  void finish(uint8_t out[32]) {
    uint64_t bits = len * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (fill != 56) update(&z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(lb, 8);
    for (int i = 0; i < 8; ++i)
      for (int k = 0; k < 4; ++k) out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
  }
};

inline void Sha256Of(const uint8_t* p, size_t n, uint8_t out[32]) {
  Sha256 s;
  s.update(p, n);
  s.finish(out);
}

struct Writer {
  std::vector<uint8_t>& v;
  void u8(uint8_t x) { v.push_back(x); }
  void u16(uint16_t x) { u8((uint8_t)x); u8((uint8_t)(x >> 8)); }
  void u32(uint32_t x) { u16((uint16_t)x); u16((uint16_t)(x >> 16)); }
  void u64(uint64_t x) { u32((uint32_t)x); u32((uint32_t)(x >> 32)); }
  void bytes(const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); }
};

struct Reader {
  const uint8_t* p;
  size_t n;
  bool ok = true;
  Reader(const uint8_t* d, size_t len) : p(d), n(len) {}
  bool need(size_t k) { if (n < k) ok = false; return ok; }
  uint8_t u8() { if (!need(1)) return 0; uint8_t x = *p; ++p; --n; return x; }
  uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (uint16_t)u8() << 8); }
  uint32_t u32() { uint32_t a = u16(); return a | (uint32_t)u16() << 16; }
  uint64_t u64() { uint64_t a = u32(); return a | (uint64_t)u32() << 32; }
  bool bytes(uint8_t* out, size_t k) { if (!need(k)) return false; memcpy(out, p, k); p += k; n -= k; return true; }
};

struct Config {
  Role role = Role::kHost;
  uint8_t exe_sha[32] = {};          // the game exe's hash: both sides must run the same build
  uint32_t mod_build = 1;            // must match
  std::string password;              // empty: none
  uint32_t session_seed = 1;         // host: makes the session id
  uint64_t timeout_ms = 5000;        // nothing heard for this long: Lost
  uint64_t hello_every_ms = 500;
  uint64_t ping_every_ms = 1000;
};

struct Stats {
  uint64_t datagrams_in = 0, datagrams_out = 0, bad_crc = 0, bad_format = 0, ignored = 0;
  uint64_t hellos = 0, unknown_session = 0, unknown_src = 0;  // diagnostics: HELLOs seen; datagrams of another session or address
  uint64_t reliable_sent = 0, resends = 0, events_in = 0, duplicates = 0, states_in = 0;
  double rtt_ms = 0, jitter_ms = 0;
};

class Endpoint {
 public:
  // dst: an opaque peer id for the caller (a host sends to the source of the client's datagrams).
  using SendFn = std::function<void(const uint8_t*, size_t, uint64_t dst)>;
  using DataFn = std::function<void(const uint8_t*, size_t)>;
  using StatusFn = std::function<void(Status)>;

  Endpoint(const Config& cfg, SendFn send) : cfg_(cfg), send_(std::move(send)) {
    status_ = cfg.role == Role::kClient ? Status::kConnecting : Status::kIdle;
  }

  void onState(DataFn f) { on_state_ = std::move(f); }
  void onState2(DataFn f) { on_state2_ = std::move(f); }
  void onEvent(DataFn f) { on_event_ = std::move(f); }
  void onStatus(StatusFn f) { on_status_ = std::move(f); }

  // The host's save file, sent to a joining client (the client's copy is the one that loads it).
  void offerSave(std::vector<uint8_t> data) {
    save_ = std::move(data);
    Sha256Of(save_.data(), save_.size(), save_sha_);
  }
  const std::vector<uint8_t>& receivedSave() const { return got_save_; }

  // The newest state replaces any not yet sent. Events are reliable and ordered.
  void sendState(const uint8_t* p, size_t n) { pending_state_.assign(p, p + n); has_state_ = n != 0; }
  void sendState2(const uint8_t* p, size_t n) { pending_state2_.assign(p, p + n); has_state2_ = n != 0; }
  void sendEvent(const uint8_t* p, size_t n) { queueReliable(kEvent, p, n); }
  void close(uint64_t now) {
    queueReliable(kBye, nullptr, 0);
    flush(now);
    setStatus(Status::kClosed);
  }

  Status status() const { return status_; }
  uint8_t rejectReason() const { return reject_reason_; }
  const Stats& stats() const { return stats_; }
  uint32_t session() const { return session_; }
  double hostClockOffsetMs() const { return clock_offset_ms_; }
  size_t saveProgress() const { return got_save_.size(); }
  // Diagnostics: where a session stands.
  std::string debug() const {
    char b[200];
    snprintf(b, sizeof b, "session=%u ready=%d sending=%d save_next=%zu save_size=%zu outbox=%zu next_in=%u next_out=%u", session_,
             (int)ready_, (int)save_sending_, save_next_, save_.size(), outbox_.size(), next_in_id_, next_out_id_);
    return b;
  }
  size_t saveSize() const { return save_size_; }

  void receive(const uint8_t* d, size_t n, uint64_t now, uint64_t src) {
    ++stats_.datagrams_in;
    if (n < kHeaderSize + 4 || n > kMaxDatagram) { ++stats_.bad_format; return; }
    uint32_t crc;
    memcpy(&crc, d + n - 4, 4);
    if (Crc32(d, n - 4) != crc) { ++stats_.bad_crc; return; }
    Reader r(d, n - 4);
    uint8_t magic[4];
    r.bytes(magic, 4);
    if (memcmp(magic, "SOWN", 4) != 0) { ++stats_.bad_format; return; }
    const uint16_t proto = r.u16();
    const uint32_t session = r.u32();
    r.u16();  // seq (kept for debugging)
    const uint32_t rack = r.u32(), bits = r.u32();
    if (!r.ok) { ++stats_.bad_format; return; }
    if (proto != kProto) { ++stats_.ignored; return; }
    const bool known = cfg_.role == Role::kClient ? (session_ == 0 || session == session_)
                                                  : (session_ != 0 && session == session_ && src == peer_);
    // A datagram that is not part of this session may only carry a HELLO (to a host), or the host's
    // WELCOME or REJECT (to a client).
    std::vector<Msg> msgs;
    if (!parseMessages(r, &msgs)) { ++stats_.bad_format; return; }
    if (!known) {
      if (cfg_.role == Role::kHost && session_ != 0) {
        if (session != session_) ++stats_.unknown_session;
        else ++stats_.unknown_src;
      }
      for (const Msg& m : msgs) {
        if (cfg_.role == Role::kHost && m.type == kHello) onHello(m, now, src);
        else if (cfg_.role == Role::kClient && (m.type == kWelcome || m.type == kReject)) onMessage(m, now, src);
      }
      flush(now);
      return;
    }
    last_rx_ = now;
    if (status_ == Status::kLost && cfg_.role == Role::kHost) setStatus(ready_ ? Status::kConnected : Status::kSyncing);
    processAcks(rack, bits, now);
    for (const Msg& m : msgs) onMessage(m, now, src);
    flush(now);
  }

  void tick(uint64_t now) {
    if (cfg_.role == Role::kClient && (status_ == Status::kConnecting || status_ == Status::kLost) &&
        (last_hello_ == kNever || now - last_hello_ >= cfg_.hello_every_ms)) {
      sendHello(now);
    }
    const bool live = status_ == Status::kConnected || status_ == Status::kSyncing;
    if (live && last_rx_ != kNever && now - last_rx_ > cfg_.timeout_ms) setStatus(Status::kLost);
    if (status_ == Status::kConnected && (last_ping_ == kNever || now - last_ping_ >= cfg_.ping_every_ms)) {
      last_ping_ = now;
      std::vector<uint8_t> b;
      Writer w{b};
      w.u64(now);
      ctrl_.push_back({kPing, b});
    }
    if (status_ == Status::kLost && cfg_.role == Role::kHost && last_rx_ != kNever && now - last_rx_ > 60000)
      resetSession();  // the host forgets a client that stayed away for a minute (all of its session, not only the id:
                       // the ids, the window and `ready_` of the old one would otherwise meet the next client's)
    flush(now);
  }

 private:
  static constexpr uint64_t kNever = ~0ull;
  struct Msg { uint8_t type; std::vector<uint8_t> body; };
  struct Out {
    uint32_t id;
    uint8_t type;
    std::vector<uint8_t> body;  // after the id
    uint64_t first = kNever, last = kNever;
    unsigned retries = 0;
  };

  bool parseMessages(Reader& r, std::vector<Msg>* out) {
    while (r.n > 0) {
      Msg m;
      m.type = r.u8();
      const uint16_t len = r.u16();
      if (!r.ok || len > r.n) return false;
      m.body.assign(r.p, r.p + len);
      r.p += len;
      r.n -= len;
      out->push_back(std::move(m));
    }
    return true;
  }

  void setStatus(Status s) {
    if (s == status_) return;
    status_ = s;
    if (on_status_) on_status_(s);
  }

  static bool isReliable(uint8_t t) { return t == kEvent || t == kSaveReq || t == kSaveChunk || t == kReady || t == kBye; }

  void queueReliable(uint8_t type, const uint8_t* p, size_t n) {
    Out o;
    o.id = next_out_id_++;
    o.type = type;
    if (n) o.body.assign(p, p + n);
    outbox_.push_back(std::move(o));
  }

  // ---- handshake -------------------------------------------------------------------------------------

  void sendHello(uint64_t now) {
    last_hello_ = now;
    if (nonce_ == 0) nonce_ = 0x9E3779B9u * (uint32_t)(now + 1) | 1u;
    std::vector<uint8_t> b;
    Writer w{b};
    w.bytes(cfg_.exe_sha, 32);
    w.u32(cfg_.mod_build);
    w.u32(nonce_);
    uint8_t pw[32];
    passwordHash(nonce_, pw);
    w.bytes(pw, 32);
    w.u32(session_);  // non-zero: resume that session
    ctrl_.push_back({kHello, b});
  }

  void passwordHash(uint32_t nonce, uint8_t out[32]) const {
    Sha256 s;
    s.update(reinterpret_cast<const uint8_t*>(cfg_.password.data()), cfg_.password.size());
    uint8_t nb[4] = {(uint8_t)nonce, (uint8_t)(nonce >> 8), (uint8_t)(nonce >> 16), (uint8_t)(nonce >> 24)};
    s.update(nb, 4);
    s.finish(out);
  }

  void reject(uint8_t reason, uint64_t src, uint64_t now) {
    if (now - last_reject_ < 1000 && last_reject_ != kNever) return;  // at most once a second
    last_reject_ = now;
    std::vector<uint8_t> b{reason};
    sendRaw(0, kReject, b, src);
  }

  void onHello(const Msg& m, uint64_t now, uint64_t src) {
    ++stats_.hellos;
    Reader r(m.body.data(), m.body.size());
    uint8_t exe[32], pw[32];
    r.bytes(exe, 32);
    const uint32_t build = r.u32(), nonce = r.u32();
    r.bytes(pw, 32);
    const uint32_t resume = r.u32();
    if (!r.ok) { ++stats_.bad_format; return; }
    if (memcmp(exe, cfg_.exe_sha, 32) != 0) return reject(kRejectProto, src, now);
    if (build != cfg_.mod_build) return reject(kRejectBuild, src, now);
    uint8_t want[32];
    passwordHash(nonce, want);
    if (memcmp(pw, want, 32) != 0) return reject(kRejectPassword, src, now);
    const bool is_resume = resume != 0 && resume == session_ && ready_;
    if (session_ != 0 && !is_resume && src == peer_ && nonce == peer_nonce_ && status_ == Status::kSyncing) {
      // The HELLO that opened this session once more (its WELCOME was lost, or the datagram came twice): answer
      // again, keep the session and the download as they are.
      last_rx_ = now;
      sendWelcome(false, now);
      return;
    }
    if (session_ != 0 && !is_resume) {
      // The same client starting over, or a newcomer once the old client has gone quiet (its game died): the old
      // session is dropped. Anyone else, while the old client is still there, is told the host is busy.
      if ((src == peer_ && status_ != Status::kConnected) || status_ == Status::kLost) {
        resetSession();
      } else {
        return reject(kRejectBusy, src, now);
      }
    }
    if (session_ == 0) {
      resetSession();  // a clean slate for every new session
      session_ = (cfg_.session_seed * 2654435761u ^ nonce) | 1u;
      peer_nonce_ = nonce;
      peer_ = src;
      last_rx_ = now;
      setStatus(Status::kSyncing);
    } else {
      peer_ = src;  // a resume may come from a new address
      last_rx_ = now;
    }
    sendWelcome(is_resume, now);
    if (is_resume && status_ == Status::kLost) setStatus(Status::kConnected);
  }

  void sendWelcome(bool is_resume, uint64_t now) {
    std::vector<uint8_t> b;
    Writer w{b};
    w.u32(session_);
    w.u64(now);
    w.u32(is_resume ? 0 : (uint32_t)save_.size());
    w.bytes(save_sha_, 32);
    w.u8(is_resume ? 1 : 0);
    sendRaw(session_, kWelcome, b, peer_);
  }

  void resetSession() {
    session_ = 0;
    ready_ = false;
    outbox_.clear();
    pending_state_.clear();
    has_state_ = false;
    pending_state2_.clear();
    has_state2_ = false;
    next_out_id_ = 0;
    next_in_id_ = 0;
    in_buf_.clear();
    save_sending_ = false;
    save_next_ = 0;
    got_save_.clear();
  }

  // A one-off datagram (handshake replies that need a specific session or destination).
  void sendRaw(uint32_t session, uint8_t type, const std::vector<uint8_t>& body, uint64_t dst) {
    std::vector<uint8_t> d;
    header(&d, session, 0, 0);
    Writer w{d};
    w.u8(type);
    w.u16((uint16_t)body.size());
    w.bytes(body.data(), body.size());
    finish(&d);
    ++stats_.datagrams_out;
    send_(d.data(), d.size(), dst);
  }

  // ---- receiving -------------------------------------------------------------------------------------

  void onMessage(const Msg& m, uint64_t now, uint64_t src) {
    if (isReliable(m.type)) {
      Reader r(m.body.data(), m.body.size());
      const uint32_t id = r.u32();
      if (!r.ok) { ++stats_.bad_format; return; }
      Msg inner{m.type, std::vector<uint8_t>(m.body.begin() + 4, m.body.end())};
#ifdef SOWNET_MUTANT  // negative control: no ordering, no duplicate suppression
      if (id != 0xFFFFFFFFu) {
        deliver(inner, now);
        ack_dirty_ = true;
        return;
      }
#endif
      if (id < next_in_id_) { ++stats_.duplicates; ack_dirty_ = true; return; }
      if (id >= next_in_id_ + kRecvAhead) { ++stats_.ignored; return; }
      if (id > next_in_id_) {
        if (!in_buf_.count(id)) in_buf_[id] = std::move(inner); else ++stats_.duplicates;
        ack_dirty_ = true;
        return;
      }
      deliver(inner, now);
      ++next_in_id_;
      for (auto it = in_buf_.find(next_in_id_); it != in_buf_.end(); it = in_buf_.find(next_in_id_)) {
        Msg next = std::move(it->second);
        in_buf_.erase(it);
        deliver(next, now);
        ++next_in_id_;
      }
      ack_dirty_ = true;
      return;
    }
    switch (m.type) {
      case kState:
        ++stats_.states_in;
        if (on_state_) on_state_(m.body.data(), m.body.size());
        break;
      case kState2:
        ++stats_.states_in;
        if (on_state2_) on_state2_(m.body.data(), m.body.size());
        break;
      case kPing: {
        Reader r(m.body.data(), m.body.size());
        const uint64_t t = r.u64();
        if (!r.ok) break;
        std::vector<uint8_t> b;
        Writer w{b};
        w.u64(t);
        w.u64(now);
        ctrl_.push_back({kPong, b});
        break;
      }
      case kPong: {
        Reader r(m.body.data(), m.body.size());
        const uint64_t t = r.u64(), host_now = r.u64();
        if (!r.ok || now < t) break;
        sampleRtt((double)(now - t));
        // The peer's clock when it answered, against ours halfway through the round trip (a host's clock).
        if (cfg_.role == Role::kClient) clock_offset_ms_ = (double)host_now - ((double)t + (double)(now - t) / 2.0);
        break;
      }
      case kHello:  // a client resuming inside its session
        if (cfg_.role == Role::kHost) onHello(m, now, src);
        break;
      case kWelcome:
        if (cfg_.role == Role::kClient) onWelcome(m, now);
        break;
      case kReject:
        if (cfg_.role == Role::kClient && !m.body.empty()) {
          reject_reason_ = m.body[0];
          // Busy is not final: the host frees the place once its old client has been silent for a few seconds, so
          // the HELLOs go on (the caller's own timeout decides how long to wait).
          if (reject_reason_ != kRejectBusy) setStatus(Status::kRejected);
        }
        break;
      default:
        ++stats_.ignored;
    }
  }

  void deliver(const Msg& m, uint64_t now) {
    switch (m.type) {
      case kEvent:
        ++stats_.events_in;
        if (on_event_) on_event_(m.body.data(), m.body.size());
        break;
      case kSaveReq:
        if (cfg_.role == Role::kHost) { save_sending_ = true; save_next_ = 0; }
        break;
      case kSaveChunk:
        if (cfg_.role == Role::kClient) onSaveChunk(m);
        break;
      case kReady:
        if (cfg_.role == Role::kHost) { ready_ = true; if (status_ == Status::kSyncing) setStatus(Status::kConnected); }
        break;
      case kBye:
        setStatus(Status::kClosed);
        break;
      default:
        break;
    }
    (void)now;
  }

  void onWelcome(const Msg& m, uint64_t now) {
    Reader r(m.body.data(), m.body.size());
    const uint32_t session = r.u32();
    const uint64_t host_t = r.u64();
    const uint32_t save_size = r.u32();
    uint8_t sha[32];
    r.bytes(sha, 32);
    const uint8_t resumed = r.u8();
    if (!r.ok) { ++stats_.bad_format; return; }
    if (status_ == Status::kConnected || (status_ == Status::kSyncing && session == session_)) return;
    clock_offset_ms_ = (double)host_t - (double)now;
    if (session_ != 0 && session != session_ && !resumed) {
      // The host started a new session: this client starts over too.
      resetSession();
    }
    session_ = session;
    last_rx_ = now;
    if (!resumed) {
      save_size_ = save_size;
      memcpy(want_sha_, sha, 32);
      if (save_size > kMaxSave) { reject_reason_ = kRejectSave; setStatus(Status::kRejected); return; }
    }
    if (!resumed && save_size > 0) {
      got_save_.clear();
      setStatus(Status::kSyncing);
      queueReliable(kSaveReq, nullptr, 0);
    } else {
      queueReliable(kReady, nullptr, 0);
      setStatus(Status::kConnected);
    }
  }

  void onSaveChunk(const Msg& m) {
    Reader r(m.body.data(), m.body.size());
    const uint32_t off = r.u32();
    if (!r.ok || off != got_save_.size() || got_save_.size() + (m.body.size() - 4) > save_size_) {
      reject_reason_ = kRejectSave;
      setStatus(Status::kRejected);
      return;
    }
    got_save_.insert(got_save_.end(), m.body.begin() + 4, m.body.end());
    if (got_save_.size() == save_size_) {
      uint8_t sha[32];
      Sha256Of(got_save_.data(), got_save_.size(), sha);
      if (memcmp(sha, want_sha_, 32) != 0) {
        reject_reason_ = kRejectSave;
        setStatus(Status::kRejected);
        return;
      }
      queueReliable(kReady, nullptr, 0);
      setStatus(Status::kConnected);
    }
  }

  void processAcks(uint32_t rack, uint32_t bits, uint64_t now) {
    for (auto it = outbox_.begin(); it != outbox_.end();) {
      bool acked = it->id < rack;
      if (!acked && it->id > rack && it->id - rack - 1 < 32) acked = (bits >> (it->id - rack - 1)) & 1u;
      if (acked) {
        if (it->retries == 0 && it->first != kNever) sampleRtt((double)(now - it->first));
        max_acked_ = std::max(max_acked_, it->id);
        have_acked_ = true;
        it = outbox_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void sampleRtt(double ms) {
    if (stats_.rtt_ms == 0) stats_.rtt_ms = ms;
    stats_.jitter_ms += (std::abs(ms - stats_.rtt_ms) - stats_.jitter_ms) / 8.0;
    stats_.rtt_ms += (ms - stats_.rtt_ms) / 8.0;
  }

  // ---- sending ---------------------------------------------------------------------------------------

  void header(std::vector<uint8_t>* d, uint32_t session, uint32_t rack, uint32_t bits) {
    Writer w{*d};
    w.bytes(reinterpret_cast<const uint8_t*>("SOWN"), 4);
    w.u16(kProto);
    w.u32(session);
    w.u16(seq_++);
    w.u32(rack);
    w.u32(bits);
  }
  static void finish(std::vector<uint8_t>* d) {
    const uint32_t crc = Crc32(d->data(), d->size());
    Writer w{*d};
    w.u32(crc);
  }

  void addMsg(std::vector<uint8_t>* d, uint8_t type, const uint8_t* p, size_t n) {
    Writer w{*d};
    w.u8(type);
    w.u16((uint16_t)n);
    w.bytes(p, n);
  }

  void pumpSave() {
    while (save_sending_ && save_next_ < save_.size() && outbox_.size() < kWindow * 2) {
      const size_t n = std::min(kSaveChunkBytes, save_.size() - save_next_);
      std::vector<uint8_t> b;
      Writer w{b};
      w.u32((uint32_t)save_next_);
      w.bytes(save_.data() + save_next_, n);
      queueReliable(kSaveChunk, b.data(), b.size());
      save_next_ += n;
    }
    if (save_sending_ && save_next_ >= save_.size()) save_sending_ = false;
  }

  void flush(uint64_t now) {
    if (session_ == 0 && cfg_.role == Role::kHost) { ctrl_.clear(); return; }
    pumpSave();
    const double rto = std::max(80.0, std::min(1000.0, stats_.rtt_ms > 0 ? stats_.rtt_ms * 1.5 + 4 * stats_.jitter_ms + 20 : 250.0));
    uint32_t rack = next_in_id_, bits = 0;
    for (uint32_t i = 0; i < 32; ++i)
      if (in_buf_.count(rack + 1 + i)) bits |= 1u << i;
    for (int guard = 0; guard < 96; ++guard) {
      std::vector<uint8_t> d;
      header(&d, session_, rack, bits);
      const size_t base = d.size();
      auto room = [&](size_t body) { return d.size() + 3 + body + 4 <= kMaxDatagram; };
      while (!ctrl_.empty() && room(ctrl_.front().body.size())) {
        addMsg(&d, ctrl_.front().type, ctrl_.front().body.data(), ctrl_.front().body.size());
        ctrl_.pop_front();
      }
      size_t seen = 0;
      for (Out& o : outbox_) {
        if (seen++ >= kWindow) break;
        bool due = o.last == kNever;
        if (!due) {
          const double backoff = rto * (double)(1u << std::min(o.retries, 3u));
          due = (double)(now - o.last) >= backoff;
          if (!due && have_acked_ && max_acked_ > o.id) due = (double)(now - o.last) >= std::max(20.0, stats_.rtt_ms + 15.0);
        }
        if (!due || !room(4 + o.body.size())) continue;
        std::vector<uint8_t> b;
        Writer w{b};
        w.u32(o.id);
        w.bytes(o.body.data(), o.body.size());
        addMsg(&d, o.type, b.data(), b.size());
        if (o.first == kNever) { o.first = now; ++stats_.reliable_sent; } else { ++o.retries; ++stats_.resends; }
        o.last = now;
      }
      if (has_state_ && room(pending_state_.size())) {
        addMsg(&d, kState, pending_state_.data(), pending_state_.size());
        has_state_ = false;
      }
      if (has_state2_ && room(pending_state2_.size())) {
        addMsg(&d, kState2, pending_state2_.data(), pending_state2_.size());
        has_state2_ = false;
      }
      if (d.size() == base && !ack_dirty_) break;
      ack_dirty_ = false;
      finish(&d);
      ++stats_.datagrams_out;
      send_(d.data(), d.size(), cfg_.role == Role::kHost ? peer_ : 0);
      if (d.size() == base + 4 && ctrl_.empty()) {
        // A bare ack: stop unless more is waiting.
        bool more = false;
        for (const Out& o : outbox_) more = more || o.last == kNever;
        if (!more) break;
      }
    }
  }

  Config cfg_;
  SendFn send_;
  DataFn on_state_, on_state2_, on_event_;
  StatusFn on_status_;
  Status status_ = Status::kIdle;
  Stats stats_;
  uint8_t reject_reason_ = 0;
  uint32_t session_ = 0, nonce_ = 0;
  uint64_t peer_ = 0;
  uint64_t last_rx_ = kNever, last_hello_ = kNever, last_ping_ = kNever, last_reject_ = kNever;
  uint16_t seq_ = 0;
  bool ack_dirty_ = false, ready_ = false;
  uint32_t peer_nonce_ = 0;  // host: the nonce of the HELLO that opened the current session
  double clock_offset_ms_ = 0;

  std::deque<Msg> ctrl_;
  std::deque<Out> outbox_;
  uint32_t next_out_id_ = 0, next_in_id_ = 0, max_acked_ = 0;
  bool have_acked_ = false;
  std::map<uint32_t, Msg> in_buf_;
  std::vector<uint8_t> pending_state_, pending_state2_;
  bool has_state_ = false, has_state2_ = false;

  std::vector<uint8_t> save_, got_save_;
  uint8_t save_sha_[32] = {}, want_sha_[32] = {};
  size_t save_next_ = 0;
  size_t save_size_ = 0;
  bool save_sending_ = false;
};

}  // namespace sownet
