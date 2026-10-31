// steamguard: stops the game's Steam cloud-save and achievement/stat writes inside the
// game process.
//
// The game sends its saves through ISteamRemoteStorage as well as writing them to disk.
// The Steam client process (steam.exe) services those calls and writes into the owner's
// Steam\userdata\<id>\356190\remote, even with Steam Cloud off for the game, so the file
// redirect cannot see it (seen 2026-10-20, see coop-anything NOTES.md). Achievements and
// stats (ISteamUserStats) go the same way, to the account itself. This thread waits until
// the game has initialised Steam, takes the same ISteamRemoteStorage (version 014) and
// ISteamUserStats (version 011) the game uses (flat API) and replaces the write methods
// in their vtables with functions that log the call and pretend it worked. Reads stay
// untouched.
//
// The game also loads its save through the cloud (FileRead at the campaign screen; seen
// 2026-10-21), which Steam answers from the real Steam folder whatever the sandbox holds.
// steam_cloud=sandbox makes this copy's sandbox folder the cloud: the game's cloud reads
// and writes go to <profile>\steam-userdata\356190\remote\ and never reach Steam.
//
// Slot offsets are read from steam_api64's flat wrappers, which are each
// `mov rax,[rcx]; jmp [rax+off]` (a few use r10); anything else stops the game (fail
// closed).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "probe.h"
#include "steamguard.h"

namespace {

constexpr const char* kRemoteStorageVersion = "STEAMREMOTESTORAGE_INTERFACE_VERSION014";
constexpr const char* kUserStatsVersion = "STEAMUSERSTATS_INTERFACE_VERSION011";

std::wstring g_cloud;  // steam_cloud=sandbox: this copy's cloud folder; empty: writes are dropped

volatile LONG g_blocked = 0;
volatile LONG g_cloud_logged = 0;

void NoteCloud(const char* method, const char* name, const char* result) {
  if (InterlockedIncrement(&g_cloud_logged) > 300) return;
  SowLog(L"steam: sandbox cloud %S('%S') -> %S", method, name ? name : "", result);
}

// The sandbox file for a cloud file name ("\x_ShadowOfWar.sav" -> <cloud>\x_ShadowOfWar.sav),
// or "" for a name that would leave the folder (drive, "..", ".").
std::wstring CloudPath(const char* name) {
  if (!name) return L"";
  std::string n(name);
  size_t start = n.find_first_not_of("\\/");
  if (start == std::string::npos || n.find(':') != std::string::npos) return L"";
  n = n.substr(start);
  size_t pos = 0;
  while (pos <= n.size()) {
    size_t end = n.find_first_of("\\/", pos);
    std::string part = n.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (part.empty() || part == "." || part == "..") return L"";
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  wchar_t wide[MAX_PATH];
  int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, n.c_str(), -1, wide, MAX_PATH);
  if (len <= 1) return L"";
  std::wstring rel(wide);
  for (auto& c : rel)
    if (c == L'/') c = L'\\';
  return g_cloud + L"\\" + rel;
}

bool CloudFileInfo(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA* info) {
  return !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, info) &&
         !(info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool CloudWrite(const char* name, const void* data, int32_t size) {
  std::wstring path = CloudPath(name);
  if (path.empty() || size < 0 || (size && !data)) {
    NoteCloud("FileWrite", name, "refused (bad name or size)");
    return false;
  }
  for (size_t i = g_cloud.size() + 1; i < path.size(); ++i)
    if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
  // Write a temporary file and swap it in, so a crash never leaves half a save.
  std::wstring tmp = path + L".sowtmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD written = 0;
  bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, (DWORD)size, &written, nullptr) && written == (DWORD)size;
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!ok) DeleteFileW(tmp.c_str());
  char v[48];
  snprintf(v, sizeof v, ok ? "%d bytes written" : "write failed (%d bytes)", size);
  NoteCloud("FileWrite", name, v);
  return ok;
}
volatile LONG g_stats_blocked = 0;
volatile LONG g_achievements_blocked = 0;

void Note(const char* method, const char* name, long long size) {
  if (InterlockedIncrement(&g_blocked) > 500) return;
  if (size >= 0) SowLog(L"steam: blocked %S('%S', %lld bytes)", method, name ? name : "", size);
  else SowLog(L"steam: blocked %S('%S')", method, name ? name : "");
}

