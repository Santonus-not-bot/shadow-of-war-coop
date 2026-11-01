# Stage 1: co-op over the internet (design, for review)

Goal: two players, one host and one client, each on their own PC, joining by IP. Progress stays in the host's
world. Slow motion is off in co-op. No port forwarding is required if both use an overlay VPN (Tailscale,
ZeroTier, Radmin); plain port forwarding also works. Speed of getting a working version out comes first.

## What exists

- Loopback only: each copy sends one 564-byte `SOWC` v8 packet per game frame (~58 a second) to the other's
  port and shows the other player as a stand-in (`src/coop.cpp`: `SendSample`, `ListenThread`).
- No handshake, no acknowledgements, no resend, no sender check beyond "from 127.0.0.1".
- The guard (`sowguard`) blocks every non-loopback connect or send in the game process.
- 50 ms smoothing; a clock offset is estimated from `time_us`.

## Shape

- Symmetric for the players (each sees the other as a stand-in), asymmetric for the world (later stages): one
  **host** (listens on a UDP port) and one **client** (connects to `host:port`).
- Config in `sowguard.ini`: `coop_role=host|client`, `coop_port=<udp port, default 27960>` (host),
  `coop_host=<ip>:<port>` (client), `coop_password=<text>` (optional), `coop_delay_ms` (default 120).
  The old loopback keys stay for the test rig.
- One UDP socket per copy. The client sends first, so only the host needs a reachable port.

## Wire format

Every datagram: `magic "SOWN"`, `proto u16`, `session u32`, `seq u16`, `ack u16`, `ack_bits u32`, then messages
`[type u8][len u16][body]`. `seq`/`ack`/`ack_bits` is the usual sliding window (the last 32 datagrams), so both
sides know what arrived without a separate ack packet. At most 1200 bytes per datagram (no fragmentation).

| Type | Direction | Reliable | Body |
|---|---|---|---|
| HELLO | client to host | resent every 500 ms | proto, exe SHA-256, mod build, nonce, password hash |
| WELCOME | host to client | with HELLO | session id, host clock, save size and SHA-256 |
| REJECT | host to client | no | reason (version, build, password, busy) |
| STATE | both | no (latest wins) | today's sample: position, rotation, health, anim inputs, buttons, attack id, mode flags |
| EVENT | both | yes (id, resent until acked, applied in order, duplicates ignored) | later stages: hits, spawn, fast travel, alive/dead |
| SAVE_CHUNK / SAVE_ACK | host to client | windowed | 1000-byte chunks of the host's save file |
| PING / PONG | both | no | timestamps, for round trip and jitter |
| BYE | both | no | reason |

STATE is sent at 30 Hz (not every frame) and is the only thing that is lossy: a lost one is replaced by the next.
EVENTs carry anything that must not be lost; they are the only place state changes by messages.

## Joining

1. **Host** starts the game (its own save, as today) and listens.
2. **Client** starts the game. Before the game reads its save (the guard loads first), the client's mod sends
   HELLO, gets WELCOME, downloads the host's save file (~2.3 MB, a few seconds), checks the SHA-256 and writes it
   into this copy's sandboxed cloud folder, so the game loads the host's world and never touches the friend's own
   save. Failure or a 20 s timeout stops the launch with a clear message (fail closed).
3. Both then run as today, plus the transport. The client is moved next to the host once in the world.
   Open question for stage 3: the host's save on disk is its last save, not where the host is now.

## Smoothing, loss and the connection

- Clock offset from STATE `time_us` as now, refined by PING/PONG; the stand-in is shown `coop_delay_ms` behind
  (default 120, up from 50), raised automatically to about twice the measured jitter plus 50 ms, capped at 250.
- No packets for 5 s: "connection lost", the stand-in is hidden and frozen state is dropped; the client resumes
  with the same session id (HELLO with it) and the host answers with a fresh baseline instead of replaying.
- Host quits or sends BYE: the client's stand-in is removed and the game keeps running as a solo game.
- Dying is left to the game: each player's death and respawn follow the game's own rules; STATE carries the
  health so the other copy shows the stand-in dead or alive. No co-op special case in stage 1.

## Safety

- Everything from the network is untrusted: every length is bounds-checked, unknown types are skipped, a bad
  datagram is counted and dropped, nothing from it is ever executed or used as a file name or path.
- The host accepts one client: after the first valid HELLO only that address and session are served; others get
  REJECT(busy) at most once a second. HELLOs are rate-limited.
- The guard learns a `network=coop_peer` rule: the game's own connections stay refused as now (offline mode); only
  UDP on the mod's own socket, to and from the configured peer, is let through.
- Password is a shared secret hashed with the nonce, only to keep strangers off the port, not encryption.

## Build order and tests

