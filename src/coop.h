#pragma once

#include <cstdint>
#include <string>

struct CoopConfig {
  // The internet transport (src/net_proto.h, NETWORK-DESIGN.md): coop_role=host|client replaces the
  // loopback pair below. The host listens on net_bind:net_port; the client connects to net_peer.
  std::string net_role;                // "host", "client" or empty (loopback pair)
  std::string net_bind = "127.0.0.1";  // coop_bind: the host's address to listen on (loopback until the guard
                                       // learns a peer rule, step 4)
  unsigned net_port = 0;               // coop_port (host)
  std::string net_peer;                // coop_host=<ipv4>:<port> (client)
  std::string net_password;            // coop_password
  std::string exe_sha_hex;             // the game exe's SHA-256, both sides must match (set by sowguard)
  unsigned state_hz = 30;              // coop_state_hz: player states sent per second over the transport
  // The host's save goes to a joining client, which plays in it (NETWORK-DESIGN.md, "Joining"). Both need the
  // sandbox cloud folder (steam_cloud=sandbox); without it the client does not join and the host offers nothing.
  std::wstring save_dir;               // the sandbox cloud folder: the host offers its newest main save, the client
                                       // is given the host's under the name its game asks for
  std::wstring save_local_dir;         // client: the local copy of the same save is replaced too
  bool join = true;                    // coop_join=off: the client plays its own save (tests, tools)
  unsigned join_timeout_sec = 20;      // coop_join_timeout_sec: no host save by then, the client stops
  bool join_message = true;            // a failed join says so in a message box (off in tests)
  // Stage 2 (STAGE2-DESIGN.md): the host's enemies shown on the client as proxies. coop_enemies=on|off.
  bool enemies = false;
  float enemy_radius = 3500.0f;        // coop_enemy_radius: enemies within this of either player are shared
  bool slowmo_off = true;              // coop_slowmo=on keeps the game's slow motion; by default co-op has none (a role is set)
  uintptr_t slowmo_rva = 0x5CF709;     // the `jne` after slow motion's per-timer test
  uintptr_t slowmo_request_rva = 0x5D08B8;  // the game's slow-motion request function (coop_slowmo=on caps it on a client)
  unsigned slowmo_cap_ms = 4000;       // a slow-motion request on a client is ended after this
  bool nemesis = true;                 // coop_nemesis=off: the host's Nemesis events are not recorded or sent
  bool nemesis_follow = true;          // a client runs the host's Nemesis events and drops its own; coop_nemesis=share only records
  bool captain_look = true;            // coop_captain_look=off: the host's captains are shown as the type's generic orc
  bool member_data = true;             // coop_member_data=off: the host does not send the members the client may lack
  bool audio_keys_fatal = false;      // coop_audio_keys_fatal=on: the game crashes when its audio key queue is full (its default)
  bool decoy_follow_remote = true;    // coop_decoy_follow=remote|standin: the decoy stands at the client's sent place or the stand-in's
  bool dmg_borrow = true;             // coop_dmg_borrow=off: a host swing from an enemy with no proxy here is dropped (counted dmg_stale)
  bool decoy_limbo = true;            // coop_decoy_limbo=off: a removed decoy leaves the proxy set at once (its AI runs again)
  bool retarget_refs = true;          // coop_retarget_refs=off: the retarget swap writes a target that sits in a game reference alone
  bool unlink_fix = true;             // coop_unlink_fix=off: the game's list destructor (rva 4DD18C) is left as it is (it can hang)
  bool ai_param_guard = true;         // coop_ai_param_guard=off: the AI's attack-parameter lookup (rva 11A4C5) is left unguarded
  bool exact_losses = true;           // coop_exact_losses=off: a proxy's loss outside hits goes to the host as stock strikes
  bool counter_heal = false;          // coop_counter_heal=on (experiment): a client's counter gives back that enemy's last swing (within 1.5 s)
  bool follow_travel = true;          // coop_follow_travel=off: a client does not follow the host's long jumps (fast travel)
  int decoy_stall = 0;                 // coop_decoy_stall: enemies near the decoy after it, no swing reaching it this long: the decoy is made again (0: not)
  int stuck_sec = 20;                  // coop_stuck_sec: an enemy after the decoy, near it, without a swing at it this long is let go and poked (0: not)
  float wake_radius = 600.0f;          // coop_wake_radius: idle hostile enemies this near the client's decoy are woken (0: not)
  bool anim_mirror = true;             // coop_anim_mirror=off: the client guesses its proxies' animations from their speed
  uintptr_t action_start_rva = 0x195988;  // the animation state machine's start-action function
  bool local_damage_report = true;     // coop_local_damage=off: damage done to a proxy here by an arrow is not told to the host
  bool proxy_react = true;             // coop_proxy_react=off: the client's strikes on proxies are only reported (no reaction shown)
  bool hunt_retarget = true;           // coop_hunt=off: the host does not point engaged enemies at the nearer player
  bool hunt_strict = false;            // coop_hunt_strict=on: the pointing replaces only the host's player or the decoy as a target
  bool hunt_to_player = false;        // coop_hunt_to_player=on: the hunt may also redirect an enemy to the host's player (crashed the host)
  // coop_hit_by: who the host's game is told made the client's hits. poke (2): the host's own player, then a harmless poke from
  // the decoy so the enemy turns to it; decoy (1): the decoy (its hits did no damage in session cg, so it falls back to poke);
  // player (0): the host's player only (before session cg).
  int hit_by = 2;
  bool standin_hits = false;           // coop_standin_hits=on: the stand-in's copies of the other player's swings hurt too
  bool decoy = true;                   // coop_decoy=on: enemies attack an invisible-ish ally where the stand-in stands
  int proxy_table_max = 200;          // coop_proxy_table_max: a proxy whose AI table holds this many entries is made again (0: never)
  bool decoy_hurt_send = true;        // coop_decoy_hurt_send=off: health the decoy loses outside the hit function is only counted
  float decoy_still = 25.0f;          // coop_decoy_still: the decoy is not moved while within this of its place (0: written every frame)
  float decoy_remake = 1000.0f;        // coop_decoy_remake: a jump of the client's place this long (or 400 up or down) remakes the decoy (0: never)
  float decoy_reanchor = 150.0f;       // coop_decoy_reanchor: the decoy is put by the game's teleport when this far from the last (0: never)
  float decoy_dy = 0.0f;               // coop_decoy_dy: its height above the stand-in
  bool enemy_direct = true;            // proxies are placed by writing their transform (not the game's teleport)
  uintptr_t play_rva = 0x664CA4;       // starts a named graph action
  uintptr_t despawn_rva = 0x1A8BB74;   // "AI told to despawn offscreen"
  uintptr_t faction_event_rva = 0x40C304;  // HandleFactionEventAction
  uintptr_t faction_lookup_rva = 0x18DDCC; // a member reference's FactionMember
  uintptr_t ambush_flag_rva = 0x267D598;  // Faction.Ambush.Enable
  uintptr_t ambient_rva = 0x166DF8;    // the ambient spawn test
  unsigned send_port = 0;    // coop_send=127.0.0.1:<port>: send this copy's player (loopback only)
  unsigned listen_port = 0;  // coop_listen=<port>: receive the other copy's player
  unsigned rate_hz = 60;     // coop_rate_hz: until (or without) sim_hook; then once per frame
  unsigned delay_ms = 50;    // coop_delay_ms: the puppet is drawn this far behind the sender
  bool show = false;         // coop_show=lkp: show the received player with the game's
                             // "last known position" effect (needs sim_hook)
  unsigned show_every_ms = 200;    // at most this often
  float show_min_move = 25.0f;     // and only when the received player moved this far
  bool puppet = false;       // coop_show=puppet: every frame, move the nearest character to
                             // the received player, interpolated (needs sim_hook)
  int follow = 1;            // coop_puppet_move: 1 teleport (every frame), 2 walk (its own
                             // locomotion, teleport only when far behind), 3 both (teleported
                             // every frame while its locomotion is asked to walk or run), 4 anim
                             // (moved only by the sender's animation inputs; teleported when it
                             // drifts more than anim_snap away, or to settle once the sender has
                             // stood still for half a second), 0 none
  bool puppet_ai = true;     // coop_puppet_ai=off: the puppet's behavior tree is skipped
  std::string spawn_type;    // coop_puppet_spawn=<type> (e.g. Talion): spawn the puppet at the
                             // sender's position instead of taking over the nearest character
  float run_speed = 300;     // coop_run_speed: sender speed (units/s) from which the puppet runs
  float teleport_dist = 600; // coop_teleport_dist: farther behind than this, it is teleported
  float anim_snap = 150;     // coop_anim_snap: in anim mode, teleported when this far off
  bool puppet_inputs = true; // coop_puppet_inputs=off: the puppet does not get the sender's
                             // animation inputs (stick, speed, turning; see simhook.h)
  bool anim_speed = true;    // coop_anim_speed=off: in anim mode the puppet's root motion is
                             // not scaled to the sender's speed (free-run; see simhook.h)
  bool puppet_actions = true;  // coop_puppet_actions=off: the puppet does not get the sender's
                               // buttons (game actions; with them a Talion stand-in free-runs)
  bool puppet_follower = true;  // coop_puppet_follower=off: the puppet is not marked as this
                                // copy's player's follower (then it puts the player in combat)
  uintptr_t move_request_rva = 0x3F4BD8;
  uintptr_t brain_site_rva = 0x2F7B40;
  uintptr_t tree_rva = 0x1919C8;
  uintptr_t gameclient_rva = 0x26FFA98;
  uintptr_t lkp_rva = 0x19C0380;
  uintptr_t teleport_rva = 0x19B7AC4;
  uintptr_t anim_var_rva = 0x176D78;
  uintptr_t mover_step_rva = 0x191868;
  uintptr_t follower_rva = 0x1156AF0;  // the player-follower getter (bytes checked)
  bool puppet_targetable = false;  // coop_puppet_targetable=on: attacks may pick and hit the
                                   // puppet (this copy's player's strikes kill it)
  uintptr_t target_check_rva = 0x18626C;  // the attack target check (bytes checked)
  uintptr_t hit_rva = 0x2C70C0;            // where every hit is applied (bytes checked)
  uintptr_t aoe_rva = 0x2E5718;            // the area-attack filter (bytes checked)
  uintptr_t anim_reset_rva = 0xFA7E10;     // a full teleport's animation reset (bytes checked)
  bool air_follow = true;  // coop_air_follow=off: in anim mode the puppet is not moved to the
                           // sender while the sender is off the ground (jumps, falls, climbs)
  bool puppet_attacks = true;  // coop_puppet_attacks=off: the puppet does not play the sender's attacks
  uintptr_t attack_thunk_rva = 0x2D6B3C;   // starts an attack (bytes checked)
  uintptr_t attack_stop_rva = 0x2EA790;    // stops one (bytes checked)
  uintptr_t attack_table_rva = 0x27008F8;  // holds the game's attack table
};

