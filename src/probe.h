#pragma once

#include <cstdint>
#include <string>

struct ProbeConfig {
  std::wstring profile;       // <profile>\sowprobe.txt gets the pipe name
  std::string ready_pattern;  // byte pattern that appears once the game has unpacked
  unsigned wait_sec = 300;    // give up waiting for it after this long
  bool write = false;         // probe_write=1: allows the write command and hooks
  bool hook_debug_print = false;
  std::string debug_print_pattern;  // a call site of the debug print; target = rip(1, 5)
  std::string debug_channels;       // channel flag RVAs (hex) set to 1 once the hook is in
  uintptr_t gameclient_rva = 0x26FFA98;    // the GameClient pointer (Steam build 3095001)
  uintptr_t save_handler_rva = 0x19C0984;  // the Player.Save console handler
  uintptr_t lkp_rva = 0x19C0380;           // shows the "last known position" effect
  uintptr_t move_request_rva = 0x3F4BD8;   // a character's move request (puppet walk)
  uintptr_t brain_site_rva = 0x2F7B40;     // the AI tick's behavior-tree call (puppet ai)
  uintptr_t tree_rva = 0x1919C8;           // the behavior tree's update
  uintptr_t teleport_rva = 0x19B7AC4;      // the game's queued teleport (probe "teleport")
  uintptr_t anim_var_rva = 0x176D78;       // sets a named float animation variable ("puppet mirror")
  uintptr_t attack_thunk_rva = 0x2D6B3C;   // starts an attack ("puppet attack"; bytes checked)
  uintptr_t attack_table_rva = 0x27008F8;  // holds the game's attack table
};

// Starts the probe threads (called from sowguard's Init when probe=1).
void StartProbe(const ProbeConfig& cfg);

// sowguard's log, shared with the probe.
void SowLog(const wchar_t* fmt, ...);
