# sow-coop

Work toward online co-op for Middle-earth: Shadow of War (Steam app 356190, build
3095001, `ShadowOfWar.exe` SHA-256 `ed0d95ca…0bdaef`). Write-up and research notes:
`games/shadow-of-war/` in [coop-anything](https://github.com/cranebeaker/coop-anything).

No game files are in this repo.

## sowguard (`src/sowguard.cpp`)

A safety layer for test copies, loaded as a `bink2w64.dll` proxy (all 73 Bink exports
forward to the original, renamed `bink2w64_.dll`). Before the game's code runs it:

- refuses to run unless the exe is inside `allowed_root`, outside the Steam folder, is the
  known build (size + SHA-256), and `steam_appid.txt` says 356190;
- hooks seven ntdll syscalls (six file calls and `NtDeviceIoControlFile`) (plain syscall stubs only; anything else stops the game)
  and redirects `%LOCALAPPDATA%\WB Games`, `Documents\WB Games` and
  `Steam\userdata\<id>\356190` into a sandbox profile;
- terminates the game (exit `0xC0DE0010`) on any write to the owner's install, save
  folders or the game's Steam userdata, and logs writes to the rest of Steam userdata;
- `network=offline` (default): sockets work, but every connect, ConnectEx or UDP send to a
  non-loopback address fails at once with "network unreachable" (`WSAENETUNREACH`), like an
  unplugged PC; loopback stays open. `network=deny_sockets` refuses socket creation (the
  game treats that as a bug and shows an error dialog); `network=allow` passes everything.
  `net_trace=1` logs every AFD request.
- Steam guard (`src/steamguard.cpp`, always on): once the game has initialised Steam it
  replaces the write methods of the game's `ISteamRemoteStorage` (v014: cloud saves,
  which `steam.exe` would write into the real Steam folder) and `ISteamUserStats` (v011:
  stats, achievements, progress pop-ups, resets, leaderboard uploads) with functions
  that log the call and report success. Reads pass. Slot offsets come from
  `steam_api64`'s one-jump flat wrappers; any other shape stops the game (`0xC0DE0006`).
  The game loads its save through the cloud (`FileRead`), which Steam answers from the
  real Steam folder, so without the next option every copy loads the same save.
- `steam_cloud=sandbox`: this copy's `<profile>\steam-userdata\356190\remote\` is the
  cloud. `FileRead`, `FileExists`, `GetFileSize`, `GetFileTimestamp`, `GetFileCount`,
  `GetFileNameAndSize` are answered from it and `FileWrite`/`FileDelete` change it (temp
  file and rename); names with `..` or a drive are refused. Seed it with the save the
  copy should load. `steam_trace=1` (without the sandbox cloud) logs the reads Steam
  answers.
- `autoload=1`: a virtual pad on XInput user 0 (the game's `XInputGetState` import slot,
  found in the rebuilt import table at the start of `.xtext`) presses A
  `autoload_first_ms` (20000) after the game window appears, then every
  `autoload_every_ms` (8000), until the game's debug print says
  `[QUESTMGR]: GameModeSet - ` (needs `hook_debug_print=1` with channel `2A34D9E`) or
  `autoload_max_presses` (6; the test copies use 8). Title, campaign list and the next screen take 4 presses
  (in the world at ~48 s). A real pad keeps working; the game reads pads unfocused.
- `background_run=1`: the game pauses when its window loses focus. This swallows the
  focus-loss messages (`WM_ACTIVATEAPP` false, inactive `WM_ACTIVATE`, `WM_KILLFOCUS`),
  makes the game's `GetForegroundWindow`/`GetActiveWindow` imports return its own window,
  and lets `ClipCursor`/`SetCursorPos` through only while the window really is in front,
  so two copies keep running side by side.
- `sim_hook=<vtable rva hex>:<slot>` (the test copies use `1F96818:36`, the player's
  per-frame update, ~58 calls a second on the simulation thread): a register-preserving
  pre-hook that runs queued work there. Engine calls that allocate need that thread.
- Co-op over a host and client link (`coop_role=host|client`, `src/net_proto.h`, replaces `coop_send` and
  `coop_listen`): the host listens on `coop_bind` (default `127.0.0.1`, loopback until the guard has a peer
  rule) port `coop_port`; the client connects to `coop_host=<ipv4>:<port>`. `coop_password` (optional, both
  sides), `coop_state_hz` (30, player states a second), `coop_delay_ms` (120 for a link). Packets carry a CRC;
  events are acknowledged, resent, ordered and never doubled; both sides must run the same game build and mod
  build. `NETWORK-DESIGN.md` has the design; `net_test.exe` tests the transport in a simulator.
