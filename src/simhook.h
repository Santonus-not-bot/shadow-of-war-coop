#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Game calls that allocate need the simulation thread: other threads have no engine
// context (1309FC returns null there; seen 2026-10-21 as a crash at 315D5D when the
// "last known position" effect was created from the pad thread). This module runs such
// calls from a pre-hook on a vtable slot the simulation thread calls every frame.

// Probe "count": replace n slots of the vtable at vtable_rva (from first) with counting
// stubs for ms, restore them, and report calls per slot and the calling thread.
std::string SimCountSlots(uintptr_t vtable_rva, unsigned first, unsigned n, unsigned ms);

// sim_hook=<vtable rva>:<slot>: a register-preserving pre-hook on that slot runs queued
// work when the simulation thread calls it.
bool SimHookInstall(uintptr_t vtable_rva, unsigned slot, std::string* err);
bool SimHookInstalled();

// A function the simulation thread calls once per frame (from the pre-hook, before the
// queued work), once the world has started. It must be quick. nullptr: none.
void SimSetFrameCallback(void (*callback)());

// Queue the game's "last known position" effect at pos (probe "lkp", coop_show=lkp).
// lkp_rva is checked against the function's known bytes first.
bool SimRequestLkp(uintptr_t lkp_rva, uintptr_t gameclient_rva, const float pos[3], std::string* err);

// How many effects the simulation thread has shown so far.
unsigned SimLkpShown();

// Puppet: an existing character the simulation thread teleports to the remote player
// (coop_show=puppet). Characters are found through their health objects (the class of
// [player+0x26A8]): health object +0x28 is the owner, owner +0x26A8 points back.

// Heap qwords equal to value (private read-write memory, resident pages only; this
// thread's stack skipped), at most room. Never on the simulation thread (seconds).
size_t SimScanHeap(uint64_t value, uintptr_t* out, size_t room, size_t* resident_bytes);

// Choose the nearest living character other than the player (scans the heap, ~5 s; never
// call it on the simulation thread). Returns a one-line report.
// make_puppet false: only remember it as the source of the spawn-type table (spawn).
bool SimPickNearestPuppet(uintptr_t gameclient_rva, std::string* report, bool make_puppet = true);

// Choose this character (its owner object), or none with 0.
bool SimSetPuppet(uintptr_t owner, std::string* err);

// Queue a teleport of the puppet to pos/rot (quaternion x, y, z, w) through the game's
// own queued teleport (19B7AC4, bytes checked).
// direct: the transform is written in place instead (the teleport puts a character on the
// ground below the point; in place it can be in the air).
bool SimRequestPuppetMove(uintptr_t teleport_rva, const float pos[3], const float rot[4], std::string* err,
                          bool direct = false);

// "puppet abs:<owner> moves=<n>" or "puppet none".
std::string SimPuppetStatus();
bool SimHasPuppet();

// Code pages within rel32 reach of the whole game image, taken at start-up (sowguard's
// Init): later the game's own heap fills the range around the image. 0: probe "stacks",
// 1: the behavior-tree filter.
void SimReserveNearPages();
uint8_t* SimNearPage(unsigned i);

// Puppet AI off: the AI tick runs each character's behavior tree through one call
// (`mov rcx,[r8]; xor edx,edx; call <tree>` at site_rva, bytes checked). That call is sent
// through a filter that leaves out the puppet's tree while its AI is off. The patch is made
// on the simulation thread at its next frame.
bool SimRequestBrainHook(uintptr_t site_rva, uintptr_t tree_rva, std::string* err);
void SimSetPuppetAi(bool on);

// Walk the puppet toward dest with its own locomotion, through the game's move request
// (request_rva, bytes checked): type 1 walks, 2 runs, 0 stops it (the controller's idle
// mode). scale is the request's speed factor (1 in the game's own requests). On the
// simulation thread it happens at once, otherwise at the next frame.
bool SimPuppetWalk(uintptr_t request_rva, const float dest[3], int type, float scale, std::string* err);
bool SimPuppetPos(float pos[3]);
bool SimPuppetXform(float pos[3], float rot[4]);  // position and rotation quaternion

