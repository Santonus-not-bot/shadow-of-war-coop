#pragma once

#include <string>

struct SteamGuardConfig {
  bool trace_reads = false;  // steam_trace=1: remote-storage reads still reach Steam but are logged
  std::wstring cloud_dir;    // steam_cloud=sandbox: the game's cloud files live here instead of Steam
};

// Starts the thread that blocks ISteamRemoteStorage and ISteamUserStats writes once the
// game has initialised Steam (see steamguard.cpp). Called from sowguard's Init.
void StartSteamGuard(const SteamGuardConfig& cfg);

// sowguard's fail-closed exit (logs, then terminates the process with `code`).
[[noreturn]] void SowFatal(unsigned long code, const wchar_t* fmt, ...);

constexpr unsigned long kExitSteamGuard = 0xC0DE0006;