- `tools/chaos_live.py` (two running copies, B joining through the hub's UDP relay from `tools/chaos`): profiles from good_wifi to hell, a 10 s blackout, one-way cuts, a 70 s blackout; the stand-in has to keep following and recover.
- Real addresses (`network=coop_peer`, needs `coop_role`): the guard stays offline for the game itself, but the
  mod's own UDP socket (and only that socket) may send to a non-loopback address: a client to the host's
  `coop_host` address and port only, a host to whoever its valid packets came from. Inbound packets were never
  blocked. A host then listens on `0.0.0.0` unless `coop_bind` says otherwise (on a PC with a firewall the
  first run asks to allow it). `network=offline` keeps the mod on loopback.
- Joining (`coop_role=client`, needs `steam_cloud=sandbox`): the client plays in the host's world. Its mod
  connects at start-up; the game's first look at its save through the cloud calls waits until the host's save
  (the newest `<hex>_ShadowOfWar.sav` in the host's sandbox cloud folder, re-read every 2 s while nobody is
  joined) has arrived and its SHA-256 has checked out. It is then put in place under the name the game asks for
  and replaces the local copy. No host, a wrong password or build, a host without a save, a damaged or
  interrupted download (`coop_join_timeout_sec`, default 20) or a place that stays taken all end the game with
  a message (exit code `0xC0DE0007`): a client that went on with its own save would play another world.
  `coop_join=off` keeps the client's own save (tests). A restarted client takes the place of a dead one once
  the host has heard nothing from it for 5 s; until then it is told "busy" and keeps asking.
- Co-op over loopback (`src/coop.cpp`): `coop_send=127.0.0.1:<port>` sends this copy's
  player (position, rotation, health, its 25 animation inputs, its root-motion speed, its
  non-zero game actions, its locomotion mode, its on-ground flag and the attack it is in;
  564-byte packets, version 8, with a
  microsecond timestamp) once per game frame from the simulation thread (`coop_rate_hz`, 60, only until the hook
  runs); `coop_listen=<port>` receives the other copy's. `coop_show=lkp` shows it with the
  game's "last known position" effect; `coop_show=puppet` picks the nearest living
  character (heap search of resident pages, retried every 10 s) and moves it every
  frame to the received player, interpolated `coop_delay_ms` (50, at most 1000) behind
  the sender. Both need `sim_hook`.
  - `coop_puppet_ai=off`: the AI tick's behavior-tree call (`2F7B40`, bytes checked) goes
    through a filter that skips the puppet's tree, so its AI no longer decides anything;
    other characters are untouched.
  - `coop_puppet_spawn=<type>` (e.g. `Talion`): spawn the puppet at the sender's position
    instead of taking over the nearest character, and give it this copy's player's behavior
    resource, so the cutscene `Talion` actor switches on and animates with the player's graph.
  - Animation inputs (on unless `coop_puppet_inputs=off`): the sender records the named
    float variables its player's update feeds the player's animation graph every frame
    (stick, speed, turning, camera: `RawMoveMag`, `RawMoveYaw`, `Game_MovementAxisX/Y`,
    `PlayerVelocity`, ...; an entry hook on the setter `176D78`, bytes checked) and sends
    them; the receiver sets them on the puppet every frame. A spawned `Talion` (the player's
    graph) then walks with the sender's own stride: moved only by them it covers the sender's
    path to within a few percent, turns and stops included (`tools/root_match.py`).
  - Buttons (game actions; on unless `coop_puppet_actions=off`, probe `puppet actions on|off`):
    the player's update fills three lists of 377 game actions on its character every frame
    (`+0x5F8` held bytes, `+0x778` pressed bytes, `+0x8F8` values; a count, then the entries),
    and the player's graph reads its buttons there (`A_Button_JustPressed` ->
    `A_Button_WaitingForUnpress` -> `Sprint`, the free-run). The sender sends its non-zero
    entries; the receiver gives the puppet full lists with them every frame and sets the two
    flags the graph's input nodes (`644AD8`) need: `character+0x113A` and `+0x1129`, both set
    to 1 by the player's update. With the flags and empty lists the game's bounds check
    (`FA9C20`) writes to address 0, so lists and flags change together. Then the stand-in
    free-runs with its own animation, about 80 ms after the sender.
  - Free-run speed, without the buttons only (on unless `coop_anim_speed=off`, anim mode):
    the stand-in's graph then walks at no more than ~230 units/s while the sender free-runs at
    600-750. The sender also sends its root-motion speed (`[player+0x11D8]` over its frame
    time), and the stand-in's movement component gets a copy of its vtable with the per-frame
    step (slot `0x80`, `191868`, bytes checked) wrapped: while the sender is over 300 units/s
    and 1.2 times the stand-in (and half a second after), the motion the stand-in's animation
    produced (`[[character+0x2C78]+0x9C]`) is scaled to the sender's speed, plus a catch-up of
    twice the gap along its own heading per second. With the buttons it is not needed (and
    made turns worse). Vaults and running jumps are not carried (not checked with the buttons).
  - The player's follower (on unless `coop_puppet_follower=off`, probe `puppet follower on|off`):
    a stand-in within ~300 units put the local player in combat (the game's combat log:
    `InCombat` TRUE, `Sheathed` FALSE, `TargetValid_ActivateRB` TRUE), and the player's
    graph walked in `Hand_Holding`, so with both copies sending each player moved differently
    beside the other's stand-in. The stand-in's player-follower byte (`character+0x23B8`;
    getter `1156AF0`, bytes checked) is set every frame: beside it the player stays out of
    combat, and the game counts it as a player ally (`IsPlayerAlly`, `366B88`).
  - Out of the attacks (unless `coop_puppet_targetable=on`, probe `puppet targetable on|off`):
    an ally can still be struck, and one or two hits killed the stand-in (the game's own
    follower setter `102F0C`, its can-damage byte `[character+0x26A8]+0x444` and its
    `SetTargetable` flag `character+0x2C16` did not stop that). Two stubs in a fifth code
    page near the image, reached by 5-byte jumps each written with one aligned 8-byte write
    (bytes checked): on the attack target check `18626C` (at its second instruction; r8 the
    candidate, `[r8+0x20]` its health object), which then rejects the stand-in, so no attack
    aims at it; and on `2C70C0`, which applies every hit, targeted (`2C642C` -> `2C658C`) or
    not (a swing without a target that sweeps through it), with `[rdx+0x28]` the target
    character: it returns at once for the stand-in. Every attacker's, not only this copy's
    player's. Area attacks (fire, explosions, auras) go neither way: a large enemy's fire took a
    stand-in from 298 to 3 with both stubs in. Every area tick asks `2E5718` about each character
    near it (`rcx` the tick, `rdx` the character; it logs `AOETick = %s; %s Evaluating Object.`
    and each reason to skip one) and adds those it accepts to the tick's hit list; a third stub
    (its entry is an aligned qword) returns at once for the stand-in. `remote` shows
    `targetable=off/<candidates rejected>/<hits dropped>/<area ticks skipped>`.
  - The sender's attacks (on unless `coop_puppet_attacks=off`, probe `puppet attacks on|off`):
    the stand-in gets no attack input of its own (the player's strikes come from command
    input, `CombatPrimary01`, not from the action lists). An attack starts in `2E297C`
    (StartAttack; it logs `Start Attack: <name>, ... Anim: '<anim>'`), which keeps the
    record at `[character+0x26A8]+0xE0` (0 once it ends) and the start time at `+0x470`
    (milliseconds). The sender sends that record's name hash (`+4`; attack records have type
    `0xA15` at `+0`) and the start time. Records sit at different addresses in each copy but
    the hashes are the same: they are the 40-byte entries of one table (`[27008F8]`: count
    `+0x28`, array `+0x38`, sorted by hash; the game's own find-by-name `1B078DC` hashes with
    `423E70` and searches it the same way). The receiver starts each new attack (id or start
    time changed) on the stand-in through `2D6B3C` (bytes checked; the player's and the AI's
    own entry: it zeroes `+0xF0`, `+0xF8`, `+0x108`, `+0x110` of the health object and jumps
    to `2E297C`), with no node, no target and scale 1. The stand-in's combat object has no
    player links (`+0x40`, `+0x50`), so nothing ends its attack (the next start logged
    `Duplicate attack`): when the sender's ends, the receiver stops it with `2EA790(health,
    0)` (bytes checked), only while `+0xE0` is still that attack and `+0xD8` is 0, as the
    game's own callers do. A strike chain (`PC_XHeld_HeavyAtk_Into_L`, then
    `PC_NT_L1_D1_F_...` ~150 ms later) then plays on the stand-in with the sender's timing,
    sword drawn, lunge included, and its sword goes back afterwards. Records also cover
    non-combat actions, which are carried the same way. `remote` shows
    `attacks=on/<started>/<stopped>`; probe `puppet attack <hash>` starts one by hand.
  - A stand-in the game unloads (freed, seen once this copy's player was ~3000 units away, and
    when this copy's player died and was put back at the tower) or kills (current health
    `[[character+0x26A8]+0x42C]` 0) is dropped and logged; a new one is spawned at the sender
    within 10 s. The spawn type is looked up in the spawn-type table (type `0xA06`) held at
    `[2702440]` (the game's queued spawns read it, `1A9212F`), else through an AI character's
    own spawn type (`+0x2E68`, then `+0x18`): after a death no AI character was loaded at the
    tower, and without the global no stand-in came back.
  - `coop_puppet_move=anim` (the one to use with a spawned `Talion`): moved only by the
    inputs (its own root motion, so its feet match the ground), and put back with a teleport
    when it is more than `coop_anim_snap` (150) units off or 150 up or down, and once the
    sender has stood still for half a second and it is more than 15 units or 15 degrees off
    (standing: under 10 units/s, or under 40 with its stick let go, since a player creeps at
    10-20 units/s for a second or more after stopping). Probe `remote` shows what this check
    last saw (`anim_frames`, `off`, `turn`, `sender_speed`, `standing_ms`, ...).
    Off the ground (unless `coop_air_follow=off`, probe `puppet air on|off`): the sender
    sends its locomotion mode (`player+0xEE0`: 1 walking or standing, 6 then 3 in a
    free-run, 2 from a jump's takeoff through the fall to the landing) and its on-ground flag
    (`player+0x112B`: 0 from ~0.1 s after takeoff and all through a climb, where the mode
    reads 2, 3, 4 and 1). A running jump is moved by the jump itself (1000+ units/s), not by
    root motion: the stand-in's graph played the jump from the buttons but stayed on the
    ledge; on a wall it was dropped to the ground below. While the sender is in mode 2 or
    its on-ground flag is 0, the stand-in's
    movement component skips its step (slot `0x80`) and its frame update (slot `0x100`,
    `2FFC1C`, which writes the transform from the movement's own state through `14554C`;
    head bytes checked), and its transform (`+0xAC` position, `+0xB8` rotation) is set to
    the sender's every frame. Not through the game's teleport: that puts a character on
    the ground below the point. On landing one teleport puts it on the ground there and its
    own movement takes over. `remote` shows `air=on/<frames>/<steps skipped>/<landings>`.
    `teleport` (default) teleports it every frame, which holds the Talion stand-in in a
    stretched pose; `both` also gives its movement controller the game's own move request
    (walk, or run above `coop_run_speed`, 300 units/s), which makes an AI body play its own
    gait; `walk` moves it only by the requests (its navmesh route; teleported when more than
    `coop_teleport_dist`, 600, behind); `none` leaves it to its animation.

Config `sowguard.ini` next to the DLL (`[sowguard]`: `profile`, `allowed_root`,
`network`, `net_trace`, `protect_extra1..4`,
`steam_cloud`, `steam_trace`, `autoload` and `autoload_*`, `background_run`, `sim_hook`,
`coop_*` (`coop_send`, `coop_listen`, `coop_rate_hz`, `coop_delay_ms`, `coop_show`,
`coop_puppet_ai`, `coop_puppet_move`, `coop_puppet_spawn`, `coop_puppet_inputs`, `coop_puppet_actions`,
`coop_puppet_follower`, `coop_puppet_targetable`, `coop_air_follow`, `coop_puppet_attacks`, `coop_anim_speed`,
`coop_run_speed`, `coop_teleport_dist`, `coop_anim_snap`), `probe`,
`probe_write`; test-only keys in `src/test_guard.cpp`). Five code pages within rel32
reach of the game image are reserved at start-up, before the game's heap fills that range.
Log: `<profile>\sowguard.log`.

Exit codes: `0xC0DE0001` config, `…02` outside allowed_root, `…03` unknown exe,
`…04` steam_appid.txt, `…05` hook refused, `…06` Steam guard, `…10` protected write
blocked.

## sowprobe (`src/probe.cpp`, `tools/probe.py`)

Read-only inspection of the running game, enabled with `probe=1`. A thread waits until
the game has unpacked itself (`probe_ready_pattern`, default the GameClient load
`48 8B 0D ? ? ? ? E8 ? ? ? ? C7 47`; `probe_wait_sec`, default 300) and logs how long that
took. Another serves `\\.\pipe\sowprobe-<pid>` (local clients only; name written to
`<profile>\sowprobe.txt`): `ping`, `module`, `scan <pattern> [max=N]`, `read <addr> <len>`,
`rip <addr> <dispoff> <len>`, `deref <addr> [off ...]`; addresses `rva:<hex>` or
`abs:<hex>`. Reads only committed, readable, non-guard pages.

`player` reads the player (`[[GameClient 26FFA98]+0x888]`): position `+0xAC`, rotation
quaternion `+0xB8`, and health/max/fraction on `[player+0x26A8]` (`+0x42C`, `+0x428`,
`+0x430`). With `probe_write=1` and the virtual pad (`autoload=1`): `pad <buttons hex>
<lx> <ly> <rx> <ry> <ms>` holds pad input, and `save` runs the game's `Player.Save`
handler (bytes checked) from the next pad read once the world has started; the game
saves ~0.5 s later.

`dmglog` (session cm) lists the last 32 damage reports: on the host each enemy swing that reached the decoy (attacker, attack
id, the attacker's distance from the hit: an arrow hits from afar), on the client what it did with each damage message
(`a` applied, `f` failed, `x` no proxy). The `enemies` status line also counts the game-reference work of session cm:
`unlink_fix`/`unlink_repaired`/`unlink_left`/`unlink_faults` (the repair in the game's reference-list destructor `4DD18C`,
`src/listfix.h`; `coop_unlink_fix=off` turns it off), `retarget_refs`/`retarget_raw` (host: targets the retarget swap moved
with the game's assign `3F52B8` / wrote alone; `coop_retarget_refs=off` or `coopset retargetrefs off` writes them alone) and
`member_links`/`member_links_raw` (client: a captain's member linked to its proxy with the game's assign / written alone),
`decoy_limbo`/`decoy_limbo_gone` (host: removed decoys kept in the proxy set, their AI off, until the game deletes them;
`coop_decoy_limbo=off` or `coopset decoylimbo off` drops them at once, the old way). `kill abs:<character> [abs:<attacker>]`
takes an optional attacker (default: this copy's player). `injectdmg <enemy id> <attack hex>` (client, `probe_write=1`) queues a host damage message for that
enemy id at this player's place (a test of `coop_dmg_borrow`).

Also: `findq <hex value> [max]` lists heap qwords equal to a value (resident pages only);
`remote` shows what the co-op receiver has; `puppet` shows the stand-in. With
`probe_write=1`: `write <addr> <hex bytes>`, `lkp <x> <y> <z>` (the effect, from the
simulation thread), `count <vtable rva hex> <first slot> <slots> <ms>` (calls per vtable
slot and the calling thread, to find a per-frame function), `stacks <addr> <entry bytes
hex> <ms> [first-argument qword hex]` (the distinct call stacks that reach a function:
a jmp at its entry for ms, the replaced bytes relocated into a register-preserving stub
with unwind info, then restored; the entry bytes must be whole instructions, not
RIP-relative; it also shows the first call's `rcx, rdx, r8, r9`), `teleport <x> <y> <z>` (this
copy's player, keeping its rotation), `spawn <type> [x y z]` (a character of a spawnable
type such as `Orc_Infantry` or `Talion`, through the game's own spawn call; optionally built
with another appearance record, or `watch` to catch what switches the new character on; no
offline test yet), `look <base> <donor> <offset hex>...` (a private appearance record mixing two),
`animres <character> <resource>` (set a behavior resource), `watch` (the write watch's hits),
`trace <addr> <ms> [rcx abs:<hex>] [names|args]` (the return addresses of calls to any address, no patch: an
execute breakpoint in debug register 1 of the simulation thread, counted per caller for calls
whose `rcx` matches; `names`: counted by the second argument read as a C string, with the
last `xmm2`, for a variable setter such as `176D78`; `args`: counted by `[r9+0x28]`, with the
first call's caller, `rcx`, `[rcx+0x28]` and `r9`), `wtrace <addr> <ms> [1|2|4|8] [rw]` (the
instructions on the simulation thread that write an aligned address, or with `rw` read or
write it, each with its first call stack: a data breakpoint in debug register 2),
`puppet nearest | off | abs:<character> | ai on|off | walk <x> <y> <z> <1 walk|2 run>
[speed factor] | idle | follow anim|walk|teleport|both|none | inputs on|off | actions on|off |
follower on|off | targetable on|off | air on|off | speed on|off | snap <units> | mirror on|off | hold <ms> <index>[=value]...` (`mirror`: the puppet
gets this copy's own player's animation inputs, a local test of the replay; `hold`: an
experiment, these game actions held on the puppet for a while, its flags left as they are),
`inputs` (the recorded animation inputs, and any other float the player's graph gets).
Access violations in the game's code on any thread are logged with their call stack
(`crash:` lines, the first 16).

Tools besides `probe.py`: `shot.ps1` (a test copy's window via `PrintWindow`), `strip.ps1`
(crops of several captures side by side), `view-standin.ps1` (B faces A's start, A walks,
B is captured), `view-steer.ps1` (the same with A steered along a world direction), `steer.py` (A walks along a world direction, its stick corrected for the camera every 100 ms),
`walk_dir.py` (a cruder one-off stick calibration), `root_match.py`
(A's path against the unpinned stand-in's), `drift.py` (the stand-in's gap to A over steered
legs, free-run with the A button too, and its put-backs; `-v` per sample with both heights),
`run_compare.py` (A against the stand-in over one run with button phases, put-backs off),
`puppet_motion.py` (both characters' per-frame root motion), `view-sprint.ps1` (B captured
while A runs, then free-runs), `run_survey.py` / `run_trace.py` (where A can free-run on flat
ground), `pos_writers.py` (which code writes A's position, via `wtrace`), `find_vslot.py` /
`vslots.py` (objects with a given function in a vtable slot; a vtable's slots),
`field_diff.py` / `int_diff.py` / `motion_fields.py` (fields that change between walking and
free-running), `puppet_vars.py` (the variables the stand-in's graph gets, by name),
`action_copy.py` (the free-run experiment with the player's action arrays), `graphs.py`
(the animation graph instances, each one's last started action, the player's and the
stand-in's marked), `graph_actions.py` (A's and the stand-in's graph actions while A moves:
walk, `Sprint`, ...), `action_edges.py` (which game actions change when a button is
pressed), `flag_try.py` / `input_flags_try.py` (the experiments that found the stand-in's
input flags), `disp_refs.py` (instructions with a given displacement), `settle_hunt.py` (walking squares until the stand-in ends far from A, then what anim mode saw), `graph_callers.py` (which code sends
messages to the player's animation graph, walking against an action, via `trace`), `gait_speed.py` (the puppet's own speed per move
request), `anim_inputs.py` / `anim_bools.py` (which float / bool animation variables an object
gets, via `stacks`), `wait-world.ps1`, `walk-loop.sh`, `records.py`, `dis_annot.py`,
`refs_to.py`, `animvars.py`, `logsum.py`, `dumpf.py`, `puppet_speed.py`, `remote_speed.py`.

```
python -I tools\probe.py <profile dir> ping "scan 48 8B 0D ? ? ? ? E8" "read rva:1C76D08 32"
```

## Playing with a friend over the internet (not yet tried with a second person)

The guard keeps the game itself offline; `network=coop_peer` lets only the co-op socket talk to the other copy.
Both players need the same game build and mod build, and `steam_cloud=sandbox` (the client plays in the host's
save, which is copied into its own sandbox, never into its real saves).

1. Put both PCs on one virtual LAN with an overlay VPN: Tailscale, ZeroTier or Radmin VPN (free, no port
   forwarding). Privacy VPNs that hide your address do not work for this. Each PC gets a private address
   (Tailscale: 100.x.y.z).
2. Host's `sowguard.ini`: `network=coop_peer`, `coop_role=host`, `coop_port=47011`, optionally
   `coop_password=...` and `coop_bind=<the VPN address>` (default under coop_peer: all addresses). Start the
   game. `sowguard.log` then lists lines `coop: a client on this network or VPN joins with
   coop_host=<address>:47011`; the VPN's address is the one to give your friend. The host's firewall may ask to
   allow the game once (UDP, private network).
3. Client's `sowguard.ini`: `network=coop_peer`, `coop_role=client`, `coop_host=<that address>:47011`, the
   same `coop_password`. Start the game after the host. The game waits at its save for the host's, copies it,
   and closes with a message if it cannot (no host, wrong password or build, a host that has not saved yet).
4. Plain port forwarding also works: forward UDP `coop_port` to the host and give the client the host's public
   address.

What is covered by tests: the same thing between two copies on one PC through a simulated bad network (loss,
jitter, blackouts) and over a LAN address. Not tried: two PCs, a real VPN, a real router.
## Build and test (no game needed)

```
build.cmd
build\test_guard.exe run <scratch dir> <abs path>\build\bink2w64.dll <original bink2w64.dll, i.e. bink2w64_.dll once staged> <ShadowOfWar.exe>
build\test_guard.exe mutant <scratch dir> <abs path>\build\mutant\bink2w64.dll <original bink2w64.dll>
```

The scratch dir must not be inside `%LOCALAPPDATA%` (the guard refuses a test owner tree inside the real one; every case then
fails with `0xC0DE0001`). The four `coop_peer` cases listen on this PC's real address, and Windows' firewall asks about each
case's copy of `test_guard.exe` (a new folder every run); `set SOWGUARD_TEST_NO_PEER=1` skips them (52 cases) for runs that do
not touch the network code.

`run` builds a fake owner tree per case and must print `RESULT: PASS (53 of 53 cases
passed)` (a case name as a sixth argument runs only that case). `mutant` runs a build
with redirect, guard, offline mode, Steam guard, the virtual pad,
background running, the simulation-thread hook and the `stacks` patch compiled out (and
a debug-print stub that loses r9) and must catch all 20 dependent cases. The co-op
cases use a fake player, fake game functions with the game's byte shapes (teleport,
move request, the AI tick's tree call, the tree, the float animation-variable setter, the
AI movement component's step) and a fake simulation thread; `coop_puppet_walk` also checks
that the sender's animation inputs reach the puppet's animation object, and nothing else does,
that outside anim mode the puppet keeps its own root-motion speed, and that a sender without
buttons gives it full, empty action lists and the player flags; `coop_puppet_recv` (a
receiver alone, fed packets by the test, follow `anim`) that a receiver which records nothing
itself still applies them, that the puppet is left alone while it is where the sender is and
put back once the sender is 500 units away, that it is put back while the sender creeps at 20
units/s with its stick let go (the fake teleport moves the fake stand-in, as the game's does),
that its action lists are full and hold the sender's buttons with the flags set, that it
keeps its own root-motion speed then, that it is marked as the player's follower, that
fixtures with the game's prologues of the target check and the hit (aligned as in the game)
reject it and drop hits on it while accepting another character, that while the sender is
in the air (locomotion mode 2, on an arc ~100 units up) its own transform follows it with its
movement step and update skipped (the fake update puts it back on the ground otherwise) and
that it is teleported onto the ground on landing, and that it is dropped once the game frees
it (then nothing is skipped); `coop_puppet_recv_nobuttons` (the same
with `coop_puppet_actions=off`, `coop_puppet_follower=off` and `coop_puppet_targetable=on`)
that lists and flags stay an AI's, its own root motion (200 units/s) is scaled to the
sender's (600) before its movement step, its follower byte stays 0, the target check and
the hit accept it, and that it is dropped once killed. Neither
touches real user folders (test mode refuses real paths, so the scratch dir must not be
under `%LOCALAPPDATA%`, which rules out the system temp folder; `test-run*` here is
git-ignored).

## Not covered yet

- DNS lookups still work (Windows resolves names in the DNS Client service, outside the
  game process); only the connection afterwards fails.
- AFD request layouts were measured on Windows 11 26200 (`test_guard.exe probe`); other
  Windows builds may differ. A request the guard cannot parse is blocked, not passed.

- Test copies only, never the owner's install. Ran in test copies of build 3095001 (with
  its SteamStub and Denuvo); any other build is refused and untested.
- 8.3 short names are not resolved (the source areas are also protected, so a short-name
  write to them would not be redirected; it would only be caught if it matched).
- Writes by other processes (Steam client itself) are outside this process's control.
