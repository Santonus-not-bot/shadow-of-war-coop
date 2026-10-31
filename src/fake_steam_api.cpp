// A stand-in steam_api64.dll for the offline tests of steamguard. No Steam involved.
//
// Exports the functions steamguard uses, with the flat wrappers compiled to the same
// one-jump shape as the real ones (mov rax,[rcx]; jmp [rax+off]), a fake ISteamClient
// whose slots 0x88 and 0x68 return a fake ISteamRemoteStorage and ISteamUserStats, and
// vtables that count calls per slot. Test helpers: FakeSteam_SetReady, FakeSteam_Count,
// FakeSteam_RemoteStorage, FakeSteam_UserStatsCount, FakeSteam_UserStats.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

#define EXPORT extern "C" __declspec(dllexport)

namespace {

constexpr int kSlots = 64;
volatile LONG g_count[2][kSlots];  // [0] remote storage, [1] user stats
volatile LONG g_ready = 0;

// Every slot: count the call; return values the guard's replacements never use, so a
// test can tell who answered (writes: false/0; FileRead: 7; GetAchievement: 9).
template <int I, int N>
uint64_t __fastcall Slot(void*, uint64_t, uint64_t, uint64_t) {
  InterlockedIncrement(&g_count[I][N]);
  return I == 0 ? (N == 1 ? 7 : 0) : (N == 6 ? 9 : 0);
}

template <int I, int... N>
struct Table {
  void* v[sizeof...(N)] = {reinterpret_cast<void*>(&Slot<I, N>)...};
};
#define SLOTS_0_63 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, \
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,   \
    55, 56, 57, 58, 59, 60, 61, 62, 63
const Table<0, SLOTS_0_63> g_rs_vtable;
const Table<1, SLOTS_0_63> g_us_vtable;
struct Obj {
  const void* const* vtable;
};
Obj g_rs = {g_rs_vtable.v};
Obj g_us = {g_us_vtable.v};

void* __fastcall GetRemoteStorage(void*, int, int, const char*) { return &g_rs; }
void* __fastcall GetUserStats(void*, int, int, const char*) { return &g_us; }
void* g_client_vtable[32];
Obj g_client = {g_client_vtable};

using Fn4 = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
inline Fn4 At(void* self, int slot) { return reinterpret_cast<Fn4>((*static_cast<void* const* const*>(self))[slot]); }

}  // namespace

EXPORT int SteamAPI_GetHSteamPipe() { return g_ready ? 3 : 0; }
EXPORT int SteamAPI_GetHSteamUser() { return 1; }
EXPORT void* SteamClient() { return &g_client; }
EXPORT void* SteamAPI_ISteamClient_GetISteamRemoteStorage(void* self, uint64_t a, uint64_t b, uint64_t c) {
  return reinterpret_cast<void*>(At(self, 0x88 / 8)(self, a, b, c));
}
EXPORT void* SteamAPI_ISteamClient_GetISteamUserStats(void* self, uint64_t a, uint64_t b, uint64_t c) {
  return reinterpret_cast<void*>(At(self, 0x68 / 8)(self, a, b, c));
}

// The flat wrappers as raw bytes, exactly the shapes in the real steam_api64.dll
// (SHA-256 f12cf470...): mov rax,[rcx] then jmp [rax] / jmp [rax+d8] / jmp [rax+d32].
// (This compiler would emit mov r10,[rax+off]; jmp r10, which the guard rightly rejects.)
#pragma section(".flat", read, execute)
#define FLAT0(name) EXPORT __declspec(allocate(".flat")) const unsigned char SteamAPI_ISteamRemoteStorage_##name[] = \
    {0x48, 0x8B, 0x01, 0x48, 0xFF, 0x20, 0xCC, 0xCC};
#define FLAT8(name, off) EXPORT __declspec(allocate(".flat")) const unsigned char SteamAPI_ISteamRemoteStorage_##name[] = \
    {0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, off, 0xCC};
#define FLAT32(name, lo, hi) EXPORT __declspec(allocate(".flat")) const unsigned char SteamAPI_ISteamRemoteStorage_##name[] = \
    {0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0, lo, hi, 0x00, 0x00, 0xCC, 0xCC};