// The game sets all ~140 stats at every load and StoreStats comes often, so stats have
// their own log budget; achievements, resets and leaderboards have another, so a late
// achievement is still logged.
void NoteStat(const char* method, const char* name, const char* value, volatile LONG* budget = &g_stats_blocked,
              LONG max = 300) {
  if (InterlockedIncrement(budget) > max) return;
  if (value) SowLog(L"steam: blocked %S('%S', %S)", method, name ? name : "", value);
  else SowLog(L"steam: blocked %S('%S')", method, name ? name : "");
}

void NoteAchievement(const char* method, const char* name, const char* value) {
  NoteStat(method, name, value, &g_achievements_blocked, 200);
}

// Replacements, same signatures as the interface methods (this in rcx).
bool __fastcall FileWrite(void*, const char* name, const void* data, int32_t size) {
  if (!g_cloud.empty()) return CloudWrite(name, data, size);
  Note("FileWrite", name, size);
  return true;
}
uint64_t __fastcall FileWriteAsync(void*, const char* name, const void*, uint32_t size) {
  Note("FileWriteAsync", name, size);
  return 0;  // k_uAPICallInvalid
}
bool __fastcall FileForget(void*, const char* name) {
  Note("FileForget", name, -1);
  return true;
}
bool __fastcall FileDelete(void*, const char* name) {
  if (!g_cloud.empty()) {
    std::wstring path = CloudPath(name);
    bool ok = !path.empty() && DeleteFileW(path.c_str());
    NoteCloud("FileDelete", name, ok ? "deleted" : "no such file");
    return ok;
  }
  Note("FileDelete", name, -1);
  return true;
}
uint64_t __fastcall FileShare(void*, const char* name) {
  Note("FileShare", name, -1);
  return 0;
}
bool __fastcall SetSyncPlatforms(void*, const char* name, int) {
  Note("SetSyncPlatforms", name, -1);
  return true;
}
uint64_t __fastcall FileWriteStreamOpen(void*, const char* name) {
  Note("FileWriteStreamOpen", name, -1);
  return ~0ull;  // k_UGCFileStreamHandleInvalid
}
bool __fastcall FileWriteStreamWriteChunk(void*, uint64_t, const void*, int32_t size) {
  Note("FileWriteStreamWriteChunk", nullptr, size);
  return false;
}
bool __fastcall FileWriteStreamClose(void*, uint64_t) {
  Note("FileWriteStreamClose", nullptr, -1);
  return false;
}
bool __fastcall FileWriteStreamCancel(void*, uint64_t) {
  Note("FileWriteStreamCancel", nullptr, -1);
  return false;
}
void __fastcall SetCloudEnabledForApp(void*, bool) { Note("SetCloudEnabledForApp", nullptr, -1); }
uint64_t __fastcall UGCDownloadToLocation(void*, uint64_t, const char* location, uint32_t) {
  Note("UGCDownloadToLocation", location, -1);
  return 0;
}

// ISteamUserStats. Flat names follow steam_api_flat.h: SetStat takes an int32, SetStat0 a
// float (MSVC puts overloads in reverse order, so their slots are 0x20 and 0x18).
bool __fastcall SetStatInt(void*, const char* name, int32_t value) {
  char v[32];
  snprintf(v, sizeof v, "%d", value);
  NoteStat("SetStat", name, v);
  return true;
}
bool __fastcall SetStatFloat(void*, const char* name, float value) {
  char v[32];
  snprintf(v, sizeof v, "%g", value);
  NoteStat("SetStat", name, v);
  return true;
}
bool __fastcall UpdateAvgRateStat(void*, const char* name, float count, double length) {
  char v[64];
  snprintf(v, sizeof v, "%g over %g s", count, length);
  NoteStat("UpdateAvgRateStat", name, v);
  return true;
}
bool __fastcall SetAchievement(void*, const char* name) {
  NoteAchievement("SetAchievement", name, nullptr);
  return true;
}
bool __fastcall ClearAchievement(void*, const char* name) {
  NoteAchievement("ClearAchievement", name, nullptr);
  return true;
}
bool __fastcall StoreStats(void*) {
  NoteStat("StoreStats", nullptr, nullptr);
  return true;
}
bool __fastcall IndicateAchievementProgress(void*, const char* name, uint32_t current, uint32_t max) {
  char v[32];
  snprintf(v, sizeof v, "%u of %u", current, max);
  NoteAchievement("IndicateAchievementProgress", name, v);
  return true;
}
bool __fastcall ResetAllStats(void*, bool achievements_too) {
  NoteAchievement("ResetAllStats", nullptr, achievements_too ? "with achievements" : "stats only");
  return true;
}
uint64_t __fastcall UploadLeaderboardScore(void*, uint64_t board, int, int32_t score) {
  char v[48];
  snprintf(v, sizeof v, "board %llu, score %d", (unsigned long long)board, score);
  NoteAchievement("UploadLeaderboardScore", nullptr, v);
  return 0;  // k_uAPICallInvalid
}
uint64_t __fastcall AttachLeaderboardUGC(void*, uint64_t board, uint64_t) {
  char v[32];
  snprintf(v, sizeof v, "board %llu", (unsigned long long)board);
  NoteAchievement("AttachLeaderboardUGC", nullptr, v);
  return 0;
}

