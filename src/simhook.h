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
// The ambient spawner's per-candidate test: with the gate on every candidate is rejected (probe "ambient").
// ---- enemies (STAGE2-DESIGN.md). All of these but the lists run on the simulation thread (the frame callback).
struct SimEnemyInfo {
  float pos[3], rot[4];
  float hp, max_hp;
  uint32_t attack_id, attack_stamp;
  char type[32];  // the character's spawn type ("Orc_Infantry"), what SimSpawn takes
  // A captain: the Nemesis member the character was made from (member_id 0xFFFFFFFF: not one). The faction is the name
  // hash of the faction record the member's id is meant in.
  uint32_t member_id, faction_hash;
  char member_name[40];
};
// The member (FactionMember object) with this id in the faction whose record has this name hash, 0 if this copy has none;
// name (40 bytes) gets his name. May scan the heap for the faction objects once in a while (seconds): simulation thread only.
uintptr_t SimFindMember(uint32_t faction_hash, uint32_t id, char name[40]);
// A member as the game's own text (what FactionMember.Save writes), and a member made from such text and put into slot id of
// its faction's member table (replacing the member there). Simulation thread only (SimCall).
// A member's random seed (+0x168): what tells two members of one slot apart. 0 if unreadable.
uint64_t SimMemberSeed(uintptr_t member);
// The character a member points to (member+0x48, what the game's presentations aim the camera at) and where it stands.
// False if the member is not here; character 0 if it points at none.
bool SimMemberCharacter(uint32_t faction_hash, uint32_t id, uintptr_t* member, uintptr_t* character, float pos[3]);
// Makes a member's character (member+0x48) the given one, or, with character 0 (unlink), clears it if it is `only`. The
// game's presentations (a captain's speech) aim the camera at the member's character; a proxy built from a member is not
// linked by the spawn, so the client links it.
void SimMemberLink(uint32_t faction_hash, uint32_t id, uintptr_t character, uintptr_t only);
// Slow motion on the client (coop_slowmo=on) must not outlast the host's: the game's slow-motion requests (a captain's identify
// pause, a finisher) that have been active for more than max_ms are ended through the request function (request_rva, bytes
// checked; the manager is the global at RVA 2A86888). Simulation thread, every frame.
void SimSlowMoCap(uintptr_t request_rva, unsigned max_ms);
// The graph actions the game starts on any character (the animation state machine's: "WalkForward", an attack's animation): a stub
// at the start-action function 195988 (bytes checked) records each start's graph and action definition in a ring on the animation
// threads. Drain (simulation thread) gives, for each start since the last call, the character it is for and the action's name.
// Not usable together with the probe's graphwatch (they hook the same function).
bool SimActionTapInstall(uintptr_t fn_rva, std::string* err);
struct SimActionStart {
  uintptr_t character;
  char name[32];
};
size_t SimActionTapDrain(SimActionStart* out, size_t max);
bool SimMemberExport(uintptr_t member, std::string* text, std::string* err);
bool SimMemberImport(uint32_t faction_hash, uint32_t id, const std::string& text, std::string* err);
// Runs fn on the simulation thread at its next frame and waits (false: it had not run in time; fn then still runs later,
// so it must not refer to the caller's stack).
bool SimCall(const std::function<void()>& fn, unsigned timeout_ms);
// True while a spawn another thread asked for waits for the simulation thread (a proxy spawn must not take its place).
bool SimSpawnBusy();
// Starts noting every AI character whose behavior tree runs (installs the behavior-tree filter if need be).
bool SimEnemyRegistryStart(uintptr_t site_rva, uintptr_t tree_rva, std::string* err);
size_t SimEnemyList(uintptr_t* out, size_t max);  // the characters whose tree ran in the last 400 ms
bool SimEnemyRead(uintptr_t owner, SimEnemyInfo* out);
// Proxies: characters the client spawned to show a host's enemy. Their behavior tree is skipped and they are
// marked as the player's followers (out of the player's combat).
// follower: also mark it as the player's follower (an ally: out of the player's combat). A captain's proxy is not marked: the
// player must be able to hit him, and the game shows an ally's level in blue.
bool SimProxyAdopt(uintptr_t owner, bool follower = true);
// A proxy made from a Nemesis member (SimSpawn's member) is let go of it: the character's member id becomes -1, so that what
// happens to the character (its death) is an ordinary character's and does not touch the member.
void SimProxyDetachMember(uintptr_t owner);
void SimProxyDrop(uintptr_t owner);
bool SimProxyValid(uintptr_t owner);
// The characters taken as proxies now (owner, its health object) - for the probe.
// The character's current and maximum health; false if it has no health object.
bool SimHealthNow(uintptr_t owner, float* cur, float* max);
// The id of an attack record by its name (the game's hash); 0 if the hash function is not the known one.
uint32_t SimAttackIdByName(const char* name);
size_t SimProxyOwners(uintptr_t* owners, uintptr_t* healths, size_t max);
bool SimProxyMove(uintptr_t teleport_rva, uintptr_t owner, const float pos[3], const float rot[4], bool direct);
bool SimProxyPlay(uintptr_t fn_rva, uintptr_t owner, const char* name);
// Starts attack record `id` on the proxy; returns the record (0: none or failed) so it can be stopped later.
// at_player: the attack is started with this copy's own player as its target (the way the AI's own start does), so that the
// game shows what it shows for an attack aimed at the player (the counter prompt); it does no damage by itself.
uintptr_t SimProxyAttack(uintptr_t thunk_rva, uintptr_t stop_rva, uintptr_t table_rva, uintptr_t owner, uint32_t id, bool at_player = false,
                         uintptr_t gameclient_rva = 0);