FLAT0(FileWrite)                        // slot 0
FLAT8(FileRead, 0x08)                   // slot 1
FLAT8(FileWriteAsync, 0x10)             // slot 2
FLAT8(FileForget, 0x28)                 // slot 5
FLAT8(FileDelete, 0x30)                 // slot 6
FLAT8(FileShare, 0x38)                  // slot 7
FLAT8(SetSyncPlatforms, 0x40)           // slot 8
FLAT8(FileWriteStreamOpen, 0x48)        // slot 9
FLAT8(FileWriteStreamWriteChunk, 0x50)  // slot 10
FLAT8(FileWriteStreamClose, 0x58)       // slot 11
FLAT8(FileWriteStreamCancel, 0x60)      // slot 12
FLAT32(SetCloudEnabledForApp, 0xB8, 0x00)    // slot 23
FLAT32(UGCDownloadToLocation, 0xB0, 0x01)    // slot 54
// Reads (traced with steam_trace=1).
FLAT8(FileReadAsync, 0x18)              // slot 3
FLAT8(FileExists, 0x68)                 // slot 13
FLAT8(FilePersisted, 0x70)              // slot 14
FLAT8(GetFileSize, 0x78)                // slot 15
FLAT32(GetFileTimestamp, 0x80, 0x00)    // slot 16
FLAT32(GetFileCount, 0x90, 0x00)        // slot 18
FLAT32(GetFileNameAndSize, 0x98, 0x00)  // slot 19
FLAT32(IsCloudEnabledForAccount, 0xA8, 0x00)  // slot 21
FLAT32(IsCloudEnabledForApp, 0xB0, 0x00)      // slot 22

// ISteamUserStats v011, offsets as in the real DLL; UploadLeaderboardScore goes through
// r10 there (mov r10,[rcx]; jmp [r10+d32]).
#define US8(name, off) EXPORT __declspec(allocate(".flat")) const unsigned char SteamAPI_ISteamUserStats_##name[] = \
    {0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, off, 0xCC};
#define US32(name, lo, hi) EXPORT __declspec(allocate(".flat")) const unsigned char SteamAPI_ISteamUserStats_##name[] = \
    {0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0, lo, hi, 0x00, 0x00, 0xCC, 0xCC};
#define US32R10(name, lo, hi) EXPORT __declspec(allocate(".flat")) const unsigned char SteamAPI_ISteamUserStats_##name[] = \
    {0x4C, 0x8B, 0x11, 0x49, 0xFF, 0xA2, lo, hi, 0x00, 0x00, 0xCC, 0xCC};
US8(SetStat0, 0x18)                     // slot 3 (float)
US8(SetStat, 0x20)                      // slot 4 (int32)
US8(UpdateAvgRateStat, 0x28)            // slot 5
US8(GetAchievement, 0x30)               // slot 6
US8(SetAchievement, 0x38)               // slot 7
US8(ClearAchievement, 0x40)             // slot 8
US8(StoreStats, 0x50)                   // slot 10
US8(IndicateAchievementProgress, 0x68)  // slot 13
US32(ResetAllStats, 0xA8, 0x00)         // slot 21
US32R10(UploadLeaderboardScore, 0xF8, 0x00)  // slot 31
US32(AttachLeaderboardUGC, 0x00, 0x01)  // slot 32

EXPORT void FakeSteam_SetReady(int ready) { InterlockedExchange(&g_ready, ready); }
EXPORT long FakeSteam_Count(int slot) { return slot >= 0 && slot < kSlots ? g_count[0][slot] : -1; }
EXPORT void* FakeSteam_RemoteStorage() { return &g_rs; }
EXPORT long FakeSteam_UserStatsCount(int slot) { return slot >= 0 && slot < kSlots ? g_count[1][slot] : -1; }
EXPORT void* FakeSteam_UserStats() { return &g_us; }

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_client_vtable[0x88 / 8] = reinterpret_cast<void*>(&GetRemoteStorage);
    g_client_vtable[0x68 / 8] = reinterpret_cast<void*>(&GetUserStats);
  }
  return TRUE;
}