// steam_trace=1: remote-storage reads still go to Steam, and are logged, to show where
// the game's saves come from.
volatile LONG g_reads_logged = 0;
void* o_FileRead;
void* o_FileReadAsync;
void* o_FileExists;
void* o_FilePersisted;
void* o_GetFileSize;
void* o_GetFileTimestamp;
void* o_GetFileCount;
void* o_GetFileNameAndSize;
void* o_IsCloudEnabledForAccount;
void* o_IsCloudEnabledForApp;

void NoteRead(const char* method, const char* name, const char* result) {
  if (InterlockedIncrement(&g_reads_logged) > 300) return;
  SowLog(L"steam: read %S('%S') -> %S", method, name ? name : "", result);
}

template <typename F>
F Orig(void* p) {
  return reinterpret_cast<F>(p);
}

int32_t __fastcall TraceFileRead(void* self, const char* name, void* data, int32_t size) {
  int32_t r = Orig<int32_t(__fastcall*)(void*, const char*, void*, int32_t)>(o_FileRead)(self, name, data, size);
  char v[48];
  snprintf(v, sizeof v, "%d of %d bytes", r, size);
  NoteRead("FileRead", name, v);
  return r;
}
uint64_t __fastcall TraceFileReadAsync(void* self, const char* name, uint32_t offset, uint32_t size) {
  uint64_t r = Orig<uint64_t(__fastcall*)(void*, const char*, uint32_t, uint32_t)>(o_FileReadAsync)(self, name, offset,
                                                                                                    size);
  char v[64];
  snprintf(v, sizeof v, "call %llu for %u bytes at %u", (unsigned long long)r, size, offset);
  NoteRead("FileReadAsync", name, v);
  return r;
}
bool __fastcall TraceFileExists(void* self, const char* name) {
  bool r = Orig<bool(__fastcall*)(void*, const char*)>(o_FileExists)(self, name);
  NoteRead("FileExists", name, r ? "true" : "false");
  return r;
}
bool __fastcall TraceFilePersisted(void* self, const char* name) {
  bool r = Orig<bool(__fastcall*)(void*, const char*)>(o_FilePersisted)(self, name);
  NoteRead("FilePersisted", name, r ? "true" : "false");
  return r;
}
int32_t __fastcall TraceGetFileSize(void* self, const char* name) {
  int32_t r = Orig<int32_t(__fastcall*)(void*, const char*)>(o_GetFileSize)(self, name);
  char v[32];
  snprintf(v, sizeof v, "%d", r);
  NoteRead("GetFileSize", name, v);
  return r;
}
int64_t __fastcall TraceGetFileTimestamp(void* self, const char* name) {
  int64_t r = Orig<int64_t(__fastcall*)(void*, const char*)>(o_GetFileTimestamp)(self, name);
  char v[32];
  snprintf(v, sizeof v, "%lld", (long long)r);
  NoteRead("GetFileTimestamp", name, v);
  return r;
}
int32_t __fastcall TraceGetFileCount(void* self) {
  int32_t r = Orig<int32_t(__fastcall*)(void*)>(o_GetFileCount)(self);
  char v[32];
  snprintf(v, sizeof v, "%d", r);
  NoteRead("GetFileCount", nullptr, v);
  return r;
}
const char* __fastcall TraceGetFileNameAndSize(void* self, int index, int32_t* size) {
  const char* r = Orig<const char*(__fastcall*)(void*, int, int32_t*)>(o_GetFileNameAndSize)(self, index, size);
  char v[300];
  snprintf(v, sizeof v, "#%d '%s', %d bytes", index, r ? r : "", size ? *size : -1);
  NoteRead("GetFileNameAndSize", nullptr, v);
  return r;
}
bool __fastcall TraceIsCloudEnabledForAccount(void* self) {
  bool r = Orig<bool(__fastcall*)(void*)>(o_IsCloudEnabledForAccount)(self);
  NoteRead("IsCloudEnabledForAccount", nullptr, r ? "true" : "false");
  return r;
}
bool __fastcall TraceIsCloudEnabledForApp(void* self) {
  bool r = Orig<bool(__fastcall*)(void*)>(o_IsCloudEnabledForApp)(self);
  NoteRead("IsCloudEnabledForApp", nullptr, r ? "true" : "false");
  return r;
}

