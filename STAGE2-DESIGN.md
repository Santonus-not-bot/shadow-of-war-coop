# Stage 2: fights (design, for review; nothing built yet)

Goal: two players fight the same enemies. The host's world is the real one : enemies,
captains and world events run on the host; the client's own game tracks the client player's health and tells the
host only whether it is alive. Scope: two players exploring and fighting, plain enemies first.

## What is known (checked in the running game, sources in `games/shadow-of-war/NOTES.md`)

1. **The two worlds do not hold the same ambient enemies.** Both copies load the same save, but 18 and 19 living
   characters were found, with different groups; after about a minute of AI the positions have drifted apart
   (only 4 of 18 pair up by health and position within 50 units). The qword at character `+0x2338` is unique per
   character and was equal in both copies for exactly one character, the 11,165-health unique; the others differ.
   So an enemy cannot be matched across copies by a stored id, except saved uniques. (session bf)
2. **Enemy AI does not pick the stand-in as an opponent**, targetable or not, follower mark on or off (ap).
3. **The stand-in's strikes did not hurt a living enemy** (one run: 600 health stayed 600, no damage line; ar).
   The player's hits may depend on player-only links (`+0x40`, `+0x50` of the health object, which the stand-in
   lacks).
4. **Attacks are one mechanism**: strikes, counters, dashes, hit reactions are attack records started through
   `2D6B3C`; the stand-in already plays the sender's (aq, ar). So a proxy enemy can play an attack by its record id.
5. Every hit goes through `2C70C0`, already stubbed for the stand-in; the area filter is `2E5718`.
6. A death opens the Nemesis screen and pauses the world on that copy (ar).

## The model

- **Host:** unchanged game. Each enemy within range of either player is sent as a state: a per-session id, type,
  position and rotation, animation inputs, current attack id and start time (same fields the player state has),
  health fraction, alive.
- **Client:** its own ambient enemies are suppressed (point 1: they would be a second, different population).
  Each host enemy is shown as a **proxy**: a spawned character of that type, AI off, moved and animated from the
  stream exactly as the player stand-in is today. Created and freed as enemies enter and leave range or die.
- **Client hits an enemy:** the client's strike lands on a proxy (hit stub on the client) and becomes an event
  `HIT(enemy id, attack record id)`. The host applies it to the real enemy through the game's own hit function
  with the host's stand-in of the client as attacker. The reaction (recoil, death) comes back in the enemy's stream.
- **An enemy hits the client:** the host's enemies must attack the host's stand-in of the client (needs the
  AI-target change, point 2). The hit on the stand-in is turned into `DAMAGE(attack record id)` and applied to the
  client's own player by its own copy (health stays local, as decided).
- **Alive/dead:** STATE already carries health. The host shows the client's stand-in dead or alive from it. Quickest
  rule for deaths: each copy follows the game's own death flow; the other player carries on; the dead one comes
  back by the game's rules. A known limit: while the host sits in its Nemesis screen the world is paused, so the
  client's proxies freeze.
- **Captains and Nemesis events** stay on the host (hooks `HandleFactionEventAction`, `OnMemberKilled...`); the
  client shows them as ordinary proxies at first.

## First result of experiment 0b (session bg)

An enemy cannot be driven like the stand-in by replaying player input names: a fighting infantry orc's
animation object gets only two names through that setter. A spawned `Orc_Infantry` made the stand-in with its AI
off does not carry out move requests (the AI tree does that), so `walk` mode does not work for it. Still open: how
an AI character's legs are driven, and what a teleported AI-off orc looks like. Two ways forward: (i) find the
function that sets AI locomotion variables and replay those, (ii) keep the proxy's AI running with a goal and no
opponent. Spawning orcs by type name (`Orc_Infantry`) works.
## Result of experiment 0b, second look (session bh): proxies can work

A spawned, AI-off `Orc_Infantry` moved by teleports and started on the graph action `WalkForward` by name walks
convincingly across the screen (8 frames, legs in stride). So the proxy recipe is: teleport the position, start
the locomotion state the host's enemy is in by name (Idle, WalkForward, Run), start attacks by record id; no
animation variable names are needed. Still to prove: `Run`, turns and the combat states, and several proxies
at once (the stand-in globals hold one).
## Step 1 (view-only proxies) built and tried live (session bl)