// Starts the sender and/or receiver threads (called from sowguard's Init).
void StartCoop(const CoopConfig& cfg);

// Probe "enemies": what the enemy sharing is doing (host: how many it announces; client: proxies and spawns).
std::string CoopEnemyStatus();
// The last 32 damage reports: the host's swings that reached the decoy (attacker, attack, the attacker's distance from the
// hit) and the client's handling of the host's damage messages (applied, failed, no proxy).
std::string CoopDamageLog();
// Test (client): queue a host damage message for enemy `id` at this player's place (probe injectdmg).
std::string CoopTestInjectDamage(uint32_t id, uint32_t attack_id);

// Probe "damage abs:<proxy> <attack id hex>": a swing of that proxy hits this copy's player (the client's damage path).
std::string CoopTestDamage(uintptr_t proxy, uint32_t attack_id);
// Experiment: kills a character through the game's hit function (on the simulation thread).
std::string CoopTestKill(uintptr_t who, uintptr_t by = 0);
// Experiment: changes a co-op setting while the game runs (decoy on|off, hunt on|off, wake <radius>).
std::string CoopSet(const std::string& key, const std::string& value);
// Probe "standinhit abs:<enemy> <attack id hex>" (host): that enemy swings at the stand-in of the client.
std::string CoopTestStandinHit(uintptr_t enemy, uint32_t attack_id);

