#pragma once

#include <cstdint>
#include <string>

struct InputConfig {
  unsigned first_ms = 20000;        // first A press this long after the game window appears
  unsigned every_ms = 8000;         // then one press every this many ms
  unsigned max_presses = 6;         // give up after this many
  unsigned hold_ms = 200;           // how long each press holds the button
  std::wstring window_class = L"Shadow of War";
  std::string iat_section = ".xtext";  // where SteamStub rebuilds the game's import table
  unsigned iat_scan_bytes = 0x10000;   // how much of that section to search
  bool background_run = false;         // keep running unfocused; no cursor grabbing then
  uintptr_t sim_vtable_rva = 0;        // sim_hook=<vtable rva>:<slot> (0: no hook)
  unsigned sim_slot = 0;
};

// Starts the virtual-pad thread (called from sowguard's Init when autoload=1).
void StartInput(const InputConfig& cfg);

// Called by the probe's debug-print sink for every game log line; ends the pressing
// once the game reports the world starting ("[QUESTMGR]: GameModeSet - ").
void InputOnDebugLine(const char* category, const char* msg);

// Probe "pad": hold these buttons and stick values on the virtual pad for ms.
bool InputHold(unsigned buttons, int lx, int ly, int rx, int ry, unsigned ms, std::string* err);

// Probe "save": once the world has started, the game's next pad read runs its
// Player.Save console handler. handler_rva is checked against the handler's known bytes.
bool InputRequestSave(uintptr_t handler_rva, uintptr_t gameclient_rva, std::string* err);

// The thread that logged the world start (the simulation thread); 0 until then.
unsigned long InputGameThread();
