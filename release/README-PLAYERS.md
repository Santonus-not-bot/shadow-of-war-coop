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
- The game's slow motion (combat finishers, wraith aiming, domination scenes) is switched off on both sides, so neither player slows the other's world.
- A client that quits or crashes can rejoin; the host forgets a lost client after a minute.

## Known problems

- **Not tried on two PCs.** Only tested on one PC.
- If the **host dies**, the game's death screen pauses the host's world, so the client's view of enemies stands still
  until the host continues. If the client dies, the game's own death flow runs on its copy.
- The client cannot fast-travel by itself: if it does (or respawns far away), it is taken back to the host about 3 s after it arrives. Captains, Nemesis
  events, fortress sieges and world events are the host's only; the client does not see them as such.
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
`coop_enemies=on|off`, `coop_decoy=on|off` (host), `coop_follow_travel=on|off` (client), `coop_slowmo=on` (keeps the game's slow motion; off by default in co-op), `coop_enemy_radius`
(default 3500), `coop_join=off`, `coop_join_timeout_sec` (default 20), `autoload_max_presses` (how many times the mod
presses Continue for you at the menus; 0: none, you click). The full list is in `README.md`.

## If something goes wrong

- A message box at start: read it; it names the cause. The log (`<CopyDir>\profile\sowguard.log`) has more.
- "The host did not answer": the host is not running yet, the address or port is wrong, a firewall blocks UDP, or the
  VPN is down (ping the host's VPN address).
- "The co-op password is wrong" / "a different version": use the same password and the same mod build on both sides.
- Stuck at a black screen while joining: wait up to 20 s; if the host is paused in a death screen, press Back there.