// steam_cloud=sandbox: the game's cloud reads answered from this copy's folder.
int32_t __fastcall CloudFileRead(void*, const char* name, void* data, int32_t size) {
  std::wstring path = CloudPath(name);
  int32_t got = 0;
  HANDLE f = path.empty() || size <= 0 || !data
                 ? INVALID_HANDLE_VALUE
                 : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD n = 0;
    if (ReadFile(f, data, (DWORD)size, &n, nullptr)) got = (int32_t)n;
    CloseHandle(f);
  }
  char v[48];
  snprintf(v, sizeof v, "%d of %d bytes", got, size);
  NoteCloud("FileRead", name, v);
  return got;
}
uint64_t __fastcall CloudFileReadAsync(void*, const char* name, uint32_t, uint32_t) {
  NoteCloud("FileReadAsync", name, "not supported (call invalid)");
  return 0;  // k_uAPICallInvalid: the game gets no result callback
}
bool __fastcall CloudFileExists(void*, const char* name) {
  WIN32_FILE_ATTRIBUTE_DATA info;
  bool r = CloudFileInfo(CloudPath(name), &info);
  NoteCloud("FileExists", name, r ? "true" : "false");
  return r;
}
bool __fastcall CloudFilePersisted(void*, const char* name) {
  WIN32_FILE_ATTRIBUTE_DATA info;
  return CloudFileInfo(CloudPath(name), &info);
}
int32_t __fastcall CloudGetFileSize(void*, const char* name) {
  WIN32_FILE_ATTRIBUTE_DATA info;
  int32_t r = CloudFileInfo(CloudPath(name), &info) ? (int32_t)info.nFileSizeLow : 0;
  char v[32];
  snprintf(v, sizeof v, "%d", r);
  NoteCloud("GetFileSize", name, v);
  return r;
}
int64_t __fastcall CloudGetFileTimestamp(void*, const char* name) {
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (!CloudFileInfo(CloudPath(name), &info)) return 0;
  ULARGE_INTEGER t;
  t.LowPart = info.ftLastWriteTime.dwLowDateTime;
  t.HighPart = info.ftLastWriteTime.dwHighDateTime;
  return (int64_t)((t.QuadPart - 116444736000000000ull) / 10000000ull);  // Unix seconds
}

// Top-level files of the cloud folder, refreshed by GetFileCount (Steam's usage: count,
// then name and size by index).
std::vector<std::pair<std::string, int32_t>> g_listing;
CRITICAL_SECTION g_listing_lock;

int32_t __fastcall CloudGetFileCount(void*) {
  EnterCriticalSection(&g_listing_lock);
  g_listing.clear();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_cloud + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring n = fd.cFileName;
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n.size() > 7 && n.compare(n.size() - 7, 7, L".sowtmp") == 0)
        continue;
      char utf8[MAX_PATH * 3];
      if (WideCharToMultiByte(CP_UTF8, 0, n.c_str(), -1, utf8, sizeof utf8, nullptr, nullptr) > 0)
        g_listing.emplace_back(utf8, (int32_t)fd.nFileSizeLow);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(g_listing.begin(), g_listing.end());
  int32_t r = (int32_t)g_listing.size();
  LeaveCriticalSection(&g_listing_lock);
  char v[32];
  snprintf(v, sizeof v, "%d", r);
  NoteCloud("GetFileCount", nullptr, v);
  return r;
}
const char* __fastcall CloudGetFileNameAndSize(void*, int index, int32_t* size) {
  EnterCriticalSection(&g_listing_lock);
  const char* r = "";
  int32_t s = 0;
  if (index >= 0 && index < (int)g_listing.size()) {
    r = g_listing[index].first.c_str();
    s = g_listing[index].second;
  }
  LeaveCriticalSection(&g_listing_lock);
  if (size) *size = s;
  return r;
}