`coop_enemies=on` on both copies. The host lists its AI characters through the behavior-tree filter and every 66 ms
announces (reliable NEW/GONE events: type name) and streams (a lossy second state channel: up to 16 enemies of 32
bytes) the ones within `coop_enemy_radius` (3500) of either player. The client spawns a proxy of each type
(AI off, follower byte), places it by writing its transform with 0.5 smoothing and 150 ms of look-ahead, starts the
attack record the host's enemy is in, plays Idle/WalkForward/Run by name from the measured speed, copies its health,
rejects every ambient spawn candidate, and asks every other AI character to despawn. Two copies loaded from the same
save, both players at the same spot among the host's enemies: 14 to 16 living characters on each side, 14 to 15
paired within 800 units, median gap 0 to 37 units, a worst of about 220 units, over 25 s; with the ambient gate on
the client the only characters near it are proxies. Window frames of the client show orcs near its player in
different poses 600 ms apart. Tests: `enemy_test.exe` (29 checks of the messages), `net_test.exe` (the second
channel). Not checked: that a proxy's animation matches its host enemy's (only that poses change), what a proxy does
when its host enemy dies (health 0 and a despawn request: not looked at), captains and other types' locomotion
state names, proxy spawns far from the client's player (spawn failed once: "created nothing").

A fix made on the way: the net thread holds `g_net_lock` while it waits for the simulation thread to run a spawn it
asked for, and the simulation thread also took that lock each frame: the stand-in's spawn timed out every time.
The simulation thread now never waits for it.
## Step 2 (the client's hits) built and tried live (session bm)

A stub on the game's hit function (`2C70C0`) calls a filter: a hit on the stand-in is dropped as before; a hit on a
proxy is dropped too, and when the attacker is this copy's own player it is reported (the proxy, the attack record id,
the position). The client sends it as a reliable event; the host looks the enemy up by id and calls the game's hit
function with its own player as the attacker (the stand-in as the attacker faulted) and the enemy's health object as the
victim. Live: 7 of 7 hits sent were applied and the proxies' health followed (two kills). `enemy_test` has the message
checks (36), the 56 offline cases still pass (the rewritten hit stub keeps `hit_standin=0 hit_other=1 hit_after=1`).
## Step 3 (damage to the client) built and tried live (session bn)

Enemies still do not choose the client as a target (its stand-in is a bystander for the AI, and an AI-off proxy's
attack record does no damage by itself). What reaches the client is the swings that overlap the stand-in on the host: the
host stops skipping the stand-in in area attacks, the hit filter reports a hit on the stand-in (attacker: the enemy,
the record, the position) and drops it, the host sends it as a reliable DAMAGE message and the client hits its own
player with the enemy's proxy as the attacker (its follower mark off for the call). Live: a host enemy made to swing at
the stand-in cost B 136 health per swing; 3 sent, 3 applied. Alive/dead stays the game's own on each copy (the stand-in's
health follows the player state); a host in its death screen pauses the host's world, so the client's proxies stand still.
The remaining gap is that enemies do not hunt the client: a client that keeps away from the host's fights is not attacked.
## Order (each step has a test that runs without a person)

0. **Experiments, small and read-only or reversible**, because three pieces are unknown:
   a. how to keep the client's own enemies away (a spawn switch, or removing them each frame);
   b. whether a spawned enemy type can be driven by the stream like the Talion stand-in (its own graph and inputs
      instead of the player's);
   c. what makes a player's hit count and whether `2C70C0` can be called on a host enemy for the stand-in;
   d. what makes enemy AI choose the stand-in.
1. **Proxies, view only:** the client sees the host's fighting, nothing else changes. Bot test: enemies fight each
   other on the host, the client's proxies follow within a measured gap.
2. **Client to host hits:** the client kills a plain orc; the host's orc dies.
3. **Host to client damage and alive/dead.**
4. **Captains and world events.**

## Cost and fallback

Step 1 is the large unknown (0b). If an enemy type cannot be driven like the stand-in, the fallback is a smaller
promise: both players explore together with the existing stand-ins and each fights the enemies of their own copy,
with the other player's attacks shown but not counted. That works today and is not shared fighting.

## Questions for the player

1. Is the proxy model (host's enemies only, client's own suppressed) the right promise, with the fallback above if
   step 0b fails?
2. First enemy kinds: plain infantry orcs only, leaving captains, beasts and drakes for later?
3. Stop after step 1 (view only) for a look before the client's hits are built?