// Animation inputs: the named float variables the player's update feeds its animation graph
// every frame (stick, speed, turning, camera; names in simhook.cpp). Install records this
// copy's player's values with an entry hook on the setter (176D78, bytes checked); the puppet
// gets values through the same setter (simulation thread only).
constexpr int kAnimInputs = 25;
bool SimAnimInputsInstall(uintptr_t setter_rva, uintptr_t gameclient_rva, std::string* err);
bool SimAnimInputs(float out[kAnimInputs], uint32_t* mask);  // false: not recording
// The puppet gets these values now (simulation thread; the setter at setter_rva is
// byte-checked on first use when this copy does not record). False: nothing applied.
bool SimApplyPuppetInputs(uintptr_t setter_rva, const float in[kAnimInputs], uint32_t mask, std::string* err);
void SimSetMirror(bool on);  // probe "puppet mirror": the puppet gets this copy's own inputs
std::string SimInputsReport();  // probe "inputs"

// Root-motion speed (simulation thread, once per frame): the puppet's own root motion is
// scaled toward `speed` (the sender's, units/s; smoothed) plus `extra` (not smoothed), so a
// stand-in whose graph only runs keeps up with a sender who free-runs. Its movement component
// gets a copy of its vtable with the per-frame step (slot 0x80, at step_rva, bytes checked)
// wrapped. speed < 0: its own motion.
void SimSetPuppetRootSpeed(uintptr_t step_rva, float speed, float extra = 0.0f);
// While skip is true the puppet's movement step (the same wrapped slot) does not run, so a
// position given by a teleport is not pulled back onto the ground. False if it cannot be.
bool SimSkipPuppetStep(uintptr_t step_rva, bool skip);
long SimSkippedSteps();
std::string SimRootSpeedReport();  // for probe "remote"

// Game actions: the player's buttons and sticks as its animation graph reads them (377
// actions: held byte, pressed byte, value; see simhook.cpp). A stand-in with the player's graph
// needs them to leave its walk for the free-run (A button), and its graph only reads them with
// two player flags set, which these calls set and clear with the lists.
struct SimAction {
  uint16_t index;
  uint8_t held, pressed;
  float value;
};
constexpr int kMaxActions = 48;
// This copy's player's non-zero actions (last frame); returns how many (0: no player).
int SimPlayerActions(uintptr_t gameclient_rva, SimAction* out, int max);
// The puppet gets these actions now, all others zero (simulation thread). False: no puppet.
bool SimApplyPuppetActions(const SimAction* a, int n, std::string* err);
// The puppet's actions and flags back to an AI character's (simulation thread; no-op if none).
void SimReleasePuppetActions();
long SimPuppetActionFrames();
// Probe "puppet hold <ms> <index>[=value]...": experiment, the puppet's lists hold these actions
// (value default 1) for ms, pressed on the first frame, its flags left as they are.
std::string SimPuppetHold(unsigned ms, const int* index, const float* values, int n);

// The puppet as the local player's follower (simulation thread, once per frame): its
// player-follower byte is set, so the local player's combat does not treat it as an enemy
// beside it; with on false, a byte this set goes back to 0 (once). getter_rva: the game's
// getter of that byte (bytes checked once). False: not set (or on false).
bool SimKeepPuppetFollower(uintptr_t getter_rva, bool on, std::string* err);
long SimFollowerMarks();  // times the byte was found 0 and set
// Attacks: stubs on the game's attack target check (check_rva) and on the function that applies
// every hit (hit_rva; bytes checked, installed once) reject the puppet as a candidate target and drop
// hits on it. SimKeepPuppetUntargeted names the puppet every frame (on false or no puppet:
// nothing is skipped). SimTargetRejects / SimHitDrops: candidates rejected, hits dropped.
// A third stub on the area-attack filter (aoe_rva) keeps the puppet out of area ticks (fire,
// explosions); SimAoeSkips counts them.
bool SimAttackHooksInstall(uintptr_t check_rva, uintptr_t hit_rva, uintptr_t aoe_rva, std::string* err);

