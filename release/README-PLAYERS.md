# Shadow of War co-op: playing it

Two players, one host and one client, each on their own PC with their own copy of
*Middle-earth: Shadow of War* (Steam build 3095001). The host's world is the real one: the client joins it, sees the
host's enemies, fights them, and is hurt by them. Progress stays in the host's world; the client's own saves are not
touched.

**Status: early.** Everything below was tested with two game copies on one PC, over loopback and over a simulated bad
network, never yet between two PCs over a real network. Read "What works" and "Known problems" first.

## What you need

- Windows 11, the game on Steam (build 3095001; both players the same), Steam running.
- About **110 GB of free disk space** each: the mod runs in its own copy of the game folder (see "Safety").
- A way for the two PCs to reach each other: an **overlay VPN** (Tailscale, ZeroTier or Radmin VPN; free, no router
  settings) or a forwarded UDP port. A VPN that hides your address (a privacy VPN) does not work.
- `bink2w64.dll`, the mod's build (from this project's releases, or built with `build.cmd`, which needs Visual Studio
  2022 with the C++ tools).

## Setting up (each player, once)

1. Install the overlay VPN on both PCs and join the same network; note each PC's VPN address (Tailscale: `100.x.y.z`).
2. Make the co-op copy with `setup-copy.ps1` (PowerShell, in this folder). Pick a folder on a drive with enough room,
   outside Steam's folders.
   - **Host:**
     ```
     .\setup-copy.ps1 -GameDir "<Steam>\steamapps\common\Middle-earth Shadow of War" -CopyDir "E:\SoWCoop" `
                      -ModDll .\bink2w64.dll -Role host -SaveFile "<path to your save>" -Password <a word>
     ```
     `-SaveFile` is the save you want to play (`<id>_ShadowOfWar.sav`, from the game's save folder under
     `%LOCALAPPDATA%\WB Games\Shadow of War`). It is copied; the original stays where it is.
   - **Client:**
     ```
     .\setup-copy.ps1 -GameDir "<Steam>\steamapps\common\Middle-earth Shadow of War" -CopyDir "E:\SoWCoop" `
                      -ModDll .\bink2w64.dll -Role client -HostAddress <host's VPN address>:47011 -Password <the same word>
     ```
     The client needs no save: it takes the host's when it joins.
3. `-DryRun` shows what a command would do without doing it. The script refuses a folder inside Steam, a game that is
   not the expected build, and a disk that is too small.

## Playing

1. **Host** starts first: `.\launch.ps1 -CopyDir "E:\SoWCoop"`, then continues the campaign as usual. The host's
   `<CopyDir>\profile\sowguard.log` lists the address to give the client (`a client on this network or VPN joins with
   coop_host=...`); open the host's firewall for the game once if Windows asks (UDP, private network).
2. **Client** starts after the host is up. While the game starts, the mod connects and copies the host's save; the
   game then loads it. If that fails, the game closes with a message saying why (no host, wrong password, other
   version, the host has no save yet, the transfer was damaged or too slow).
3. Both play in the same world. A client that starts far from where the host is now is taken next to the host once it is in the world; the host's enemies appear on the
   client; the client's strikes hurt them; they hit back.
4. When the host fast-travels (or respawns), the client follows within a couple of seconds. A client that fast-travels (or respawns) alone is taken back to the host a few seconds after it arrives.

To stop, quit the game normally. To remove everything, delete the copy folder (`E:\SoWCoop`).

## What works

- The other player is shown as a stand-in that walks, runs, climbs, falls and fights with the other's animations.
- The client joins the host's save automatically and checks it (SHA-256).
- The host's enemies (up to 16 near either player) are shown on the client and follow within about 40 units; the
  client's own random enemies are kept away.
- The client's strikes hurt the host's enemies (kills included); enemies' swings that reach the client hurt it, and
  enemies turn on the client through an invisible ally the host keeps at the client's place.
- Fast travel by the host takes the client along.
- The host's Nemesis events (captains threatening, duelling, dying, being promoted; the player's kills and deaths and the
  promotions they cause) are sent to the client, whose game runs the same events on its own Nemesis world, so the two
  worlds keep the same captains in the same places. The client's own Nemesis events and ambushes are off.
- A captain the host meets is shown on the client as himself: the client's game builds him from its own copy of his
  Nemesis record (made from the host's if it has none), the way the game builds its own captains. What was checked: the same Nemesis record, seed and faction on both sides, and a screenshot of him on the client.
- The game's slow motion (combat finishers, wraith aiming, domination scenes) is switched off on both sides, so neither player slows the other's world.
- A client that quits or crashes can rejoin; the host forgets a lost client after a minute.
- Enemies on the client play the host's enemies' actions and attack moves; an enemy that dies on the host dies on the
  client the game's way (death animation, body). Enemies near the client are pointed at it (the nearer player) and idle
  ones near it are woken.
- The counter prompt shows on the client for many (not all) attacks aimed at it.
- The client's strikes make the enemy react on the client's screen (staggered, knocked back) and a killed enemy leaves its
  body there (`coop_proxy_react`, on by default; checked in frames). A counter by the client was seen once: the prompt, the
  counter move and the enemy thrown.
- The client's arrows count on the host (an arrow kill on the client becomes strikes on the host's enemy).

## Known problems