// Probe "remote": what the receiver has, and how far it is from this copy's player.
std::string CoopStatus();

// Probe "puppet follow teleport|walk|both|none".
void CoopSetFollow(int follow);

// Probe "puppet inputs on|off": whether the puppet gets the sender's animation inputs.
void CoopSetInputs(bool on);

// Probe "puppet snap <units>": anim mode's put-back distance.
void CoopSetAnimSnap(float units);

// Probe "puppet speed on|off": whether anim mode scales the puppet's root motion to the
// sender's speed.
void CoopSetSpeed(bool on);

// Probe "puppet actions on|off": whether the puppet gets the sender's buttons (game actions).
void CoopSetActions(bool on);

// Probe "puppet follower on|off": whether the puppet is marked as this copy's player's
// follower (off puts its byte back to 0).
void CoopSetFollower(bool on);

// Probe "puppet targetable on|off": whether attacks may pick and hit the puppet.
void CoopSetTargetable(bool on);

// Probe "puppet air on|off": whether anim mode moves the puppet to the sender in the air.
void CoopSetAirFollow(bool on);

// Probe "puppet attacks on|off": whether the puppet plays the sender's attacks.
void CoopSetAttacks(bool on);

// Probe "puppet reset on|off": whether the puppet's put-backs restart its animation (the game's
// way, which made the sheathe loop) or keep it (the default).
void CoopSetPutBackResets(bool on);