long SimProxyMovesOk();      // attack moves played on proxies' controllers
long SimProxyMovesFailed();
void SimProxyStopAttack(uintptr_t owner, uintptr_t record);
void SimProxyHealth(uintptr_t owner, float frac);
// Tells an AI character to despawn once it is offscreen, now (the caller is the simulation thread).
bool SimDespawnNow(uintptr_t fn_rva, uintptr_t character);
void SimDespawnOnce(bool on);  // a character is asked to go once (default); off: every time again (the old way)
void SimDespawnStats(long* calls, long* repeats, long* skipped, long* faults, long* repeat_faults, long* refused);
// The game's reference-list destructor (rva 4DD18C) unlinks through a repair (listfix.h): a reference linked with no target is
// unlinked instead of looping for ever (the client's hang). Call on the simulation thread.
bool SimUnlinkFixInstall(std::string* err);
// Session cm: the call at 11A4C5 goes through a guard that answers a non-AI attacker (the host crashed at 11AA3A) as no target.
bool SimAiParamGuardInstall(std::string* err);
void SimAiParamStats(long* installed, long* guarded);
// Session cm: the call at 4990D8 goes through a guard that skips it for a component whose owner has no AI component (190F7C).
bool SimAiTickGuardInstall(std::string* err);
void SimAiTickStats(long* installed, long* skipped);
// The retarget swap moves a target that sits in a game reference with the game's assign (on by default; off: the qword alone).
void SimRetargetRefs(bool on);
void SimRetargetRefStats(long* refs, long* raw);
// How SimMemberLink set a member's character: through the game's reference assign, or written alone (not a reference).
void SimMemberLinkStats(long* assigned, long* raw);
void SimUnlinkStats(long* installed, long* repaired, long* left, long* faults);

// Asks the game to despawn an AI character once it is offscreen ("AI told to despawn offscreen").
bool SimDespawnOffscreen(uintptr_t fn_rva, uintptr_t character, std::string* err);
bool SimAmbientHookInstall(uintptr_t fn_rva, std::string* err);
void SimAmbientBlock(bool on);
// The audio manager's crash on a full future-key queue is turned off (its convar GameAudioManager.FatalErrorOnFutureKeyFull).
bool SimAudioKeysNonFatal(std::string* err);
bool SimAmbushEnable(uintptr_t flag_rva, bool on, std::string* err);
std::string SimAmbientReport();