// A stub on the game's animation reset (reset_rva, bytes checked), which a full teleport runs on
// the character's animation controller (it restarts the graph at IdleL), returns at once for the
// controller SimKeepPuppetAnimOnTeleport(true) last named: the puppet's put-backs move it
// without restarting its animation (they caused the sheathe loop, session bd).
bool SimAnimResetHookInstall(uintptr_t reset_rva, std::string* err);
void SimKeepPuppetAnimOnTeleport(bool on);  // each frame; false: the puppet's resets run
long SimAnimResetsSkipped();
long SimAoeSkips();
void SimKeepPuppetUntargeted(bool on);
long SimTargetRejects();
long SimHitDrops();
// A puppet the game unloaded, freed or killed is dropped (logged), so a new one is picked or
// spawned. True when it dropped one.
bool SimForgetGonePuppet();

// Probe "trace": for ms, count the return addresses of calls to fn (any address; an
// execute breakpoint on the simulation thread) whose rcx is `rcx` (0: all).
// Probe "wtrace" (write_len 1, 2, 4 or 8): count the instructions on the simulation thread
// that write that many bytes at fn (any aligned address), each with its first call stack.
// names: count by the second argument (a variable name) instead of the return address.
// reads (with write_len): reads of the address count too. args: count by the qword at r9+0x28
// (a character, for a function whose fourth argument is one of its components), with the first
// call's caller, rcx, [rcx+0x28] and r9.
// The sender's attacks: SimAttackRecord finds the attack record with this name hash (the same
// in every copy) in the game's attack table (table_rva holds it; read-only); 0 if none.
// SimPuppetAttack (probe "puppet attack <hash>"): the puppet starts that attack, through the
// game's own start (thunk_rva, bytes checked) on the simulation thread.
uintptr_t SimAttackRecord(uintptr_t table_rva, uint32_t id);
// gameclient_rva non-zero: on this copy's own player instead (experiments: "puppet attack <hash> player").
std::string SimPuppetAttack(uintptr_t thunk_rva, uintptr_t table_rva, uint32_t id, uintptr_t gameclient_rva = 0);
// Once per frame (simulation thread): the puppet plays the sender's attacks. id: the attack the
// sender's player is in (its name hash; 0: none), stamp: when it started. A new id or stamp
// starts that attack on the puppet; id 0 stops the puppet's attack (stop_rva, bytes checked) if
// it is still the one started here. False with err when it cannot.
bool SimFollowAttack(uintptr_t thunk_rva, uintptr_t stop_rva, uintptr_t table_rva, uint32_t id, uint32_t stamp,
                     std::string* err);
long SimAttacksStarted();
long SimAttacksStopped();

// Probe "puppet play <action> [flag]": the puppet's animation graph plays that named action through
// the game's play-by-name (fn_rva, bytes checked), on the simulation thread. Experiment.
std::string SimPuppetPlay(uintptr_t fn_rva, const char* name, int flag);
// Probe "puppet move <attack id> [speed] [player]": plays that attack's move on the puppet's (or
// this copy's player's, gameclient_rva non-zero) animation controller, as the combat code does
// for a player (session bd). Experiment.
std::string SimPuppetMove(uintptr_t table_rva, uint32_t id, float speed, uintptr_t gameclient_rva);
// If the puppet's graph sits in the running jump's approach states (its inputs come from the sender's,
// so the sender is jumping), plays RunningJump_Assisted_Wraith on it (the sender's graph takes it; the
// puppet's takes FreeJmp and ends in WallStop), at most once per 1.5 s (session be).
void SimAssistJump(bool sender_in_air);
void SimSetVisMask(unsigned mask);  // probe "puppet vis <hex>" (experiment)
// Probe "puppet assist <action>|off": the action played (default RunningJump_AutoPicker_FreeJmp) or none.
void SimSetAssistJump(const char* action);
// Probe "puppet moves on|off": whether SimFollowAttack also plays each attack's move on the
// puppet's controller (experiment, session bd), and how many it played.
void SimSetAttackMoves(bool on);
long SimAttackMovesPlayed();
// Probe "graphwatch <ms> [player]": the graph actions started on the puppet's (or this copy's
// player's) graph for ms, on any thread, each with its caller (a recording stub on fn_rva, the
// action start, bytes checked; installed on first use). Experiment.
// armed: run once recording has begun (the probe pipe takes one command at a time, so a strike
// sent after the watch command ran only after the window, session bd).
std::string SimGraphWatch(uintptr_t fn_rva, bool player, uintptr_t gameclient_rva, unsigned ms,
                          const std::function<void()>& armed = {});
