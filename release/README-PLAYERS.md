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
- **Enemies can still stop attacking the client.** The host keeps an invisible ally at the client's place for its enemies to
  attack. Moved by the game's teleport it kept working over short moves, but not after a long jump (the way down from where the
  client joins, a respawn): in tests 0 enemy swings in 120 s. This build makes the ally again after a jump of more than 1000
  units (`coop_decoy_remake`), and in a 5-minute bot fight the client took 57 swings instead of 13. In that fight the swings
  still stopped after about 90 seconds, with wild wargs, an olog and camp orcs joining in; with only ordinary orcs around, three
  4-minute tests had no stop. In long bot fights on later builds (60 and 90 minutes, archers included) enemies swung at the
  client in most 30-second stretches (111 of 119 in the 60-minute run), with pauses of up to a minute. If it happens, the host's
  log has `coop: stuck:` lines: please send them.
- **The client's game could freeze** ("Not Responding"; four times in bot tests). Found and fixed in this build: when a captain's
  picture on the client was removed, the mod cleared the captain's link to it the wrong way, and the game's own clean-up then
  looped for ever when that character went. The mod now uses the game's own function for that link, and the game's clean-up is
  also guarded: a link left that way is undone instead of looping (logged as `a reference linked with no target was unlinked`;
  `coop_unlink_fix=off` turns the guard off). In bot tests since: no freeze, and the guard never had to act. If the client still
  freezes, close it and join again, and please send both logs.
- **The host's picture of the client can fall behind** (it moves by small jumps and got stuck 300-1,700 units back while the
  client ran over rough ground). The host's invisible ally, which its enemies attack, now stands where the client says it is
  instead of where that picture is (`coop_decoy_follow=standin` in the host's `sowguard.ini`: the old way): in a 10-minute bot
  run it stayed within 21 units of the client the whole time, where the old way lost the client after 90 seconds on the same
  route. The picture itself can still lag behind on the host's screen.
- **The host's game could crash after it made its invisible ally again** (each time the client makes a long jump: joining, a
  respawn, a fast travel). Found and fixed in this build: the old ally's own AI was switched back on while the game was taking it
  apart. A bot test that remade the ally ~4 times a minute crashed the host after 6-9 minutes before the fix and ran 16 minutes
  (61 remakes) without a crash after it. The mod also points enemies at the ally through the game's own links now
  (`coop_retarget_refs=off` and `coop_decoy_limbo=off` in the host's `sowguard.ini` go back to the old ways). Old allies are not
  deleted by the game: after about 40 remakes in one session the enemies stopped attacking in that test; restarting both games
  clears them.
- **The client's game could crash on a full audio queue** (the game's own check: "m_vFutureKeys is at capacity ... Game will
  crash"; seen 3 times in tests, each right after the client arrived among the host's enemies). The game has a setting to drop
  sounds instead of crashing there (`GameAudioManager.FatalErrorOnFutureKeyFull`); the mod turns it on in co-op
  (`coop_audio_keys_fatal=on` in `sowguard.ini` keeps the crash). Some sounds may be lost when it happens.
- **The client's game could crash in long fights** on another of the game's own checks (a 256-entry table in its AI; three times
  in tests, after 2-13 minutes of fighting). Found: the client's pictures of the host's enemies have their own AI switched off,
  and the game counts on that AI to empty the table. The client now makes a picture again (it blinks out and back) before its
  table fills (`coop_proxy_table_max`, default 200; 0 turns this off). Each player's picture of the other is made again the same
  way when its table fills (it disappears for a few seconds). Since this build the mod also counts the table's list itself:
  after the client died, the client's picture of the host kept a list that grew about 2 entries a second while the host fought,
  and the game never counted it. That picture is now made again about every 2 minutes of the host fighting.
- **The host's tamed wargs could hurt the client.** A tamed warg's running lunge passes through the host's picture of the client
  and was sent to the client as damage. Since this build hits by the host's tamed wargs and followers are not sent
  (`coop_ally_hits=on` sends them again).
- **A player's picture of the other could slide and jump instead of walking.** The game has a switch for each character's AI
  update; when it is off, the picture moves only when the mod puts it back in place (jumps of about 150 units) and its AI table
  is never emptied. What turns it off in play is not known yet (likely a death). Since this build a picture whose switch stays
  off for 6 seconds is made again (it disappears for a moment and walks normally after; `coop_standin_agent_off`, seconds,
  default 6, 0 turns this off).
- **The host's archers hurt the client** since this build: their arrows hit the host's picture of the client (it stands where
  the invisible ally does), and that damage is now sent to the client as an archer's shot (`coop_decoy_hurt_send=off` stops it).
  Archers often keep their distance and shoot less than at the host's player.
- With the Nazgul gear set, some enemies you kill rise as undead allies and fight the other orcs: orcs fighting orcs
  near you can be that (the game), not the mod.
- Counters work for the client and protect it: in bot tests it countered many attacks (some killed the orc on both screens), and
  a swing it countered did not take its health. The counter prompt can still be missing for some attacks (cause not known; some
  attacks cannot be countered in the game either).
- A finisher by the client on an enemy is buggy (the enemy can vanish). A downed client is not attacked with a kill move.
- Since this build the damage of the client's arrows (and other damage its enemies' pictures take outside sword hits) reaches the
  host's enemy exactly; before, it was sent as stock sword strikes (`coop_exact_losses=off` keeps that).