struct Slot {
  const char* flat;  // steam_api64 export whose body names the vtable offset
  void* replacement;
  void** original = nullptr;  // where to keep the replaced method (pass-through slots)
};

const Slot kStorageSlots[] = {
    {"SteamAPI_ISteamRemoteStorage_FileWrite", (void*)&FileWrite},
    {"SteamAPI_ISteamRemoteStorage_FileWriteAsync", (void*)&FileWriteAsync},
    {"SteamAPI_ISteamRemoteStorage_FileForget", (void*)&FileForget},
    {"SteamAPI_ISteamRemoteStorage_FileDelete", (void*)&FileDelete},
    {"SteamAPI_ISteamRemoteStorage_FileShare", (void*)&FileShare},
    {"SteamAPI_ISteamRemoteStorage_SetSyncPlatforms", (void*)&SetSyncPlatforms},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen", (void*)&FileWriteStreamOpen},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk", (void*)&FileWriteStreamWriteChunk},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamClose", (void*)&FileWriteStreamClose},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel", (void*)&FileWriteStreamCancel},
    {"SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp", (void*)&SetCloudEnabledForApp},
    {"SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation", (void*)&UGCDownloadToLocation},
};

const Slot kStatSlots[] = {
    {"SteamAPI_ISteamUserStats_SetStat", (void*)&SetStatInt},
    {"SteamAPI_ISteamUserStats_SetStat0", (void*)&SetStatFloat},
    {"SteamAPI_ISteamUserStats_UpdateAvgRateStat", (void*)&UpdateAvgRateStat},
    {"SteamAPI_ISteamUserStats_SetAchievement", (void*)&SetAchievement},
    {"SteamAPI_ISteamUserStats_ClearAchievement", (void*)&ClearAchievement},
    {"SteamAPI_ISteamUserStats_StoreStats", (void*)&StoreStats},
    {"SteamAPI_ISteamUserStats_IndicateAchievementProgress", (void*)&IndicateAchievementProgress},
    {"SteamAPI_ISteamUserStats_ResetAllStats", (void*)&ResetAllStats},
    {"SteamAPI_ISteamUserStats_UploadLeaderboardScore", (void*)&UploadLeaderboardScore},
    {"SteamAPI_ISteamUserStats_AttachLeaderboardUGC", (void*)&AttachLeaderboardUGC},
};

const Slot kTraceSlots[] = {
    {"SteamAPI_ISteamRemoteStorage_FileRead", (void*)&TraceFileRead, &o_FileRead},
    {"SteamAPI_ISteamRemoteStorage_FileReadAsync", (void*)&TraceFileReadAsync, &o_FileReadAsync},
    {"SteamAPI_ISteamRemoteStorage_FileExists", (void*)&TraceFileExists, &o_FileExists},
    {"SteamAPI_ISteamRemoteStorage_FilePersisted", (void*)&TraceFilePersisted, &o_FilePersisted},
    {"SteamAPI_ISteamRemoteStorage_GetFileSize", (void*)&TraceGetFileSize, &o_GetFileSize},
    {"SteamAPI_ISteamRemoteStorage_GetFileTimestamp", (void*)&TraceGetFileTimestamp, &o_GetFileTimestamp},
    {"SteamAPI_ISteamRemoteStorage_GetFileCount", (void*)&TraceGetFileCount, &o_GetFileCount},
    {"SteamAPI_ISteamRemoteStorage_GetFileNameAndSize", (void*)&TraceGetFileNameAndSize, &o_GetFileNameAndSize},
    {"SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount", (void*)&TraceIsCloudEnabledForAccount,
     &o_IsCloudEnabledForAccount},
    {"SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp", (void*)&TraceIsCloudEnabledForApp, &o_IsCloudEnabledForApp},
};