// Nemesis events (HandleFactionEventAction): a stub on the handler's entry records every event the game applies
// (the two member references, the event record's name, the members' names, the raw action) and, in drop mode,
// does not let it run. SimFactionTake hands the recorded ones out in order.
struct SimFactionEvent {
  uint8_t subj[16], other[16];  // portable references: {faction record hash u32, 0, member id u32, extra u32}
  uint32_t event_hash;          // the event record's name hash
  uint32_t region_hash;         // the region (faction) object the handler ran on: its record's name hash
  uint8_t flags[2];
  char event[48], subj_name[40], other_name[40];
  uint8_t raw[0x60];
};
bool SimFactionHookInstall(uintptr_t handler_rva, uintptr_t lookup_rva, std::string* err);
// The name of the member a reference (16 bytes, as the game holds it) stands for in THIS copy (false: none).
bool SimFactionResolve(const uint8_t ref[16], char name[40]);
// The client: runs a host's event on this copy's Nemesis world (the handler called by hand, the records found again by
// hash); false with err when a record or the manager is not known here.
// The client: finds its region objects (once; a scan of the heap), so a replay does not wait for one of its own events.
void SimFactionPrime();
bool SimFactionReplay(uintptr_t handler_rva, const SimFactionEvent& e, std::string* err);
void SimFactionDrop(bool on);  // on: the game's own events do not run (the client takes the host's)
size_t SimFactionTake(SimFactionEvent* out, size_t max);
std::string SimFactionReport();

// Slow motion off (see simhook.cpp): the per-timer test's `jne` becomes two nops. A start (the SlowMO.Start console
// program's call, run on the simulation thread) and a report of the five timers' rates are for tests.
bool SimSlowMoOff(uintptr_t jne_rva, std::string* err);
bool SimSlowMoStart(uintptr_t fn_rva, const std::string& record, std::string* err);
std::string SimSlowMoReport(uintptr_t jne_rva);

bool SimAnimResetHookInstall(uintptr_t reset_rva, std::string* err);
void SimKeepPuppetAnimOnTeleport(bool on);  // each frame; false: the puppet's resets run
long SimAnimResetsSkipped();
long SimAoeSkips();
void SimKeepPuppetUntargeted(bool on);
// Hits on proxies (the host's enemies shown here): dropped, and those of this copy's own player reported (the
// owner is the proxy character), so the host can apply them. SimHitWatch needs SimAttackHooksInstall.
struct SimHitReport {
  uintptr_t owner;
  uint32_t attack_id;
  float pos[3];
};
void SimHitWatch(uintptr_t gameclient_rva, bool on);
// A hit of this copy's player on a proxy is reported (SimHitReports) and also let through, so the game plays its reaction (a hit, a
// counter, a finisher) on the proxy; the host's health is written over the damage every frame.
void SimProxyReact(bool on);
size_t SimHitReports(SimHitReport* out, size_t max);
long SimProxyHits();
// Redirecting an enemy's target (the character it attacks) to another character, kept up every frame: STAGE2-DESIGN.md.
uintptr_t SimEnemyTarget(uintptr_t enemy);              // the character an AI character is after (0: none)
// This copy's own player character (0 if none).
uintptr_t SimLocalPlayer(uintptr_t gameclient_rva);
bool SimRetarget(uintptr_t enemy, uintptr_t target);    // target 0: stop
void SimApplyRetargets();                               // once a frame, on the simulation thread
long SimRetargeted();
// The only current targets a redirect replaces (the host's player and the decoy; 0, 0: any, as before session cg); an enemy that
// took another target is left to it (counted).
void SimRetargetFrom(uintptr_t a, uintptr_t b);
void SimRetargetDropTarget(uintptr_t target);  // every redirect to target ended (the decoy removed)
long SimRetargetLeft();
long SimRetargetSkipped();  // redirects not made: the enemy dead, or its target not a character
// Redirect slots in use (of 32), and of those, held by an enemy whose health is 0 or unreadable (session cm: the hunt never
// let a dead enemy's slot go while its AI still named the decoy).
int SimRetargetSlotsUsed(int* dead);
// The host: swings of enemies that reach the stand-in are reported (and not taken) instead of skipped, so the client can
// take them (SimDamageReports; the owner is the attacking enemy). Call before SimKeepPuppetUntargeted runs.
void SimStandinHurt(bool on);
// The decoy: an allied character the enemies attack where the stand-in is; hits on it are reported like the stand-in's.
void SimSetDecoy(uintptr_t owner);
// Diagnostic: the next n hits that reach the game's hit function are logged (types, health, roles of both characters).
void SimHitLog(long n);
// The combat manager's presentation flag (byte +0x1810 of [exe+26FFB70], written by F0F44: on during the death cinematic and a
// captain's or the killer's presentation, off on the death screens and in play; session cm, tools/presentation_flag.py).
bool SimPresentationActive();
// Hits of the host's allies (bystander kinds: tamed wargs, followers) on the stand-in or the decoy are not sent to the client
// (on by default, session cm); the count of those dropped.
void SimDropAllyHits(bool on);
long SimAllyHitDrops();
void SimHideCharacter(uintptr_t owner);
size_t SimDamageReports(SimHitReport* out, size_t max);
// Is this attack one of the player's counter strikes (its name holds _Cntr_; the table is read once)?
bool SimIsCounterAttack(uintptr_t table_rva, uint32_t attack_id);
// Second reports of one swing that reached both the stand-in and the decoy, not reported (session cm).
long SimDamageDups();
bool SimHitStandin(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t attacker_owner, uint32_t attack_id);
// The client: applies such a swing to this copy's own player; the attacker is the enemy's proxy.
bool SimApplyDamage(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t attacker_owner, const float pos[3], uint32_t attack_id,
                    uintptr_t gameclient_rva);