- Since this build an enemy swing that reached both the host's picture of the client and the invisible ally is sent once (it
  was sent twice, so the client took some swings twice).
- If the host's game stops sending (a loading screen, its pause menu), the client gets no news from it: after 5 seconds the
  host's enemies disappear from the client's screen, and they come back when the host plays on.
- If the host dies, the client stays where it is (and keeps fighting) through the host's death scene, death screens and the
  killer's presentation, and is taken to the host's respawn point once the host walks again (at most 2 minutes;
  `coop_travel_wait_host=off`: at once). Checked with bots: the client arrived the moment the killer's presentation ended. If the client dies, the game's own death flow runs on its copy; it respawns at its own
  respawn point and is taken back to the host about 3 s later. Both were checked with bots on one PC.
- When a captain ambushes the host, the client's game plays the host's Nemesis events too (the captain's introduction, his
  showdown, his kill of the host's player) and may show a screen the client has to press through.
- After a death, a player's picture of the other once stood still far away and would not follow (seen on one PC, cause not
  known). Since this build such a picture is moved straight to the other player whenever the game does not move it.
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
where it is), `coop_decoy_remake` (host, default 1000, 0: off: a jump of the client's place this long makes the host's
invisible ally again there), `coop_decoy_stall` (host, default 0: off; seconds: when enemies near that ally have not hit
it for this long it is made again; not tested), `coop_unlink_fix=on|off` (both; default on: the guard in the game's clean-up
described under "Known problems"), `coop_retarget_refs=on|off` and `coop_decoy_limbo=on|off` (host; default on), `coop_decoy_follow=remote|standin` (host; default remote), `coop_proxy_table_max` (client; default 200), `coop_standin_agent_off` (both; seconds, default 6, 0 off), `coop_hunt_release_dead=on|off` (host; default on: a dead enemy no longer keeps one of the 32 slots that point enemies at the client), `coop_ally_hits=on|off` (host; default off: hits of the host's tamed wargs and followers on the client's picture are not sent to the client), `coop_decoy_hurt_send=on|off` (host; default on: an arrow that hits the ally is sent to the client as an archer's shot),
`coop_decoy_still` (host; default 25: the ally is not moved while within that many units of its place; 0:
moved every frame), `coop_audio_keys_fatal=off|on` (both; default off), `coop_ai_param_guard=on|off` (both; default on: a guard where the host's game crashed twice in its AI), `coop_counter_heal=off|on` (client; default off, not needed: a swing the client counters does not hurt it, checked; the option gives back the health an enemy's last swing took when the client counters it within 1.5 s), `coop_hunt_to_player=off|on` (host; default off: enemies are pointed only at the client's place, never back at the host's player; `on` is the older behavior, which may have caused those crashes), `coop_dmg_borrow=on|off` (client; default on: an
enemy's hit that arrives while the client has no picture of that enemy is applied with the nearest picture instead of being
dropped), `coop_stuck_sec` (host, default 20, 0: off: an enemy
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