// Probe "ctrlwatch <ms> [player]": the move requests made on the puppet's (or this copy's
// player's) animation controller for ms, on any thread, each with its caller and the request
// (a recording stub on fn_rva, bytes checked; installed on first use). Experiment.
std::string SimCtrlWatch(uintptr_t fn_rva, bool player, uintptr_t gameclient_rva, unsigned ms,
                         const std::function<void()>& armed = {});

std::string SimTraceCalls(uintptr_t fn, uintptr_t rcx, unsigned ms, unsigned write_len = 0, bool names = false,
                          bool reads = false, bool args = false, bool by_r8 = false);

// Probe "teleport": this copy's player ([[GameClient]+0x888]) to pos, keeping its rotation,
// through the same queued teleport, at the next frame.
bool SimRequestPlayerTeleport(uintptr_t teleport_rva, uintptr_t gameclient_rva, const float pos[3], std::string* err);

// Probe "spawn": a character of a spawnable type (e.g. "AI_Talion"; the table is found
// through the current puppet) at pos (nullptr: the player's position), with the player's
// rotation, created on the simulation thread the way the game's queued spawn requests do.
// Waits up to 2 s; report says what was made.
struct SimSpawnRvas {
  uintptr_t hash_rva = 0x423E70, spawn_rva = 0x1036D0, is_character_rva = 0x274A04, region_rva = 0x168DA8,
            area_rva = 0x607610, set_area_rva = 0x35DC38, setup_rva = 0x10AFB0,
            appearance_site_rva = 0x3FAB76, appearance_lookup_rva = 0x1B08BB0, behavior_rva = 0x459474, world_rva = 0x26FFAB0, gameclient_rva = 0x26FFA98,
            spawn_table_rva = 0x2702440;  // holds the spawn-type table (0: only through an AI character)
};
// look (nullptr: the type's own): an appearance record (type 0xA66, e.g. "PlayerSkin_Talion")
// the new character is built with instead.
bool SimSpawn(const SimSpawnRvas& r, const char* record, const float* pos, const char* look, std::string* report,
              uintptr_t* spawned = nullptr);

// coop_puppet_spawn: spawn a character of type at pos, give it this copy's player's behavior
// resource (so a "Talion", a cutscene actor, animates with the player's graph) and make it
// the puppet. From any thread but the simulation thread (it waits for the spawn).
bool SimSpawnPuppet(const SimSpawnRvas& r, const char* type, const float pos[3], std::string* report);

// Probe "watch": what the write watch armed by "spawn <type> x y z watch" caught (the
// writes to the new character's +0x19A8, with their call stacks).
std::string SimWatchReport();

// Probe "animres": give a character's animation object a behavior resource (459474, bytes
// checked) at the next frame; with both its animation and behavior resources set, the game
// switches the character on.
bool SimSetBehaviorResource(uintptr_t fn_rva, uintptr_t character, uintptr_t res, std::string* err);

// Probe "look": a private copy of appearance record base_name with the 8-byte fields at
// offsets (in its field block) taken from donor_name, e.g. the model (+0x148). The report
// names it as abs:<address>, which spawn takes as its look.
bool SimMakeLook(uintptr_t lookup_rva, const char* base_name, const std::vector<unsigned>& offsets,
                 const char* donor_name, std::string* report);

// "walks=.. idles=.. ai=on|off tree_skips=.. tree_hook=.. mode=<controller mode> pos=x,y,z".
std::string SimPuppetDetail();