// The host: wakes an idle enemy by a hit from `attacker_owner` (the client's decoy): the game's hit function with the attack record
// named `attack_name` (hash found by the game's own name hash, 423E70), then the enemy's health is put back to what it was: the
// enemy keeps the decoy as the one that hit it (its target), at no cost. False if anything is not as known.
// An attack's id from its name (the game's name hash 423E70), 0 if the hash function is not this build's.
uint32_t SimAttackId(const char* attack_name);
bool SimPokeEnemy(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t enemy_owner, uintptr_t attacker_owner, const char* attack_name);
// Kills a character the way the game does: its health put at 1, then a hit from this copy's own player with the attack record
// `attack_name` through the game's hit function, so that the game's own death runs (the animation, the ragdoll, the removal of the
// body later), not a health of 0 that the game never notices. The character is let go as a proxy first (its hits are otherwise
// dropped). False if the call could not be made.
// `by`: the attacking character (0: this copy's player; the player's hit does not hurt an ally such as the host's decoy).
bool SimKillCharacter(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t owner, uintptr_t gameclient_rva, const char* attack_name,
                      uintptr_t by = 0);
// True if the character is marked as the player's follower (an ally).
bool SimIsFollower(uintptr_t owner);
// The host: applies a client's hit to the enemy `victim_owner`; the attacker is `attacker_owner` (the decoy), or this copy's own
// player when that is 0 or not a character.
bool SimApplyHit(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t victim_owner, const float pos[3], uint32_t attack_id,
                 uintptr_t gameclient_rva, uintptr_t attacker_owner = 0);
// The stand-in's own swings (it plays the other player's attacks) are dropped in the hit function when on: what the other player hits
// is told by that player, and the stand-in's copy of the swing hurt the host's enemies a second time (session cg).
void SimDropStandinHits(bool on);
long SimStandinHitDrops();

long SimTargetRejects();
long SimHitDrops();
// A puppet the game unloaded, freed or killed is dropped (logged), so a new one is picked or
// spawned. True when it dropped one.
bool SimForgetGonePuppet();
// The stand-in (0: none), and letting it go so that a new one is made (returns the old one, now hidden, health 0, tree skipped).
uintptr_t SimPuppetOwner();
uintptr_t SimRetirePuppet();

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
std::string SimPuppetAttack(uintptr_t thunk_rva, uintptr_t table_rva, uint32_t id, uintptr_t gameclient_rva = 0,
                            bool at_player = false);
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
// member: a FactionMember (SimFindMember): the character is made from that Nemesis member, the way the game spawns a captain
// (his name, look and level come from the member; the spawn function takes the member as its eighth argument).
bool SimSpawn(const SimSpawnRvas& r, const char* record, const float* pos, const char* look, std::string* report,
              uintptr_t* spawned = nullptr, bool run_now = false,  // run_now: the caller is the simulation thread
              uintptr_t member = 0);

// coop_puppet_spawn: spawn a character of type at pos, give it this copy's player's behavior
// resource (so a "Talion", a cutscene actor, animates with the player's graph) and make it
// the puppet. From any thread but the simulation thread (it waits for the spawn).
bool SimSpawnPuppet(const SimSpawnRvas& r, const char* type, const float pos[3], std::string* report, bool run_now = false);

// Probe "watch": what the write watch armed by "spawn <type> x y z watch" caught (the
// writes to the new character's +0x19A8, with their call stacks).
std::string SimWatchReport();
std::string SimWatchWrites(const std::vector<uintptr_t>& addrs, int len);  // probe "watchw": up to 4 addresses, every thread

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