const Slot kCloudSlots[] = {
    {"SteamAPI_ISteamRemoteStorage_FileRead", (void*)&CloudFileRead},
    {"SteamAPI_ISteamRemoteStorage_FileReadAsync", (void*)&CloudFileReadAsync},
    {"SteamAPI_ISteamRemoteStorage_FileExists", (void*)&CloudFileExists},
    {"SteamAPI_ISteamRemoteStorage_FilePersisted", (void*)&CloudFilePersisted},
    {"SteamAPI_ISteamRemoteStorage_GetFileSize", (void*)&CloudGetFileSize},
    {"SteamAPI_ISteamRemoteStorage_GetFileTimestamp", (void*)&CloudGetFileTimestamp},
    {"SteamAPI_ISteamRemoteStorage_GetFileCount", (void*)&CloudGetFileCount},
    {"SteamAPI_ISteamRemoteStorage_GetFileNameAndSize", (void*)&CloudGetFileNameAndSize},
};

enum class When { kAlways, kTrace, kCloud };

struct Iface {
  const char* label;   // for the log
  const char* what;    // what the patch does, for the log
  const char* version;
  const char* getter;  // SteamAPI_ISteamClient_GetISteam... export
  const Slot* slots;
  size_t count;
  When when;           // kTrace: steam_trace=1 (and no sandbox cloud); kCloud: steam_cloud=sandbox
};

const Iface kIfaces[] = {
    {"remote-storage", "writes blocked", kRemoteStorageVersion, "SteamAPI_ISteamClient_GetISteamRemoteStorage",
     kStorageSlots, sizeof kStorageSlots / sizeof kStorageSlots[0], When::kAlways},
    {"user-stats", "writes blocked", kUserStatsVersion, "SteamAPI_ISteamClient_GetISteamUserStats", kStatSlots,
     sizeof kStatSlots / sizeof kStatSlots[0], When::kAlways},
    {"remote-storage", "reads traced", kRemoteStorageVersion, "SteamAPI_ISteamClient_GetISteamRemoteStorage",
     kTraceSlots, sizeof kTraceSlots / sizeof kTraceSlots[0], When::kTrace},
    {"remote-storage", "reads served from the sandbox cloud", kRemoteStorageVersion,
     "SteamAPI_ISteamClient_GetISteamRemoteStorage", kCloudSlots, sizeof kCloudSlots / sizeof kCloudSlots[0],
     When::kCloud},
};
bool g_trace = false;

bool Wanted(const Iface& f) {
  switch (f.when) {
    case When::kTrace: return g_trace && g_cloud.empty();
    case When::kCloud: return !g_cloud.empty();
    default: return true;
  }
}
constexpr size_t kMaxSlots = 16;

// Vtable offset named by a flat wrapper: 48 8B 01 (mov rax,[rcx]) then 48 FF 20
// (jmp [rax]), 48 FF 60 d8 (jmp [rax+d8]) or 48 FF A0 d32 (jmp [rax+d32]); or the same
// through r10: 4C 8B 11 then 49 FF 22 / 49 FF 62 d8 / 49 FF A2 d32. -1 otherwise.
long SlotOffset(const uint8_t* f) {
  const bool rax = f[0] == 0x48 && f[1] == 0x8B && f[2] == 0x01 && f[3] == 0x48 && f[4] == 0xFF;
  const bool r10 = f[0] == 0x4C && f[1] == 0x8B && f[2] == 0x11 && f[3] == 0x49 && f[4] == 0xFF;
  if (!rax && !r10) return -1;
  const uint8_t modrm = (uint8_t)(f[5] - (r10 ? 2 : 0));
  if (modrm == 0x20) return 0;
  if (modrm == 0x60) return f[6];
  if (modrm == 0xA0) {
    int32_t d;
    memcpy(&d, f + 6, 4);
    return d >= 0 && d < 0x1000 ? d : -1;
  }
  return -1;
}

bool InModule(void* p) {
  HMODULE m = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m) &&
         m != nullptr;
}

using GetPipe_t = int(__cdecl*)();
using SteamClient_t = void*(__cdecl*)();
using GetInterface_t = void*(__cdecl*)(void* client, int user, int pipe, const char* version);

