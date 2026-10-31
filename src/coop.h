#pragma once

#include <cstdint>
#include <string>

struct CoopConfig {
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

