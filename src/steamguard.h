#pragma once

#include <string>

struct SteamGuardConfig {
  bool trace_reads = false;  // steam_trace=1: remote-storage reads still reach Steam but are logged
  std::wstring cloud_dir;    // steam_cloud=sandbox: the game's cloud files live here instead of Steam
};

// Called once, on the game's first look at its save in the sandbox cloud, with the file name it asks for. A
// joining client uses it to have the host's save in place by then; it may block, or end the process. Set it
// before the game gets that far (the coop code does, once its config is read).
void SteamSetBeforeSaveRead(void (*fn)(const char* name));

// The game's main save in the cloud: "<hex>_ShadowOfWar.sav" (the numbered slots are local files).
inline bool SowIsSaveName(const char* name) {
  if (!name) return false;
  const char* base = name;
  for (const char* p = name; *p; ++p)
    if (*p == '\\' || *p == '/') base = p + 1;
  const char* suffix = "_ShadowOfWar.sav";
  size_t n = 0;
  while (base[n] && base[n] != '_') {
    const char c = base[n];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    ++n;
  }
  if (n == 0) return false;
  for (size_t i = 0; suffix[i]; ++i)
    if (base[n + i] != suffix[i]) return false;
  return base[n + 16] == 0;
}

// Writes a file into the sandbox cloud folder, the way the game's own cloud write does (temporary file, then
// swap). False when there is no sandbox cloud or the name is not one a cloud file may have.
bool SteamCloudPlace(const char* name, const void* data, size_t size);

// Starts the thread that blocks ISteamRemoteStorage and ISteamUserStats writes once the
// game has initialised Steam (see steamguard.cpp). Called from sowguard's Init.
void StartSteamGuard(const SteamGuardConfig& cfg);

// sowguard's fail-closed exit (logs, then terminates the process with `code`).
[[noreturn]] void SowFatal(unsigned long code, const wchar_t* fmt, ...);

constexpr unsigned long kExitSteamGuard = 0xC0DE0006;
constexpr unsigned long kExitCoopJoin = 0xC0DE0007;  // a joining client could not get the host's save