- **Not tried on two PCs.** Only tested on one PC.
- **Enemies stopping their attacks on the client: likely fixed in this build, not yet confirmed in play.** In earlier
  builds the host's enemies went after the client but stood off and never struck once the client had moved away from
  where it joined: the invisible ally the host keeps at the client's place stayed, for the game, where it was made. It is
  now moved with the game's own teleport (`coop_decoy_reanchor`). In tests far from the join point: 0 enemy swings in 60 s
  before, 17-36 in 50 s after. If enemies still stand off, the host's log has `coop: stuck:` lines: please send them.
- **The host's archers do not hurt the client.** Their arrows never landed on the client's place on the host; the fix
  above may change that (not tried).
- With the Nazgul gear set, some enemies you kill rise as undead allies and fight the other orcs: orcs fighting orcs
  near you can be that (the game), not the mod.
- The counter prompt is missing for some attacks aimed at the client. Counters were tried once only.
- A finisher by the client on an enemy is buggy (the enemy can vanish). A downed client is not attacked with a kill move.
- An arrow's damage is sent as a few sword strikes, so the amount is approximate.
- If the host's game stops (the death screen, a loading screen, its pause menu), the client gets no news from it: after
  5 seconds the host's enemies disappear from the client's screen, and they come back when the host plays on. (The
  host's death screen itself was not tried; a stop of the host's game was.) If the client dies, the game's own death
  flow runs on its copy.
- The client cannot fast-travel by itself: if it does (or respawns far away), it is taken back to the host about 3 s after it arrives. Fortress
  sieges and other world events are the host's only. Setting `coop_nemesis=share` in the client's `sowguard.ini`
  turns off the client running the host's Nemesis events (it only logs them).
- A captain the host's game made after the client joined (a replacement for a dead one, a promoted orc) is sent to the client
  as the game's own text for him (about 8 KB, once) and made there in the same slot, so the host's events about him apply and
  he is shown as himself. Checked on one PC: members made that way read back the same as the host's, and the events the
  client applied matched the host's in order and names. What the host's kill code sets on a member (he is dead, how he died) is
  not carried: only the host's Nemesis events are, so a captain the host killed may still be alive in the client's Nemesis
  world. `coop_member_data=off` in the host's `sowguard.ini` stops the sending; `coop_captain_look=off` in the client's shows
  every captain as an ordinary orc.
- Animation: the stand-in has no leap animation (it keeps its position right, the pose is wrong); the client sees no
  hit reaction on an enemy until the host's enemy reacts (about a tenth of a second).
- The decoy ally may show next to the client's stand-in on the host's screen.
- Some enemy types may not animate their walk (the mod plays the orc's walk and run states by name).
- Spawning enemies far from the client's player can fail; the mod tries again.
- Startup: the game sometimes fails to start with a Steam-related message when it is launched right after another
  copy was killed; start it again.

## Safety: what the mod touches

- It reads your Steam install (to copy it) and your save file (only if you give `-SaveFile`). It never writes to the
  install, your saves, or your Steam cloud: in the copy it redirects the game's save folders and Steam cloud to
  `<CopyDir>\profile`, and ends the game if anything tries to write outside it.
- The game itself stays offline: every connection of the game except the mod's own co-op socket is refused. The co-op
  socket talks only to the other player's address (a client: the host's address; a host: whoever sends valid packets;
  a password keeps strangers off the port; nothing is encrypted).
- No game files or assets are in this project. Nothing is sent anywhere except to the other player.
- Stats and achievements of the copy are blocked (they would otherwise reach your Steam account).

## Settings (`sowguard.ini`, written by `setup-copy.ps1`)

`coop_role=host|client`, `coop_port` (host, default 47011), `coop_host=<address>:<port>` (client), `coop_password`,
`coop_enemies=on|off`, `coop_decoy=on|off` (host), `coop_anim_mirror=on|off`, `coop_wake_radius` (host, default 600, 0: off),
`coop_proxy_react=on|off` and `coop_local_damage=on|off` (client), `coop_hit_by=poke|player|decoy` (host: how the client's
hits are applied; `player` is the way builds before 2026-11-05 did it), `coop_standin_hits=off|on` (`on`: the other player's
picture also hurts enemies with its copied swings, as before 2026-11-05), `coop_decoy_reanchor` (host, default 150, 0: off: how far the client's place may move before the host's game is told
where it is), `coop_stuck_sec` (host, default 20, 0: off: an enemy
that goes after the client but has not swung at it for this long is nudged; each time is logged as `coop: stuck:`, which helps
with the problem below), `coop_follow_travel=on|off` (client), `coop_slowmo=on` (keeps the game's slow motion, ended after 4 s on a client; off by default in co-op), `coop_enemy_radius`
(default 3500), `coop_join=off`, `coop_join_timeout_sec` (default 20), `autoload_max_presses` (how many times the mod
presses Continue for you at the menus; 0: none, you click). The full list is in `README.md`.

## Sending logs

The log is `<CopyDir>\profile\sowguard.log` (both players). It contains file paths, which include your Windows user
name: replace it (for example with `USER`) before you send the file. Say which player (host or client) it is from, what
you did and roughly when the problem happened.

## If something goes wrong

- A message box at start: read it; it names the cause. The log (`<CopyDir>\profile\sowguard.log`) has more.
- "The host did not answer": the host is not running yet, the address or port is wrong, a firewall blocks UDP, or the
  VPN is down (ping the host's VPN address).
- "The co-op password is wrong" / "a different version": use the same password and the same mod build on both sides.
- Stuck at a black screen while joining: wait up to 20 s; if the host is paused in a death screen, press Back there.