1. **Protocol core** (`src/net_proto.h`, no game code): framing, ack window, reliable messages, handshake,
   chunked transfer. Tested in a simulator over the hub's chaos model (`tools/chaos/cpp/impair.h`) with many seeds
   for loss, reordering, duplication and delay.
2. **Host and client roles on loopback** carrying today's STATE; the existing co-op test cases keep passing.
3. **Save transfer and the join flow**, tested with a corrupted and an interrupted transfer.
4. **Guard peer rule**, tested with a fake non-loopback peer (blocked until configured).
5. **Chaos between two real copies** (the hub's chaos box in the middle): loss, jitter, a 10 s outage and a
   rejoin; the stand-in must keep following and recover.
6. **A real internet test** with a friend over an overlay VPN.

## Open questions

- Port and name defaults; whether to show the host's address in the game UI.
- Save transfer before the game starts means the mod blocks start-up for a few seconds; acceptable?
- Where the client appears on joining (next to the host, same region): needs the host's current region.

## Status

- Step 1 (protocol core) is done: `src/net_proto.h` and `src/net_test.cpp` (`net_test.exe [seeds] [loss]`, plus a
  negative-control build that must fail). It covers framing with a CRC, acknowledgements, reliable ordered events
  without duplicates, the HELLO/WELCOME/REJECT handshake (password, game build, mod build, busy), the save
  download checked by SHA-256, resume after a long outage without re-downloading, and bad input.
- Step 2 (host and client roles in the mod, on loopback) is done: `coop_role`, `coop_port`, `coop_host`,
  `coop_password`, `coop_bind`, `coop_state_hz`; an offline case where the test is a client of the child host
  (`coop_puppet_recv_net`, same checks as the loopback receive case), and the live suite between two real copies
  with A hosting and B joining (all checks, both directions; the stand-in states go at 30 a second).
- Step 3 (the join flow) is done. The client does not block start-up: its mod connects while the game boots,
  and the Steam guard's cloud calls hold the game's first look at its save (`SteamSetBeforeSaveRead`) until the
  host's save is in hand and its SHA-256 matches; it is then written into the sandbox cloud folder under the
  name the game asks for (so a friend with another Steam account works) and over the local copy. Any failure
  ends the game with a message (exit `0xC0DE0007`). The host offers the newest `<hex>_ShadowOfWar.sav` of its
  sandbox cloud folder (the numbered slots are never offered) and re-reads it every 2 s while nobody is joined.
  Offline cases: ok, lossy (a quarter of the host's datagrams lost), a 1.5 s stall mid-download, a damaged save
  with a good datagram checksum, an interrupted download, no host, no save, wrong password, and the mod as host
  serving its newest save. Found and fixed on the way: a restarted client was refused ("busy") until the host
  forgot the old one after a minute (now: as soon as the old one has been silent for the 5 s timeout, and busy
  is retried, not final), and a duplicated HELLO restarted the session (now the same HELLO is just answered
  again; one seed in forty failed to connect at 15 % loss before). Separately, a spawn request that timed out
  could still run later and leave a second stand-in in the world; a timed-out request is now withdrawn.
- Step 4 (the guard peer rule) is done: `network=coop_peer`. The mod registers its socket with the guard
  (`SowAllowPeerSocket`); the guard lets that handle send to the client's configured host address and port (a
  host: to the source of its valid packets) and still refuses every other socket and address as before. Offline
  cases on this PC's own non-loopback address: the mod's socket gets through while a stray socket of the same
  process gets "network unreachable"; with `network=offline` nothing leaves; a host answers a client through the
  guard, and under `offline` nobody can join. Not tested offline: the 0.0.0.0 default bind.
- Step 5 (chaos between two real copies) is done: `tools/chaos_live.py` puts the hub's UDP relay between the
  host (A) and the client (B) and changes its profile while the game runs. Both directions, one copy walking
  then running with the other showing the stand-in: good_wifi, bad_wifi, mobile_tether, transatlantic and
  hell (rtt up to ~350 ms, jitter ~120 ms, 8 % loss in bursts): the link stayed up, the stand-in kept following
  (mean gap 19-35 units, worst 141) and closed to under 1 unit once the mover stopped. A 10 s blackout while the
  mover walked off: reported lost after the 5 s timeout, connected again at the heal, stand-in not replaced, no
  extra characters; 4 s one-way cuts either way: nothing dropped; a 70 s blackout (the host forgets the client
  after 60 s): the client starts a session again, the game goes on in its world, the stand-in returns. Only
  player states cross the link so far, so the resend path of reliable events was not exercised live (the
  simulator covers it); `resends` stayed 0.
- Step 6 (an internet test with a second person) has not been done: nobody to test with. What could be prepared is in: the host logs the addresses a client can use (`coop: a client on this network or VPN joins with coop_host=...`, checked by the offline host case) and the README has the overlay-VPN guide, marked as not yet tried on two PCs.