void PatchVtable(const Iface& f, void* object, const long* offsets, ULONGLONG t0, int pipe) {
  void** vtable = *static_cast<void***>(object);
  for (size_t i = 0; i < f.count; ++i)
    if (!InModule(vtable[offsets[i] / 8]))
      SowFatal(kExitSteamGuard, L"steam: vtable slot %ld (%S) does not point into a module", offsets[i], f.slots[i].flat);
  long max_off = 0;
  for (size_t i = 0; i < f.count; ++i) max_off = std::max(max_off, offsets[i]);
  DWORD old = 0;
  if (!VirtualProtect(vtable, max_off + 8, PAGE_READWRITE, &old))
    SowFatal(kExitSteamGuard, L"steam: cannot unprotect the %S vtable (%lu)", f.version, GetLastError());
  for (size_t i = 0; i < f.count; ++i) {
    if (f.slots[i].original) *f.slots[i].original = vtable[offsets[i] / 8];
    vtable[offsets[i] / 8] = f.slots[i].replacement;
  }
  VirtualProtect(vtable, max_off + 8, old, &old);
  SowLog(L"steam: %S %S (%zu methods, %llu ms after start, pipe %d)", f.label, f.what, f.count, GetTickCount64() - t0,
         pipe);
}

DWORD WINAPI SteamGuardThread(void*) {
  HMODULE api = GetModuleHandleW(L"steam_api64.dll");
  if (!api) {
    SowLog(L"steam: steam_api64.dll not loaded, Steam guard not needed");
    return 0;
  }
  auto pipe_fn = reinterpret_cast<GetPipe_t>(GetProcAddress(api, "SteamAPI_GetHSteamPipe"));
  auto user_fn = reinterpret_cast<GetPipe_t>(GetProcAddress(api, "SteamAPI_GetHSteamUser"));
  auto client_fn = reinterpret_cast<SteamClient_t>(GetProcAddress(api, "SteamClient"));
  constexpr size_t kIfaceCount = sizeof kIfaces / sizeof kIfaces[0];
  GetInterface_t getters[kIfaceCount];
  for (size_t k = 0; k < kIfaceCount; ++k)
    getters[k] = reinterpret_cast<GetInterface_t>(GetProcAddress(api, kIfaces[k].getter));
  if (!pipe_fn || !user_fn || !client_fn || std::find(getters, getters + kIfaceCount, nullptr) != getters + kIfaceCount)
    SowFatal(kExitSteamGuard, L"steam: steam_api64 lacks the exports the Steam guard needs");

  // Every offset is checked before Steam is up, so a mismatch stops the game early.
  long offsets[kIfaceCount][kMaxSlots];
  for (size_t k = 0; k < kIfaceCount; ++k)
    for (size_t i = 0; i < kIfaces[k].count && Wanted(kIfaces[k]); ++i) {
      auto* f = reinterpret_cast<const uint8_t*>(GetProcAddress(api, kIfaces[k].slots[i].flat));
      offsets[k][i] = f ? SlotOffset(f) : -1;
      if (offsets[k][i] < 0 || offsets[k][i] % 8)
        SowFatal(kExitSteamGuard, L"steam: %S is not the expected one-slot wrapper", kIfaces[k].slots[i].flat);
    }

  // Wait for the game's SteamAPI_Init (a pipe exists). Fast at first: the game initialises
  // Steam early and must not get to save before the vtable is patched.
  const ULONGLONG t0 = GetTickCount64();
  int pipe = 0;
  while ((pipe = pipe_fn()) == 0) Sleep(GetTickCount64() - t0 < 60000 ? 10 : 250);
  const int user = user_fn();
  void* client = client_fn();
  for (size_t k = 0; k < kIfaceCount; ++k) {
    if (!Wanted(kIfaces[k])) continue;
    void* object = client ? getters[k](client, user, pipe, kIfaces[k].version) : nullptr;
    if (!object) SowFatal(kExitSteamGuard, L"steam: no %S after Steam init", kIfaces[k].version);
    PatchVtable(kIfaces[k], object, offsets[k], t0, pipe);
  }
  return 0;
}

}  // namespace

void StartSteamGuard(const SteamGuardConfig& cfg) {
  g_trace = cfg.trace_reads;
  g_cloud = cfg.cloud_dir;
  InitializeCriticalSection(&g_listing_lock);
  if (!g_cloud.empty()) SowLog(L"steam: cloud files come from and go to %s (steam_cloud=sandbox)", g_cloud.c_str());
#ifdef SOWGUARD_MUTANT  // negative control: no Steam guard
  SowLog(L"steam: Steam guard disabled (mutant build)");
#else
  HANDLE t = CreateThread(nullptr, 0, SteamGuardThread, nullptr, 0, nullptr);
  if (!t) SowFatal(kExitSteamGuard, L"steam: cannot start the Steam guard thread");
  CloseHandle(t);
#endif
}
