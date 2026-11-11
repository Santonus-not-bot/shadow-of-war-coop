// simhook: run game calls on the game's simulation thread (see simhook.h).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "input.h"
#include "coop_rules.h"
#include "listfix.h"
#include "probe.h"
#include "simhook.h"

namespace {

const uint8_t* Base() { return reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)); }

// True when p is inside an executable section of the game's own image.
bool IsGameCode(const void* p) {
  const uint8_t* base = Base();
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto* sec = IMAGE_FIRST_SECTION(nt);
  const auto* b = static_cast<const uint8_t*>(p);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && b >= base + sec->VirtualAddress &&
        b < base + sec->VirtualAddress + sec->Misc.VirtualSize)
      return true;
  return false;
}

bool ReadPtr(void* const* at, void** out) {
  __try {
    *out = *at;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool WriteSlot(void** slot, void* value) {
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  InterlockedExchangePointer(slot, value);
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}

void PutRel32(uint8_t* at, const void* from_end, const void* to) {
  const int32_t rel = (int32_t)(static_cast<const uint8_t*>(to) - static_cast<const uint8_t*>(from_end));
  memcpy(at, &rel, 4);
}

// ---------------------------------------------------------------- count

// Per slot, 28 bytes of code that touch only rax and the flags (not arguments in the
// Windows x64 convention), then continue into the original:
//   F0 48 FF 05 <rel32>       lock inc qword [rip+calls]
//   65 8B 04 25 48 00 00 00   mov eax, gs:[48h]          ; TEB thread id
//   89 05 <rel32>             mov [rip+thread], eax
//   FF 25 <rel32>             jmp [rip+original]
struct CountData {
  volatile LONG64 calls;
  volatile DWORD thread;
  DWORD pad;
  void* original;
};

}  // namespace

std::string SimCountSlots(uintptr_t vtable_rva, unsigned first, unsigned n, unsigned ms) {
  if (n == 0 || n > 64 || ms == 0 || ms > 30000) return "err usage: count <vtable rva> <first slot> <1..64 slots> <1..30000 ms>\n";
  void** vt = reinterpret_cast<void**>(const_cast<uint8_t*>(Base()) + vtable_rva) + first;
  void* p = nullptr;
  for (unsigned i = 0; i < n; ++i)
    if (!ReadPtr(&vt[i], &p) || !IsGameCode(p)) {
      char e[96];
      snprintf(e, sizeof e, "err slot %u does not point into the game's code\n", first + i);
      return e;
    }
  // One page, never freed: a stub may still be running on another thread after the restore.
  auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!page) return "err no memory for the counting stubs\n";
  auto* data = reinterpret_cast<CountData*>(page + 2048);
  for (unsigned i = 0; i < n; ++i) {
    uint8_t* s = page + 32 * i;
    data[i] = CountData{0, 0, 0, vt[i]};
    const uint8_t code[] = {0xF0, 0x48, 0xFF, 0x05, 0, 0, 0, 0, 0x65, 0x8B, 0x04, 0x25, 0x48, 0x00,
                            0x00, 0x00, 0x89, 0x05, 0, 0, 0, 0, 0xFF, 0x25, 0, 0, 0, 0};
    memcpy(s, code, sizeof code);
    PutRel32(s + 4, s + 8, const_cast<LONG64*>(&data[i].calls));
    PutRel32(s + 18, s + 22, const_cast<DWORD*>(&data[i].thread));
    PutRel32(s + 24, s + 28, &data[i].original);
  }
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  for (unsigned i = 0; i < n; ++i) WriteSlot(&vt[i], page + 32 * i);
  SowLog(L"probe: counting calls to %u slots of vtable rva:%llX for %u ms", n, (unsigned long long)vtable_rva, ms);
  Sleep(ms);
  for (unsigned i = 0; i < n; ++i) WriteSlot(&vt[i], data[i].original);
  const DWORD sim = InputGameThread();
  std::string out = "ok simulation thread " + std::to_string(sim) + "\n";
  char line[160];
  for (unsigned i = 0; i < n; ++i) {
    const LONG64 c = data[i].calls;
    snprintf(line, sizeof line, "slot %u rva:%llX calls=%lld per_s=%.1f thread=%lu%s\n", first + i,
             (unsigned long long)(static_cast<const uint8_t*>(data[i].original) - Base()), c, c * 1000.0 / ms,
             data[i].thread, c && data[i].thread == sim ? " (simulation)" : "");
    out += line;
  }
  return out;
}

namespace {

// ---------------------------------------------------------------- simulation-thread hook

// Pre-hook stub: saves rax, rcx, rdx, r8-r11, xmm0-5 and the flags, calls SimWork, restores
// them and jumps to the original. Entry rsp is 8 mod 16; eight pushes keep it there; the
// sub makes it 16-aligned for the call.
//   0  9C                       pushfq
//   1  50 51 52                 push rax, rcx, rdx
//   4  41 50 41 51 41 52 41 53  push r8, r9, r10, r11
//  12  48 81 EC 88 00 00 00     sub rsp, 88h
//  19  movdqu [rsp+20h+16*i], xmm0..5      (6 x 6 bytes)
//  55  48 B8 <imm64>            mov rax, SimWork
//  65  FF D0                    call rax
//  67  movdqu xmm0..5, [rsp+20h+16*i]      (6 x 6 bytes)
// 103  48 81 C4 88 00 00 00     add rsp, 88h
// 110  41 5B 41 5A 41 59 41 58  pop r11, r10, r9, r8
// 118  5A 59 58                 pop rdx, rcx, rax
// 121  9D                       popfq
// 122  FF 25 <rel32>            jmp [rip+original]   (original at page+224)
constexpr size_t kPreStubSize = 128;

// UNWIND_INFO: version 1, prologue 19 bytes, 10 slots: sub rsp,88h (UWOP_ALLOC_LARGE, 17 in
// the next slot), then the eight pushes as UWOP_ALLOC_SMALL 8 at their end offsets.
const uint8_t kPreStubUnwind[24] = {0x01, 19, 10, 0x00, 19, 0x01, 17, 0x00, 12, 0x02, 10, 0x02,
                                    8,    0x02, 6, 0x02, 4, 0x02, 3, 0x02, 2, 0x02, 1, 0x02};

using Lkp_t = void(__fastcall*)(void* player, const float* pos);
CRITICAL_SECTION g_lock;
struct LockInit {
  LockInit() { InitializeCriticalSection(&g_lock); }
} g_lock_init;  // the probe may ask before sim_hook is in
volatile LONG g_installed = 0;
volatile LONG g_pending = 0;
float g_pos[3] = {};
Lkp_t g_lkp = nullptr;
uintptr_t g_player_global = 0;
volatile LONG g_shown = 0;
volatile LONG g_wrong_thread_logged = 0;

void* ReadPlayer() {
  __try {
    auto gc = *reinterpret_cast<uint8_t* const*>(g_player_global);
    return gc ? *reinterpret_cast<void* const*>(gc + 0x888) : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void* PlayerEffect(void* player) {
  __try {
    return *reinterpret_cast<void* const*>(static_cast<uint8_t*>(player) + 0x3890);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// Puppet state. The transform is what 1820FC builds: position, then the quaternion.
struct Xform {
  float pos[3];
  float rot[4];
};
using Teleport_t = void(__fastcall*)(void* object, const Xform* xform);
uintptr_t g_puppet = 0, g_puppet_vt = 0, g_puppet_health = 0;
Teleport_t g_teleport = nullptr;
volatile LONG g_move_pending = 0;
Xform g_move{};
// Direct: the transform written in place (+0xAC position, as the game's transform setter 14554C
// writes it, and +0xB8 rotation) instead of the queued teleport, which puts a character on the
// ground below the given point (a stand-in "teleported" to a sender in the air landed 2,374
// units down, at the foot of the tower; session an).
bool g_move_direct = false;

bool WriteXform(uintptr_t owner, const Xform& x) {
  for (float v : x.pos)
    if (!std::isfinite(v)) return false;
  __try {
    memcpy(reinterpret_cast<void*>(owner + 0xAC), x.pos, sizeof x.pos);
    memcpy(reinterpret_cast<void*>(owner + 0xB8), x.rot, sizeof x.rot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}
volatile LONG g_moves = 0;

bool ReadQ(uintptr_t at, uintptr_t* out) {
  __try {
    *out = *reinterpret_cast<const uintptr_t*>(at);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The puppet still is what was chosen: same class, and its health object still points
// back at it (a freed or reused object fails this).
bool PuppetAlive(uintptr_t owner, uintptr_t vt, uintptr_t health) {
  uintptr_t v = 0, h = 0, back = 0;
  return ReadQ(owner, &v) && v == vt && ReadQ(owner + 0x26A8, &h) && h == health && ReadQ(h + 0x28, &back) &&
         back == owner;
}

void RunLkp(DWORD me) {
  float pos[3];
  EnterCriticalSection(&g_lock);
  const bool run = InterlockedExchange(&g_pending, 0) != 0;
  memcpy(pos, g_pos, sizeof pos);
  LeaveCriticalSection(&g_lock);
  void* player = run ? ReadPlayer() : nullptr;
  if (!player) return;
  g_lkp(player, pos);
  const LONG n = InterlockedIncrement(&g_shown);
  if (n <= 20 || n % 100 == 0)
    SowLog(L"sim: last-known-position effect #%ld at %.1f,%.1f,%.1f (player+0x3890 = %p, thread %lu)", n, pos[0],
           pos[1], pos[2], PlayerEffect(player), me);
}

void RunPuppetMove(DWORD me) {
  EnterCriticalSection(&g_lock);
  const bool run = InterlockedExchange(&g_move_pending, 0) != 0;
  const Xform x = g_move;
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!run || !owner) return;
  if (!PuppetAlive(owner, vt, health)) {
    EnterCriticalSection(&g_lock);
    if (g_puppet == owner) g_puppet = 0;
    LeaveCriticalSection(&g_lock);
    SowLog(L"sim: puppet abs:%llX is gone (unloaded or freed); no puppet now", (unsigned long long)owner);
    return;
  }
  EnterCriticalSection(&g_lock);
  const bool direct = g_move_direct;
  LeaveCriticalSection(&g_lock);
  if (direct) {
    WriteXform(owner, x);
    return;
  }
  g_teleport(reinterpret_cast<void*>(owner), &x);
  const LONG n = InterlockedIncrement(&g_moves);
  if (n <= 10 || n % 200 == 0)
    SowLog(L"sim: puppet move #%ld to %.1f,%.1f,%.1f (thread %lu)", n, x.pos[0], x.pos[1], x.pos[2], me);
}

// Puppet AI and walking (defined below): queued work for the simulation thread.
bool ExtraPending();
void RunExtra();
}  // namespace
void RefreshInputOwner();
bool InputsOn();
void ApplyMirror();
namespace {

uintptr_t ImageEnd();  // below

// Access violations in the game's own code, on any thread, go to the log with the faulting
// address and the call stack (the first 16; the exception is passed on unchanged). A test
// copy that dies otherwise leaves only its exit code.
volatile LONG g_crashes_logged = 0;
LONG CALLBACK CrashLogHandler(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD* x = e->ExceptionRecord;
  const uintptr_t rip = e->ContextRecord->Rip, base = reinterpret_cast<uintptr_t>(Base());
  if (x->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rip < base || rip >= ImageEnd() ||
      InterlockedIncrement(&g_crashes_logged) > 16)
    return EXCEPTION_CONTINUE_SEARCH;
  wchar_t line[512];
  int n = swprintf_s(line, L"crash: access violation at rva:%llX (%s abs:%llX) thread %lu, stack",
                     (unsigned long long)(rip - base), x->ExceptionInformation[0] ? L"writing" : L"reading",
                     (unsigned long long)x->ExceptionInformation[1], GetCurrentThreadId());
  CONTEXT c = *e->ContextRecord;
  __try {
    for (int k = 0; k < 12 && n > 0 && n < 480; ++k) {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
      if (f) {
        void* hd = nullptr;
        DWORD64 fr = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &hd, &fr, nullptr);
      } else {
        c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
        c.Rsp += 8;
      }
      if (c.Rip < 0x10000) break;
      n += c.Rip >= base && c.Rip < ImageEnd()
               ? swprintf_s(line + n, 512 - n, L" rva:%llX", (unsigned long long)(c.Rip - base))
               : swprintf_s(line + n, 512 - n, L" abs:%llX", (unsigned long long)c.Rip);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  SowLog(L"%s", line);
  // The registers at the fault, and the first qword at each when it points into the image (a vtable names the object's class):
  // the client's crash at 11244A (session cm) checks a table at rbx+0x70 whose object could not be named from the stack alone.
  {
    const CONTEXT& r = *e->ContextRecord;
    const DWORD64 regs[] = {r.Rax, r.Rbx, r.Rcx, r.Rdx, r.Rsi, r.Rdi, r.R8, r.R9, r.R12, r.R13, r.R14, r.R15};
    const wchar_t* names[] = {L"rax", L"rbx", L"rcx", L"rdx", L"rsi", L"rdi", L"r8", L"r9", L"r12", L"r13", L"r14", L"r15"};
    wchar_t rl[900];
    int m = swprintf_s(rl, L"crash registers:");
    for (int k = 0; k < 12 && m > 0 && m < 800; ++k) {
      m += swprintf_s(rl + m, 900 - m, L" %s=%llX", names[k], (unsigned long long)regs[k]);
      uintptr_t q = 0;
      if (regs[k] > 0x10000 && ReadQ(regs[k], &q) && q >= base && q < ImageEnd())
        m += swprintf_s(rl + m, 900 - m, L"[rva:%llX]", (unsigned long long)(q - base));
    }
    SowLog(L"%s", rl);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void CrashLogInstall() {
  static PVOID handler = nullptr;
  if (!handler) handler = AddVectoredExceptionHandler(0, CrashLogHandler);
}

// Called by the pre-stub every time the hooked slot runs; must stay cheap.
void (*volatile g_frame_callback)() = nullptr;

void SimWork() {
  void (*const callback)() = g_frame_callback;
  if (!callback && !g_pending && !g_move_pending && !ExtraPending() && !InputsOn()) return;
  const DWORD me = GetCurrentThreadId(), sim = InputGameThread();
  if (!sim) return;  // the world has not started; nothing runs yet
  if (me != sim) {
    if (InterlockedIncrement(&g_wrong_thread_logged) == 1)
      SowLog(L"sim: hook ran on thread %lu, not the simulation thread %lu; work left queued", me, sim);
    return;
  }
  if (InputsOn()) RefreshInputOwner();
  if (callback) callback();
  if (g_pending) RunLkp(me);
  if (g_move_pending) RunPuppetMove(me);
  if (ExtraPending()) RunExtra();
  ApplyMirror();
}

}  // namespace

bool SimHookInstall(uintptr_t vtable_rva, unsigned slot, std::string* err) {
  void** vt = reinterpret_cast<void**>(const_cast<uint8_t*>(Base()) + vtable_rva);
  void* current = nullptr;
  if (!ReadPtr(&vt[slot], &current) || !IsGameCode(current)) {
    *err = "the slot does not point into the game's code";
    return false;
  }
  auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!page) {
    *err = "no memory for the stub";
    return false;
  }
  std::vector<uint8_t> s = {0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52,
                            0x41, 0x53, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x7F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x48, 0xB8});
  void* work = reinterpret_cast<void*>(&SimWork);
  const auto* w = reinterpret_cast<const uint8_t*>(&work);
  s.insert(s.end(), w, w + 8);
  s.insert(s.end(), {0xFF, 0xD0});
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x6F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
                     0x5A, 0x59, 0x58, 0x9D, 0xFF, 0x25, 0, 0, 0, 0});
  if (s.size() != kPreStubSize) {
    *err = "stub size mismatch";
    return false;
  }
  memcpy(page, s.data(), s.size());
  void* original = vt[slot];
  memcpy(page + 224, &original, 8);
  PutRel32(page + 124, page + 128, page + 224);
  memcpy(page + 160, kPreStubUnwind, sizeof kPreStubUnwind);
  auto* fn = reinterpret_cast<RUNTIME_FUNCTION*>(page + 192);
  fn->BeginAddress = 0;
  fn->EndAddress = (DWORD)kPreStubSize;
  fn->UnwindData = 160;
  DWORD old = 0;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  RtlAddFunctionTable(fn, 1, reinterpret_cast<DWORD64>(page));
#ifndef SOWGUARD_MUTANT  // negative control: the hook is never wired in
  if (!WriteSlot(&vt[slot], page)) {
    *err = "cannot write the vtable slot";
    return false;
  }
  InterlockedExchange(&g_installed, 1);
#endif
  CrashLogInstall();
  SowLog(L"sim: pre-hook on vtable rva:%llX slot %u (rva:%llX)", (unsigned long long)vtable_rva, slot,
         (unsigned long long)(static_cast<const uint8_t*>(original) - Base()));
  return true;
}

// 19C0380 starts: mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,40h;
// mov rax,[rip+<GameClient+0x18>] (another client global, read for a visibility check).
bool SimRequestLkp(uintptr_t lkp_rva, uintptr_t gameclient_rva, const float pos[3], std::string* err) {
  if (!g_installed) {
    *err = "simulation-thread hook not installed (sim_hook)";
    return false;
  }
  if (!InputGameThread()) {
    *err = "world not started yet (no GameModeSet seen)";
    return false;
  }
  static const uint8_t kHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                  0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x05};
  const uint8_t* base = Base();
  const uint8_t* f = base + lkp_rva;
  int32_t disp = 0;
  memcpy(&disp, f + 18, 4);
  if (memcmp(f, kHead, sizeof kHead) || f + 22 + disp != base + gameclient_rva + 0x18) {
    *err = "last-known-position function bytes differ from the known build; refused";
    return false;
  }
  EnterCriticalSection(&g_lock);
  g_lkp = reinterpret_cast<Lkp_t>(const_cast<uint8_t*>(f));
  g_player_global = reinterpret_cast<uintptr_t>(base) + gameclient_rva;
  memcpy(g_pos, pos, sizeof g_pos);
  InterlockedExchange(&g_pending, 1);
  LeaveCriticalSection(&g_lock);
  return true;
}

unsigned SimLkpShown() { return (unsigned)g_shown; }

bool SimHookInstalled() { return g_installed != 0; }

void SimSetFrameCallback(void (*callback)()) { g_frame_callback = callback; }

namespace {
size_t ScanPage(const uint64_t* p, uint64_t value, uintptr_t* out, size_t room);
}

// Only pages already in the game's working set are read: reading a committed page that
// was never touched makes Windows give it RAM (seen 2026-10-21: a full scan grew the game
// from ~2 GB to 6.3 GB).
size_t SimScanHeap(uint64_t value, uintptr_t* out, size_t room, size_t* resident_bytes) {
  ULONG_PTR stack_lo = 0, stack_hi = 0;  // this thread's stack holds value itself
  GetCurrentThreadStackLimits(&stack_lo, &stack_hi);
  std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(4096);
  size_t found = 0, resident = 0;
  MEMORY_BASIC_INFORMATION mbi;
  for (uintptr_t a = 0x10000; found < room && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi);
       a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & PAGE_GUARD) ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (start >= stack_lo && start < stack_hi))
      continue;
    for (uintptr_t chunk = start; chunk < start + mbi.RegionSize && found < room; chunk += pages.size() * 4096) {
      const size_t n = std::min<size_t>(pages.size(), (start + mbi.RegionSize - chunk) / 4096);
      for (size_t i = 0; i < n; ++i) pages[i].VirtualAddress = reinterpret_cast<void*>(chunk + i * 4096);
      if (!QueryWorkingSetEx(GetCurrentProcess(), pages.data(), (DWORD)(n * sizeof pages[0]))) continue;
      for (size_t i = 0; i < n && found < room; ++i) {
        if (!pages[i].VirtualAttributes.Valid) continue;
        resident += 4096;
        found += ScanPage(reinterpret_cast<const uint64_t*>(chunk + i * 4096), value, out + found, room - found);
      }
    }
  }
  if (resident_bytes) *resident_bytes = resident;
  return found;
}

namespace {

size_t ScanPage(const uint64_t* p, uint64_t value, uintptr_t* out, size_t room) {
  size_t found = 0;
  __try {
    for (size_t i = 0; i < 4096 / 8 && found < room; ++i)
      if (p[i] == value) out[found++] = reinterpret_cast<uintptr_t>(p + i);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return found;
}

bool ReadFloats(uintptr_t at, float* out, size_t n) {
  __try {
    memcpy(out, reinterpret_cast<const void*>(at), n * sizeof(float));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool InImage(uintptr_t p) {
  const uint8_t* base = Base();
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  return p >= reinterpret_cast<uintptr_t>(base) && p < reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
}

}  // namespace

bool SimSetPuppet(uintptr_t owner, std::string* err) {
  if (!owner) {
    EnterCriticalSection(&g_lock);
    g_puppet = 0;
    LeaveCriticalSection(&g_lock);
    SowLog(L"sim: no puppet");
    return true;
  }
  if (!g_installed) {
    *err = "simulation-thread hook not installed (sim_hook)";
    return false;
  }
  uintptr_t vt = 0, health = 0, back = 0;
  if (!ReadQ(owner, &vt) || !InImage(vt) || !ReadQ(owner + 0x26A8, &health) || !health ||
      !ReadQ(health + 0x28, &back) || back != owner) {
    *err = "not a character (no health object pointing back at it)";
    return false;
  }
  EnterCriticalSection(&g_lock);
  g_puppet = owner;
  g_puppet_vt = vt;
  g_puppet_health = health;
  LeaveCriticalSection(&g_lock);
  SowLog(L"sim: puppet is abs:%llX (class vtable rva:%llX)", (unsigned long long)owner,
         (unsigned long long)(vt - reinterpret_cast<uintptr_t>(Base())));
  return true;
}

namespace {
uintptr_t g_spawn_table_src = 0;  // an AI character whose spawn type leads to the spawn-type table
}  // namespace

bool SimPickNearestPuppet(uintptr_t gameclient_rva, std::string* report, bool make_puppet) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(Base());
  uintptr_t gc = 0, player = 0, health = 0, health_vt = 0;
  float ppos[3];
  if (!ReadQ(base + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &player) || !player ||
      !ReadQ(player + 0x26A8, &health) || !health || !ReadQ(health, &health_vt) || !ReadFloats(player + 0xAC, ppos, 3)) {
    *report = "no player (not in the world?)";
    return false;
  }
  std::vector<uintptr_t> hits(8192);
  const size_t n = SimScanHeap(health_vt, hits.data(), hits.size(), nullptr);
  uintptr_t best = 0;
  float best_d = 0, best_pos[3] = {}, best_hp = 0;
  unsigned characters = 0;
  for (size_t i = 0; i < n; ++i) {
    uintptr_t owner = 0, back = 0, vt = 0;
    float pos[3], hp[2];
    if (!ReadQ(hits[i] + 0x28, &owner) || !owner || owner == player || !ReadQ(owner + 0x26A8, &back) ||
        back != hits[i] || !ReadQ(owner, &vt) || !InImage(vt) || !ReadFloats(owner + 0xAC, pos, 3) ||
        !ReadFloats(hits[i] + 0x428, hp, 2) || !(hp[1] > 0))
      continue;
    ++characters;
    const float dx = pos[0] - ppos[0], dy = pos[1] - ppos[1], dz = pos[2] - ppos[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!best || d < best_d) {
      best = owner;
      best_d = d;
      memcpy(best_pos, pos, sizeof best_pos);
      best_hp = hp[1];
    }
  }
  char b[256];
  if (!best) {
    snprintf(b, sizeof b, "no living character found (%zu health objects, %u characters)", n, characters);
    *report = b;
    return false;
  }
  g_spawn_table_src = best;
  if (!make_puppet) {
    *report = "spawn-type table source found";
    return true;
  }
  std::string err;
  if (!SimSetPuppet(best, &err)) {
    *report = err;
    return false;
  }
  snprintf(b, sizeof b, "puppet abs:%llX distance=%.1f pos=%.1f,%.1f,%.1f health=%.0f (of %u characters)",
           (unsigned long long)best, best_d, best_pos[0], best_pos[1], best_pos[2], best_hp, characters);
  *report = b;
  return true;
}

// 19B7AC4: sub rsp,38h; mov r8,rdx; mov r9b,1; mov rdx,rcx; mov rcx,[rip+<manager>];
// call <queued teleport>; add rsp,38h; ret
bool TeleportBytesOk(const uint8_t* f) {
  static const uint8_t kHead[] = {0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC2, 0x41, 0xB1,
                                  0x01, 0x48, 0x8B, 0xD1, 0x48, 0x8B, 0x0D};
  static const uint8_t kTail[] = {0x48, 0x83, 0xC4, 0x38, 0xC3};
  return !memcmp(f, kHead, sizeof kHead) && f[20] == 0xE8 && !memcmp(f + 25, kTail, sizeof kTail);
}

bool SimRequestPuppetMove(uintptr_t teleport_rva, const float pos[3], const float rot[4], std::string* err,
                          bool direct) {
  if (!g_installed) {
    *err = "simulation-thread hook not installed (sim_hook)";
    return false;
  }
  if (!InputGameThread()) {
    *err = "world not started yet (no GameModeSet seen)";
    return false;
  }
  if (!g_puppet) {
    *err = "no puppet chosen";
    return false;
  }
  const uint8_t* f = Base() + teleport_rva;
  if (!TeleportBytesOk(f)) {
    *err = "teleport function bytes differ from the known build; refused";
    return false;
  }
  EnterCriticalSection(&g_lock);
  g_teleport = reinterpret_cast<Teleport_t>(const_cast<uint8_t*>(f));
  memcpy(g_move.pos, pos, sizeof g_move.pos);
  memcpy(g_move.rot, rot, sizeof g_move.rot);
  g_move_direct = direct;
  InterlockedExchange(&g_move_pending, 1);
  LeaveCriticalSection(&g_lock);
  return true;
}

bool SimHasPuppet() { return g_puppet != 0; }
uintptr_t SimPuppetOwner() { return g_puppet; }

std::string SimPuppetStatus() {
  char b[96];
  EnterCriticalSection(&g_lock);
  if (g_puppet) snprintf(b, sizeof b, "puppet abs:%llX moves=%ld", (unsigned long long)g_puppet, g_moves);
  else snprintf(b, sizeof b, "puppet none (moves=%ld)", g_moves);
  LeaveCriticalSection(&g_lock);
  return b;
}

// ---------------------------------------------------------------- near pages

namespace {

uint8_t* g_near[8] = {};
// The game's reference assign (listfix.h): unlink the reference, then link it into the new target's list and set the target.
constexpr uintptr_t kRefAssign = 0x3F52B8;
const char kRefAssignHead[] = "4883EC284C8BC24C8BD1483B5120";  // checked with Head (inside the image only)

uintptr_t ImageEnd() {
  const uint8_t* base = Base();
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  return reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
}

// A page within rel32 reach of every address in [lo, hi): below lo or above hi.
uint8_t* AllocNearImage(uintptr_t lo, uintptr_t hi) {
  const uintptr_t gran = 0x10000, reach = 0x7FF00000 - (hi - lo);
  const uintptr_t below = lo & ~(gran - 1), above = (hi + gran - 1) & ~(gran - 1);
  for (uintptr_t step = gran; step < reach; step += gran) {
    const uintptr_t candidates[2] = {below - step, above + step - gran};
    for (uintptr_t a : candidates) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
      void* p = VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (p) return static_cast<uint8_t*>(p);
    }
  }
  return nullptr;
}

}  // namespace

void SimReserveNearPages() {
  const uintptr_t lo = reinterpret_cast<uintptr_t>(Base()), hi = ImageEnd();
  int got = 0;
  for (auto& p : g_near) {
    if (!p) p = AllocNearImage(lo, hi);
    got += p != nullptr;
  }
  SowLog(L"sim: %d code page(s) reserved within reach of the game image", got);
}

uint8_t* SimNearPage(unsigned i) { return i < sizeof g_near / sizeof g_near[0] ? g_near[i] : nullptr; }

// ---------------------------------------------------------------- puppet AI and walking

namespace {

// The AI tick (2F79A4, slot 119 of the AI class) runs a character's behavior tree with
//   49 8B 08      mov rcx,[r8]      ; r8 = character+0x2D38; the tree's +8 is the character
//   33 D2         xor edx,edx
//   E8 <rel32>    call 1919C8
// That call is pointed at a jmp to BrainFilter, which leaves out the puppet's tree while its
// AI is off. The code after the call reads only rdx (2F5EB4), so a skipped call is safe.
const uint8_t kBrainSite[] = {0x49, 0x8B, 0x08, 0x33, 0xD2, 0xE8};
const uint8_t kTreeHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
using Tree_t = void(__fastcall*)(void* tree, uint8_t arg);
Tree_t g_tree = nullptr;
uint8_t* g_brain_call = nullptr;  // the E8
enum BrainState : LONG { kBrainNone = 0, kBrainQueued = 1, kBrainInstalled = 2, kBrainFailed = 3 };
volatile LONG g_brain_state = kBrainNone;
volatile LONG g_puppet_ai_off = 0;
volatile LONG g_brain_skips = 0;

// The enemy registry and the proxy set (end of this file): every AI character's tree passes through here once
// a tick, which is how the host learns which characters exist and how the client keeps its proxies' AI off.
void EnemyNote(uintptr_t owner);
bool ProxyHas(uintptr_t owner);
volatile LONG g_registry_on = 0;

volatile LONG g_brain_torn = 0;
// Characters whose tree ran with their component at +0x2CD8 present: one of them found without it is being taken apart. A
// character that never had it (none seen in the game; the guard tests' stand-in characters) is left alone.
SRWLOCK g_had_comp_lock = SRWLOCK_INIT;
std::vector<uintptr_t> g_had_comp;  // sorted

void __fastcall BrainFilter(void* tree, uint8_t arg) {
  // A character whose component at +0x2CD8 is gone (being taken apart) is not ticked: the tree writes into it (the host crashed at
  // 37671F writing [0+0x528], session cm; also 342E1A reading it). The code after the call reads only rdx.
  {
    uintptr_t ch = 0, comp = 0;
    if (ReadQ(reinterpret_cast<uintptr_t>(tree) + 8, &ch) && ch) {
      const bool has = ReadQ(ch + 0x2CD8, &comp) && comp;
      AcquireSRWLockShared(&g_had_comp_lock);
      const bool had = std::binary_search(g_had_comp.begin(), g_had_comp.end(), ch);
      ReleaseSRWLockShared(&g_had_comp_lock);
      if (has && !had) {
        AcquireSRWLockExclusive(&g_had_comp_lock);
        if (g_had_comp.size() >= 8192) g_had_comp.clear();  // addresses are reused: start again rather than grow
        g_had_comp.insert(std::lower_bound(g_had_comp.begin(), g_had_comp.end(), ch), ch);
        ReleaseSRWLockExclusive(&g_had_comp_lock);
      } else if (!has && had) {
        if (InterlockedIncrement(&g_brain_torn) <= 10)
          SowLog(L"sim: a behavior tree of abs:%llX skipped: its component at +0x2CD8 is gone", (unsigned long long)ch);
        return;
      }
    }
  }
  const uintptr_t puppet = g_puppet;
  uintptr_t owner = 0;
  const bool have = (g_puppet_ai_off || g_registry_on) && ReadQ(reinterpret_cast<uintptr_t>(tree) + 8, &owner) && owner;
  if (g_puppet_ai_off && puppet && have && owner == puppet) {
    InterlockedIncrement(&g_brain_skips);
    return;
  }
  if (have && g_registry_on) {
    if (ProxyHas(owner)) {
      InterlockedIncrement(&g_brain_skips);
      return;
    }
    EnemyNote(owner);
  }
  g_tree(tree, arg);
}

// On the simulation thread, so no call can be inside the patched bytes.
void InstallBrainNow() {
  uint8_t* page = SimNearPage(1);
  const int64_t rel = page ? reinterpret_cast<intptr_t>(page) - reinterpret_cast<intptr_t>(g_brain_call + 5) : 0;
  DWORD old = 0;
  if (!page || rel < INT32_MIN || rel > INT32_MAX || !VirtualProtect(page, 4096, PAGE_READWRITE, &old)) {
    InterlockedExchange(&g_brain_state, kBrainFailed);
    SowLog(L"sim: no code page near the image; puppet AI stays on");
    return;
  }
  const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
  memcpy(page, jmp, sizeof jmp);
  void* filter = reinterpret_cast<void*>(&BrainFilter);
  memcpy(page + 6, &filter, 8);
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 16);
  const int32_t rel32 = (int32_t)rel;
  if (!VirtualProtect(g_brain_call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
    InterlockedExchange(&g_brain_state, kBrainFailed);
    SowLog(L"sim: cannot patch the behavior-tree call; puppet AI stays on");
    return;
  }
  memcpy(g_brain_call + 1, &rel32, 4);
  VirtualProtect(g_brain_call + 1, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), g_brain_call, 5);
  InterlockedExchange(&g_brain_state, kBrainInstalled);
  SowLog(L"sim: behavior-tree call at rva:%llX goes through the puppet filter",
         (unsigned long long)(g_brain_call - Base()));
}

// 3F4BD8 SetMoveRequest(controller, request) starts: sub rsp,28h; mov dword [rcx+68h],1
// (mode 1: go to the request); mov al,[rdx]; mov [rcx+70h],al. Then it copies the request
// to +0x70; with direct 0 it also asks the character's navigator ([character+0x2528],
// vfunc +0x140) for a path, and the controller keeps mode 1 while the navigator has one
// (41B188); direct 1 is checked another way (41B2C0) and fell back to mode 2 when tried.
const uint8_t kSetMoveHead[] = {0x48, 0x83, 0xEC, 0x28, 0xC7, 0x41, 0x68, 0x01,
                                0x00, 0x00, 0x00, 0x8A, 0x02, 0x88, 0x41, 0x70};
struct MoveRequest {
  uint8_t has_dest, pad0[3];
  float dest[3];
  float dir[2];  // destination minus position, x and z
  int32_t type;  // 1 walk, 2 run
  float dist;    // horizontal distance
  float scale;   // 1 or [character+0x2CE8]+0x500 in the game's own requests; goes to the
                 // motion system with the gait (374CA0)
  uint8_t direct, pad1[3];  // 0: through the navigator (the game's go-to-point requests)
};
static_assert(sizeof(MoveRequest) == 0x28, "the game copies 0x25 bytes");
using SetMove_t = void(__fastcall*)(void* controller, const MoveRequest* r);
SetMove_t g_set_move = nullptr;
volatile LONG g_walks = 0, g_idles = 0;
volatile LONG g_walk_pending = 0;
float g_walk_dest[3] = {};
int g_walk_type = 0;
float g_walk_scale = 1.0f;

// The puppet's movement controller ([character+0x2C78], whose +0 is the character).
bool PuppetController(uintptr_t* controller, float pos[3]) {
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  uintptr_t c = 0, back = 0;
  if (!owner || !PuppetAlive(owner, vt, health) || !ReadQ(owner + 0x2C78, &c) || !c || !ReadQ(c, &back) ||
      back != owner || !ReadFloats(owner + 0xAC, pos, 3))
    return false;
  *controller = c;
  return true;
}

bool WalkNow(const float dest[3], int type, float scale) {
  uintptr_t c = 0;
  float pos[3];
  if (!PuppetController(&c, pos)) return false;
  if (type <= 0) {
    // What the game does when a move ends (3F4A79, 4E7272): back to mode 2.
    *reinterpret_cast<volatile int32_t*>(c + 0x68) = 2;
    InterlockedIncrement(&g_idles);
    return true;
  }
  MoveRequest r{};
  r.has_dest = 1;
  memcpy(r.dest, dest, sizeof r.dest);
  r.dir[0] = dest[0] - pos[0];
  r.dir[1] = dest[2] - pos[2];
  r.type = type;
  r.dist = std::sqrt(r.dir[0] * r.dir[0] + r.dir[1] * r.dir[1]);
  r.scale = scale;
  r.direct = 0;
  g_set_move(reinterpret_cast<void*>(c), &r);
  const LONG n = InterlockedIncrement(&g_walks);
  if (n <= 5 || n % 500 == 0)
    SowLog(L"sim: puppet move request #%ld to %.1f,%.1f,%.1f type %d scale %.2f (%.1f away)", n, dest[0], dest[1],
           dest[2], type, scale, r.dist);
  return true;
}

void RunQueuedWalk();

// Probe "teleport": this copy's player, keeping its rotation.
volatile LONG g_player_tp_pending = 0;
float g_player_tp[3] = {};
uintptr_t g_player_tp_global = 0;

void RunPlayerTeleport() {
  EnterCriticalSection(&g_lock);
  const bool run = InterlockedExchange(&g_player_tp_pending, 0) != 0;
  Xform x{};
  memcpy(x.pos, g_player_tp, sizeof x.pos);
  const uintptr_t global = g_player_tp_global;
  LeaveCriticalSection(&g_lock);
  uintptr_t gc = 0, player = 0;
  if (!run || !ReadQ(global, &gc) || !gc || !ReadQ(gc + 0x888, &player) || !player ||
      !ReadFloats(player + 0xB8, x.rot, 4))
    return;
  g_teleport(reinterpret_cast<void*>(player), &x);
  SowLog(L"sim: player teleported to %.1f,%.1f,%.1f", x.pos[0], x.pos[1], x.pos[2]);
}

bool SpawnPending();
void RunSpawnNow();
}  // namespace
bool AnimResPending();
void RunAnimRes();
bool TracePending();
void RunTrace();
bool HoldPending();
void RunHold();
bool AttackPending();
void RunAttack();
bool PlayPending();
void RunPlay();
bool MovePending();
void RunMove();
namespace {

// "AI told to despawn offscreen" (1A8BB74(character, int, nothing)): sets the character's despawn request
// (+0x910 of its AI data at +0x2CE8: 1, offscreen 1, the int) and logs; it goes once it is out of sight.
volatile LONG g_despawn_pending = 0;
uintptr_t g_despawn_fn = 0, g_despawn_char = 0;

bool DespawnPending() { return g_despawn_pending != 0; }

void RunDespawn() {
  if (!InterlockedExchange(&g_despawn_pending, 0)) return;
  using Despawn_t = void(__fastcall*)(void* ai_character, int arg, void* other);
  reinterpret_cast<Despawn_t>(g_despawn_fn)(reinterpret_cast<void*>(g_despawn_char), 0, nullptr);
  SowLog(L"sim: abs:%llX told to despawn offscreen", (unsigned long long)g_despawn_char);
}

// Starts a slow-motion record by name: 7CE80C(const char* record) finds the record (a binary search of the record
// table by name hash; the SlowMO.Start console program stops there), 5D09E0(this unused, record) sets it as the world
// UI's slow motion ("SetWorldUISlowMo = ..." in the log). Queued by the probe's "slowmo start", run on the simulation
// thread.
volatile LONG g_slowmo_start_pending = 0;
uintptr_t g_slowmo_start_fn = 0, g_slowmo_set_fn = 0;
char g_slowmo_start_name[96] = {};

bool SlowMoStartPending() { return g_slowmo_start_pending != 0; }

void RunSlowMoStart() {
  if (!InterlockedExchange(&g_slowmo_start_pending, 0)) return;
  using Find_t = void*(__fastcall*)(const char* record);
  using Set_t = void(__fastcall*)(void* unused, void* record);
  void* const rec = reinterpret_cast<Find_t>(g_slowmo_start_fn)(g_slowmo_start_name);
  if (rec) reinterpret_cast<Set_t>(g_slowmo_set_fn)(nullptr, rec);
  SowLog(L"sim: slow motion record %S %S", g_slowmo_start_name, rec ? "requested (as the world UI's)" : "not found");
}

// Work other threads ask the simulation thread to do (SimCall): run at the next frame, in order.
std::vector<std::pair<std::function<void()>, HANDLE>> g_tasks;
volatile LONG g_task_count = 0;

bool TasksPending() { return g_task_count != 0; }

void RunTasks() {
  std::vector<std::pair<std::function<void()>, HANDLE>> todo;
  EnterCriticalSection(&g_lock);
  todo.swap(g_tasks);
  g_task_count = 0;
  LeaveCriticalSection(&g_lock);
  for (auto& t : todo) {
    t.first();
    if (t.second) SetEvent(t.second);
  }
}

bool ExtraPending() {
  return TasksPending() || SlowMoStartPending() || DespawnPending() || g_brain_state == kBrainQueued || g_walk_pending || g_player_tp_pending || SpawnPending() ||
         AnimResPending() || TracePending() || HoldPending() || AttackPending() || PlayPending() ||
         MovePending();
}

void RunExtra() {
  if (TasksPending()) RunTasks();
  if (SlowMoStartPending()) RunSlowMoStart();
  if (DespawnPending()) RunDespawn();
  if (g_brain_state == kBrainQueued) InstallBrainNow();
  if (g_walk_pending) RunQueuedWalk();
  if (g_player_tp_pending) RunPlayerTeleport();
  if (SpawnPending()) RunSpawnNow();
  if (AnimResPending()) RunAnimRes();
  if (TracePending()) RunTrace();
  if (HoldPending()) RunHold();
  if (AttackPending()) RunAttack();
  if (PlayPending()) RunPlay();
  if (MovePending()) RunMove();
}

void RunQueuedWalk() {
  EnterCriticalSection(&g_lock);
  const bool run = InterlockedExchange(&g_walk_pending, 0) != 0;
  float dest[3];
  memcpy(dest, g_walk_dest, sizeof dest);
  const int type = g_walk_type;
  const float scale = g_walk_scale;
  LeaveCriticalSection(&g_lock);
  if (run) WalkNow(dest, type, scale);
}

int32_t PuppetMode() {
  uintptr_t c = 0;
  float pos[3];
  int32_t mode = -1;
  if (PuppetController(&c, pos)) {
    __try {
      mode = *reinterpret_cast<const int32_t*>(c + 0x6C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  return mode;
}

}  // namespace

bool SimRequestBrainHook(uintptr_t site_rva, uintptr_t tree_rva, std::string* err) {
  if (g_brain_state == kBrainQueued || g_brain_state == kBrainInstalled) return true;
  if (!g_installed) {
    *err = "simulation-thread hook not installed (sim_hook)";
    return false;
  }
  const uint8_t* site = Base() + site_rva;
  const uint8_t* tree = Base() + tree_rva;
  int32_t rel = 0;
  memcpy(&rel, site + sizeof kBrainSite, 4);
  if (memcmp(site, kBrainSite, sizeof kBrainSite) || site + sizeof kBrainSite + 4 + rel != tree ||
      memcmp(tree, kTreeHead, sizeof kTreeHead)) {
    *err = "behavior-tree call bytes differ from the known build; refused";
    return false;
  }
  g_tree = reinterpret_cast<Tree_t>(const_cast<uint8_t*>(tree));
  g_brain_call = const_cast<uint8_t*>(site + 5);
  InterlockedExchange(&g_brain_state, kBrainQueued);
  return true;
}

void SimSetPuppetAi(bool on) {
  if ((g_puppet_ai_off != 0) == !on) return;
  InterlockedExchange(&g_puppet_ai_off, on ? 0 : 1);
  SowLog(L"sim: puppet AI %s", on ? L"on" : L"off (its behavior tree is skipped)");
}

bool SimPuppetWalk(uintptr_t request_rva, const float dest[3], int type, float scale, std::string* err) {
  if (!g_installed) {
    *err = "simulation-thread hook not installed (sim_hook)";
    return false;
  }
  if (!g_puppet) {
    *err = "no puppet chosen";
    return false;
  }
  const uint8_t* f = Base() + request_rva;
  if (memcmp(f, kSetMoveHead, sizeof kSetMoveHead)) {
    *err = "move-request function bytes differ from the known build; refused";
    return false;
  }
  g_set_move = reinterpret_cast<SetMove_t>(const_cast<uint8_t*>(f));
  if (GetCurrentThreadId() == InputGameThread()) {
    if (!WalkNow(dest, type, scale)) {
      *err = "puppet gone";
      return false;
    }
    return true;
  }
  EnterCriticalSection(&g_lock);
  memcpy(g_walk_dest, dest, sizeof g_walk_dest);
  g_walk_type = type;
  g_walk_scale = scale;
  InterlockedExchange(&g_walk_pending, 1);
  LeaveCriticalSection(&g_lock);
  return true;
}

bool SimRequestPlayerTeleport(uintptr_t teleport_rva, uintptr_t gameclient_rva, const float pos[3], std::string* err) {
  if (!g_installed) {
    *err = "simulation-thread hook not installed (sim_hook)";
    return false;
  }
  if (!InputGameThread()) {
    *err = "world not started yet (no GameModeSet seen)";
    return false;
  }
  const uint8_t* f = Base() + teleport_rva;
  if (!TeleportBytesOk(f)) {
    *err = "teleport function bytes differ from the known build; refused";
    return false;
  }
  EnterCriticalSection(&g_lock);
  g_teleport = reinterpret_cast<Teleport_t>(const_cast<uint8_t*>(f));
  g_player_tp_global = reinterpret_cast<uintptr_t>(Base()) + gameclient_rva;
  memcpy(g_player_tp, pos, sizeof g_player_tp);
  InterlockedExchange(&g_player_tp_pending, 1);
  LeaveCriticalSection(&g_lock);
  return true;
}

bool SimPuppetPos(float pos[3]) {
  uintptr_t c = 0;
  return PuppetController(&c, pos);
}

bool SimPuppetXform(float pos[3], float rot[4]) {
  uintptr_t c = 0;
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet;
  LeaveCriticalSection(&g_lock);
  return PuppetController(&c, pos) && ReadFloats(owner + 0xB8, rot, 4);
}

std::string SimPuppetDetail() {
  float pos[3] = {};
  const bool have = SimPuppetPos(pos);
  const LONG state = g_brain_state;
  char b[256];
  snprintf(b, sizeof b, "walks=%ld idles=%ld ai=%s tree_skips=%ld torn_skips=%ld tree_hook=%s mode=%d pos=%.1f,%.1f,%.1f", g_walks,
           g_idles, g_puppet_ai_off ? "off" : "on", g_brain_skips, g_brain_torn,
           state == kBrainInstalled ? "in" : state == kBrainQueued ? "queued" : state == kBrainFailed ? "failed" : "none",
           have ? PuppetMode() : -1, pos[0], pos[1], pos[2]);
  return b;
}

// ---------------------------------------------------------------- write watch (probe "spawn ... watch")

namespace {

// A hardware write watch (debug register 0, 1 byte) on the simulation thread: the first
// writes are recorded with their instruction and call stack, then the watch clears itself.
constexpr int kWatchHits = 16, kWatchDepth = 14;
struct WatchHit {
  uintptr_t frames[kWatchDepth];
  int n;
  uint8_t value;
};
WatchHit g_watch_hits[kWatchHits];
volatile LONG g_watch_count = 0;
uintptr_t g_watch_addr = 0;
PVOID g_watch_handler = nullptr;

// Probe "watchw": up to four addresses (debug registers 0-3) on every thread of the game; which one a hit was is kept.
uintptr_t g_watch_multi[4] = {};
int g_watch_slot[kWatchHits] = {};
DWORD g_watch_thread[kWatchHits] = {};

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
  const DWORD64 dr6 = e->ContextRecord->Dr6;
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(dr6 & 0xF) || !g_watch_addr)
    return EXCEPTION_CONTINUE_SEARCH;
  const LONG i = InterlockedIncrement(&g_watch_count) - 1;
  if (i < kWatchHits) {
    WatchHit& h = g_watch_hits[i];
    g_watch_slot[i] = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3;
    g_watch_thread[i] = GetCurrentThreadId();
    CONTEXT c = *e->ContextRecord;
    h.n = 0;
    h.frames[h.n++] = c.Rip;  // the instruction after the write
    __try {
      const uintptr_t at = g_watch_multi[g_watch_slot[i]] ? g_watch_multi[g_watch_slot[i]] : g_watch_addr;
      h.value = *reinterpret_cast<const uint8_t*>(at);
      for (int k = 0; k < 40 && h.n < kWatchDepth; ++k) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (f) {
          void* hd = nullptr;
          DWORD64 fr = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &hd, &fr, nullptr);
        } else {
          c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
          c.Rsp += 8;
        }
        if (c.Rip < 0x10000) break;
        h.frames[h.n++] = c.Rip;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  e->ContextRecord->Dr6 = 0;
  if (i + 1 >= kWatchHits) e->ContextRecord->Dr7 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// On the thread to be watched.
void ArmWatch(uintptr_t addr) {
  if (!g_watch_handler) g_watch_handler = AddVectoredExceptionHandler(1, WatchHandler);
  g_watch_addr = addr;
  InterlockedExchange(&g_watch_count, 0);
  CONTEXT c{};
  c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  c.Dr0 = addr;
  c.Dr7 = 1 | (1u << 16);  // local enable 0, break on write, length 1
  SetThreadContext(GetCurrentThread(), &c);
  SowLog(L"sim: write watch on abs:%llX (simulation thread)", (unsigned long long)addr);
}

}  // namespace

// Arms (addrs non-empty) or clears (empty) write watches of len bytes (1, 2, 4 or 8) on every thread but the caller's.
std::string SimWatchWrites(const std::vector<uintptr_t>& addrs, int len) {
  if (addrs.size() > 4) return "err at most four addresses\n";
  if (!g_watch_handler) g_watch_handler = AddVectoredExceptionHandler(1, WatchHandler);
  const DWORD64 len_bits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
  DWORD64 dr7 = 0;
  uintptr_t dr[4] = {};
  for (size_t k = 0; k < addrs.size(); ++k) {
    dr[k] = addrs[k];
    dr7 |= (1ull << (2 * k)) | (1ull << (16 + 4 * k)) | (len_bits << (18 + 4 * k));  // local enable, break on write, length
  }
  for (int k = 0; k < 4; ++k) g_watch_multi[k] = dr[k];
  g_watch_addr = addrs.empty() ? 0 : addrs[0];
  InterlockedExchange(&g_watch_count, 0);
  const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return "err no thread list\n";
  THREADENTRY32 te{};
  te.dwSize = sizeof te;
  int armed = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
    if (!t) continue;
    if (SuspendThread(t) != (DWORD)-1) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(t, &c)) {
        c.Dr0 = dr[0];
        c.Dr1 = dr[1];
        c.Dr2 = dr[2];
        c.Dr3 = dr[3];
        c.Dr6 = 0;
        c.Dr7 = dr7;
        if (SetThreadContext(t, &c)) ++armed;
      }
      ResumeThread(t);
    }
    CloseHandle(t);
  }
  CloseHandle(snap);
  char b[96];
  snprintf(b, sizeof b, "ok %s on %d threads\n", addrs.empty() ? "watches cleared" : "write watches armed", armed);
  return b;
}

std::string SimWatchReport() {
  const LONG n = g_watch_count;
  char b[96];
  snprintf(b, sizeof b, "ok watch abs:%llX hits=%ld\n", (unsigned long long)g_watch_addr, n);
  std::string out = b;
  const uintptr_t base = reinterpret_cast<uintptr_t>(Base()), end = ImageEnd();
  for (LONG i = 0; i < n && i < kWatchHits; ++i) {
    const WatchHit& h = g_watch_hits[i];
    snprintf(b, sizeof b, "hit slot=%d at abs:%llX thread=%lu value=%u\n ", g_watch_slot[i],
             (unsigned long long)(g_watch_multi[g_watch_slot[i]] ? g_watch_multi[g_watch_slot[i]] : g_watch_addr), g_watch_thread[i],
             h.value);
    out += b;
    for (int k = 0; k < h.n; ++k) {
      const uintptr_t f = h.frames[k];
      if (f >= base && f < end) snprintf(b, sizeof b, " rva:%llX", (unsigned long long)(f - base));
      else snprintf(b, sizeof b, " abs:%llX", (unsigned long long)f);
      out += b;
    }
    out += "\n";
  }
  return out;
}

// ---------------------------------------------------------------- spawn (probe "spawn")

namespace {

// The game's queued spawn requests (1A9290C and its neighbours) create a character with
//   obj = 1036D0([[26FFAB0]+0xF58], name, &transform, record, 1, 1, 0, 0, 0)
// and, when it is a character (274A04 walks its class chain), store the region at its
// position (+0x26F0 = 168DA8(obj)) and the area (35DC38(obj, 607610(obj), 0)). The record
// is a spawnable character type (type 0xA06, e.g. "AI_Talion"; the same names also exist
// as appearance records, type 0xA66, which the spawn does not take). An AI character keeps
// its own spawn type at +0x2E68, whose +0x18 is the table: count +0x28, an array at +0x38
// of 40-byte entries with the name hash at +4 (the game's hash, 423E70).
using Hash_t = uint32_t(__fastcall*)(const char* name, void* unused, uint32_t seed);
using Spawn_t = void*(__fastcall*)(void* ctx, const char* name, const float* xform, void* record, uint8_t a5,
                                   uint8_t a6, void* a7, void* a8, void* a9);
using IsCharacter_t = int(__fastcall*)(void* obj);
// Called by the game with rdx = r8 = 0, which they pass on (1A9290C: xor edx,edx; xor r8d,r8d).
using AtPosition_t = void*(__fastcall*)(void* obj, void* zero1, void* zero2);
using SetArea_t = void(__fastcall*)(void* obj, void* area, uint8_t force);
// 10AFB0: the character set-up the spawner path (1045EC) and the player's model swap
// (19BB3CC) both end with.
using Setup_t = void(__fastcall*)(void* obj);

struct SpawnFns {
  Hash_t hash;
  Spawn_t spawn;
  IsCharacter_t is_character;
  AtPosition_t region, area;
  SetArea_t set_area;
  Setup_t setup;
  uintptr_t world_global;
  uintptr_t type_table_global;  // [2702440], or 0
};

bool Head(const uint8_t* f, const char* hex) {
  // Outside the image (a smaller exe, as in the tests) there is nothing to compare.
  const uintptr_t a = reinterpret_cast<uintptr_t>(f);
  if (a < reinterpret_cast<uintptr_t>(Base()) || a + strlen(hex) / 2 > ImageEnd()) return false;
  for (size_t i = 0; hex[2 * i]; ++i) {
    char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
    if (f[i] != (uint8_t)strtoul(b, nullptr, 16)) return false;
  }
  return true;
}

const uint8_t* Rel32Target(const uint8_t* call) {
  int32_t rel = 0;
  memcpy(&rel, call + 1, 4);
  return call + 5 + rel;
}

// The entry of type kind with name hash h in a record table (count +0x28, array +0x38), or null.
void* SearchTypeTable(uintptr_t table, uint32_t kind, uint32_t h) {
  uintptr_t array = 0;
  uint32_t count = 0;
  if (!table || !ReadQ(table + 0x38, &array) || !array ||
      !ReadFloats(table + 0x28, reinterpret_cast<float*>(&count), 1) || count == 0 || count > 100000)
    return nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    const uintptr_t entry = array + 40ull * i;
    uint32_t head[2] = {};
    if (!ReadFloats(entry, reinterpret_cast<float*>(head), 2)) return nullptr;
    if (head[1] == h && head[0] == kind) return reinterpret_cast<void*>(entry);
  }
  return nullptr;
}

// The spawn type named name. The table (type 0xA06) is held by a global ([2702440]; the game's
// queued spawns read it, 1A9212F), or found through an AI character (the puppet or the one
// remembered for it): its spawn type at +0x2E68, whose +0x18 is the table. Only the second
// worked once this copy's player had died: the game freed the puppet when it put the player
// back at the tower, and no AI character was loaded there to find the table through, so no
// stand-in was spawned again (session ar).
void* FindSpawnType(Hash_t hash, const char* name, uintptr_t table_global) {
  const uint32_t h = hash(name, nullptr, 0);
  uintptr_t table = 0;
  if (table_global && ReadQ(table_global, &table))
    if (void* found = SearchTypeTable(table, 0xA06, h)) return found;
  const uintptr_t owner = g_puppet ? g_puppet : g_spawn_table_src;
  uintptr_t own_type = 0;
  uint32_t kind = 0;
  if (!owner || !ReadQ(owner + 0x2E68, &own_type) || !own_type || !ReadQ(own_type + 0x18, &table) ||
      !ReadFloats(own_type, reinterpret_cast<float*>(&kind), 1))
    return nullptr;
  return SearchTypeTable(table, kind, h);
}

// Appearance override for one spawn. The character set-up at creation (3FAAEC, from the
// AI class's slot 12) asks for the spawn type's appearance record (type 0xA66) and applies
// it (vfunc +0x440) before building the model from it:
//   48 8B 0E      mov rcx,[rsi]         ; the spawn type
//   48 85 C9 74 14 48 8B 1F
//   E8 <rel32>    call 16680C           ; its appearance
//   48 8B D0 48 8B CF FF 93 40 04 00 00 ; mov rdx,rax; mov rcx,rdi; call [rbx+440h]
// That call is pointed at a jmp to AppearanceFilter (the code after it reads only rax and
// saved registers), which answers with the override while our spawn runs. Appearance
// records are looked up by name with 1B08BB0 (the table Player.SetCharacterModelOverride uses).
using TypeAppearance_t = void*(__fastcall*)(void* type);
using AppearanceLookup_t = void*(__fastcall*)(const char* name);
TypeAppearance_t g_type_appearance = nullptr;
void* volatile g_override_type = nullptr;
void* volatile g_override_appearance = nullptr;
volatile LONG g_appearance_used = 0;
uint8_t* g_appearance_call = nullptr;  // the E8, once patched

void* __fastcall AppearanceFilter(void* type) {
  if (type && type == g_override_type && g_override_appearance) {
    InterlockedIncrement(&g_appearance_used);
    return g_override_appearance;
  }
  return g_type_appearance(type);
}

// On the simulation thread (RunSpawn), so no creation is inside the patched call.
bool InstallAppearanceNow(uint8_t* call) {
  if (g_appearance_call) return true;
  uint8_t* page = SimNearPage(2);
  const int64_t rel = page ? reinterpret_cast<intptr_t>(page) - reinterpret_cast<intptr_t>(call + 5) : 0;
  DWORD old = 0;
  if (!page || rel < INT32_MIN || rel > INT32_MAX || !VirtualProtect(page, 4096, PAGE_READWRITE, &old)) return false;
  g_type_appearance = reinterpret_cast<TypeAppearance_t>(const_cast<uint8_t*>(Rel32Target(call)));
  const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
  memcpy(page, jmp, sizeof jmp);
  void* filter = reinterpret_cast<void*>(&AppearanceFilter);
  memcpy(page + 6, &filter, 8);
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 16);
  const int32_t rel32 = (int32_t)rel;
  if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(call + 1, &rel32, 4);
  VirtualProtect(call + 1, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), call, 5);
  g_appearance_call = call;
  SowLog(L"sim: appearance lookup at rva:%llX goes through the spawn override", (unsigned long long)(call - Base()));
  return true;
}

volatile LONG g_spawn_pending = 0, g_spawn_done = 0;
char g_spawn_record[64] = {};
char g_spawn_look[64] = {};
bool g_spawn_watch = false;  // arm a write watch on the new character's +0x19A8
uint8_t* g_spawn_look_site = nullptr;
AppearanceLookup_t g_appearance_lookup = nullptr;
Xform g_spawn_at{};
SpawnFns g_spawn_fns{};
uintptr_t g_spawned = 0;
uintptr_t g_spawn_member = 0;  // the FactionMember the spawn is made from (0: none)
std::string g_spawn_report;

void RunSpawn();
bool SpawnPending() { return g_spawn_pending != 0; }
void RunSpawnNow() { RunSpawn(); }

void RunSpawn() {
  EnterCriticalSection(&g_lock);
  const bool run = InterlockedExchange(&g_spawn_pending, 0) != 0;
  char name[64], look[64];
  memcpy(name, g_spawn_record, sizeof name);
  memcpy(look, g_spawn_look, sizeof look);
  const Xform at = g_spawn_at;
  const SpawnFns f = g_spawn_fns;
  void* const member = reinterpret_cast<void*>(g_spawn_member);
  LeaveCriticalSection(&g_lock);
  if (!run) return;
  char b[320];
  uintptr_t world = 0, ctx = 0, obj = 0;
  void* record = FindSpawnType(f.hash, name, f.type_table_global);
  void* appearance = !look[0]                             ? nullptr
                     : strncmp(look, "abs:", 4) == 0 ? reinterpret_cast<void*>(strtoull(look + 4, nullptr, 16))
                                                     : g_appearance_lookup(look);
  const LONG used0 = g_appearance_used;
  if (look[0] && !appearance) {
    snprintf(b, sizeof b, "no appearance named %s", look);
  } else if (look[0] && !InstallAppearanceNow(g_spawn_look_site)) {
    snprintf(b, sizeof b, "cannot hook the appearance lookup");
  } else if (!record) {
    snprintf(b, sizeof b, "no spawnable character type named %s (or no AI character to find the table from)", name);
  } else if (!ReadQ(f.world_global, &world) || !world || !ReadQ(world + 0xF58, &ctx) || !ctx) {
    snprintf(b, sizeof b, "no spawn context (world not ready)");
  } else {
    g_override_appearance = appearance;
    g_override_type = appearance ? record : nullptr;
    obj = reinterpret_cast<uintptr_t>(f.spawn(reinterpret_cast<void*>(ctx), "CoopPuppet", at.pos, record, 1, 1,
                                              nullptr, member, nullptr));
    g_override_type = nullptr;
    g_override_appearance = nullptr;
    uintptr_t active_q = 0;
    const int active_after_spawn = obj && ReadQ(obj + 0x19A8, &active_q) ? (int)(active_q & 0xFF) : -1;
    if (!obj) {
      snprintf(b, sizeof b, "the game created nothing for %s", name);
    } else if (!f.is_character(reinterpret_cast<void*>(obj))) {
      snprintf(b, sizeof b, "created abs:%llX for %s, not a character", (unsigned long long)obj, name);
    } else {
      void* region = f.region(reinterpret_cast<void*>(obj), nullptr, nullptr);
      *reinterpret_cast<void**>(obj + 0x26F0) = region;
      void* area = f.area(reinterpret_cast<void*>(obj), nullptr, nullptr);
      if (area) f.set_area(reinterpret_cast<void*>(obj), area, 0);
      f.setup(reinterpret_cast<void*>(obj));
      uintptr_t active_q2 = 0;
      const int active_after_setup = ReadQ(obj + 0x19A8, &active_q2) ? (int)(active_q2 & 0xFF) : -1;
      if (g_spawn_watch) ArmWatch(obj + 0x19A8);
      uintptr_t worn = 0, worn_name = 0;
      char worn_text[64] = "?";
      if (ReadQ(obj + 0x2B30, &worn) && worn && ReadQ(worn + 0x20, &worn_name) && worn_name) {
        __try {
          strncpy_s(worn_text, reinterpret_cast<const char*>(worn_name), _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
      }
      snprintf(b, sizeof b,
               "spawned %s as abs:%llX at %.1f,%.1f,%.1f wearing %s (appearance overrides %ld; active +0x19A8 after "
               "spawn %d, after set-up %d; region %p, area %p)",
               name, (unsigned long long)obj, at.pos[0], at.pos[1], at.pos[2], worn_text, g_appearance_used - used0,
               active_after_spawn, active_after_setup, region, area);
    }
  }
  SowLog(L"sim: %S", b);
  EnterCriticalSection(&g_lock);
  g_spawned = obj;
  g_spawn_report = b;
  LeaveCriticalSection(&g_lock);
  InterlockedExchange(&g_spawn_done, 1);
}

}  // namespace

bool SimSpawn(const SimSpawnRvas& r, const char* record, const float* pos, const char* look, std::string* report,
              uintptr_t* spawned, bool run_now, uintptr_t member) {
  if (!g_installed) {
    *report = "simulation-thread hook not installed (sim_hook)";
    return false;
  }
  if (!InputGameThread()) {
    *report = "world not started yet (no GameModeSet seen)";
    return false;
  }
  const uint8_t* base = Base();
  const uint8_t *hash = base + r.hash_rva, *spawn = base + r.spawn_rva, *is_char = base + r.is_character_rva,
                *region = base + r.region_rva, *area = base + r.area_rva, *set_area = base + r.set_area_rva,
                *setup = base + r.setup_rva;
  // 168DA8 and 607610 share their first 33 bytes; their call targets tell them apart.
  if (!Head(hash, "0FB60184C074274C8D0D") || !Head(spawn, "488BC448895808488970104889781855") ||
      !Head(is_char, "33C04885C9742D488B49784885C97405") || !Head(region, "4883EC38F20F1081AC0000008B81B400") ||
      !Head(area, "4883EC38F20F1081AC0000008B81B400") || region[0x21] != 0xE8 || area[0x21] != 0xE8 ||
      Rel32Target(region + 0x21) != base + r.region_rva + 0x11C || Rel32Target(area + 0x21) != base + r.area_rva + 0x2C ||
      !Head(set_area, "40534883EC204C8BCA488BD94584C074") || !Head(setup, "48894C2408555356574154415541564157")) {
    *report = "spawn function bytes differ from the known build; refused";
    return false;
  }
  const uint8_t* look_site = base + r.appearance_site_rva;
  const uint8_t* look_up = base + r.appearance_lookup_rva;
  // Before our patch the call goes to the type-appearance function; after it, to our page.
  if (look && (!Head(look_site, "488B0E4885C97414488B1FE8") || !Head(look_site + 16, "488BD0488BCFFF9340040000") ||
               !Head(look_up, "4883EC284533C0E8") ||
               (!g_appearance_call && !Head(Rel32Target(look_site + 11), "4883EC284C8BD1E8")))) {
    *report = "spawn function bytes differ from the known build; refused";
    return false;
  }
  if (strlen(record) >= sizeof g_spawn_record) {
    *report = "record name too long";
    return false;
  }
  uintptr_t gc = 0, player = 0;
  Xform at{};
  if (!ReadQ(reinterpret_cast<uintptr_t>(base) + r.gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &player) ||
      !player || !ReadFloats(player + 0xB8, at.rot, 4)) {
    *report = "no player";
    return false;
  }
  if (pos) {
    memcpy(at.pos, pos, sizeof at.pos);
  } else if (!ReadFloats(player + 0xAC, at.pos, 3)) {
    *report = "no player position";
    return false;
  }
  EnterCriticalSection(&g_lock);
  g_spawn_fns = SpawnFns{reinterpret_cast<Hash_t>(const_cast<uint8_t*>(hash)),
                         reinterpret_cast<Spawn_t>(const_cast<uint8_t*>(spawn)),
                         reinterpret_cast<IsCharacter_t>(const_cast<uint8_t*>(is_char)),
                         reinterpret_cast<AtPosition_t>(const_cast<uint8_t*>(region)),
                         reinterpret_cast<AtPosition_t>(const_cast<uint8_t*>(area)),
                         reinterpret_cast<SetArea_t>(const_cast<uint8_t*>(set_area)),
                         reinterpret_cast<Setup_t>(const_cast<uint8_t*>(setup)),
                         reinterpret_cast<uintptr_t>(base) + r.world_rva,
                         r.spawn_table_rva && reinterpret_cast<uintptr_t>(base) + r.spawn_table_rva + 8 <= ImageEnd()
                             ? reinterpret_cast<uintptr_t>(base) + r.spawn_table_rva
                             : 0};
  strcpy_s(g_spawn_record, record);
  g_spawn_look[0] = 0;
  g_spawn_watch = false;
  if (look && strcmp(look, "watch") == 0) {
    g_spawn_watch = true;
    look = nullptr;
  }
  if (look) strncpy_s(g_spawn_look, look, _TRUNCATE);
  g_spawn_look_site = const_cast<uint8_t*>(look_site + 11);
  g_appearance_lookup = reinterpret_cast<AppearanceLookup_t>(const_cast<uint8_t*>(look_up));
  g_spawn_at = at;
  g_spawn_member = member;
  InterlockedExchange(&g_spawn_done, 0);
  InterlockedExchange(&g_spawn_pending, 1);
  LeaveCriticalSection(&g_lock);
  if (run_now) RunSpawn();  // the caller is the simulation thread: no one else could run it
  else for (int i = 0; i < 100 && !g_spawn_done; ++i) Sleep(20);
  if (!g_spawn_done && InterlockedExchange(&g_spawn_pending, 0) != 0) {
    // The simulation thread never took it. Withdrawn, so a late run does not spawn a character that nobody
    // uses (the next attempt would spawn another).
    *report = "queued; the simulation thread has not run it yet";
    return false;
  }
  // It was taken: wait for it to end, however long the spawn takes.
  for (int i = 0; i < 500 && !g_spawn_done; ++i) Sleep(20);
  EnterCriticalSection(&g_lock);
  *report = g_spawn_done ? g_spawn_report : std::string("the spawn did not finish");
  const bool ok = g_spawn_done && g_spawned != 0;
  if (spawned) *spawned = ok ? g_spawned : 0;
  LeaveCriticalSection(&g_lock);
  return ok;
}

// ---------------------------------------------------------------- hybrid appearance (probe "look")

// An appearance record (type 0xA66) is a 40-byte entry whose +0x10 points at a fixed-layout
// field block (the same offset holds the same field in every record of the type; +0x148 is
// the model, type 0xA19). A hybrid is a private copy of one record's entry and block with
// some fields taken from another record. Never freed: a character built from it keeps
// pointing at it.
bool SimMakeLook(uintptr_t lookup_rva, const char* base_name, const std::vector<unsigned>& offsets,
                 const char* donor_name, std::string* report) {
  const uint8_t* lookup = Base() + lookup_rva;
  if (!Head(lookup, "4883EC284533C0E8")) {
    *report = "appearance lookup bytes differ from the known build; refused";
    return false;
  }
  auto find = reinterpret_cast<AppearanceLookup_t>(const_cast<uint8_t*>(lookup));
  const uintptr_t base = reinterpret_cast<uintptr_t>(find(base_name));
  const uintptr_t donor = reinterpret_cast<uintptr_t>(find(donor_name));
  uintptr_t base_data = 0, donor_data = 0;
  if (!base || !donor || !ReadQ(base + 0x10, &base_data) || !base_data || !ReadQ(donor + 0x10, &donor_data) ||
      !donor_data) {
    *report = std::string("no appearance named ") + (base ? donor_name : base_name);
    return false;
  }
  constexpr size_t kBlock = 0x600;
  auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!mem) {
    *report = "no memory";
    return false;
  }
  uint8_t* data = mem + 0x100;
  for (size_t i = 0; i < 40; i += 8) {
    uintptr_t q = 0;
    if (!ReadQ(base + i, &q)) {
      *report = "cannot read the base record";
      return false;
    }
    memcpy(mem + i, &q, 8);
  }
  for (size_t i = 0; i < kBlock; i += 8) {
    uintptr_t q = 0;
    if (!ReadQ(base_data + i, &q)) break;  // the rest stays zero
    memcpy(data + i, &q, 8);
  }
  std::string taken;
  for (unsigned off : offsets) {
    uintptr_t q = 0;
    if (off + 8 > kBlock || !ReadQ(donor_data + off, &q)) {
      *report = "bad field offset";
      return false;
    }
    memcpy(data + off, &q, 8);
    char t[24];
    snprintf(t, sizeof t, " +0x%X", off);
    taken += t;
  }
  const void* dp = data;
  memcpy(mem + 0x10, &dp, 8);
  char b[256];
  snprintf(b, sizeof b, "look abs:%llX = %s with%s from %s", (unsigned long long)reinterpret_cast<uintptr_t>(mem),
           base_name, taken.c_str(), donor_name);
  *report = b;
  SowLog(L"sim: %S", b);
  return true;
}

// ---------------------------------------------------------------- behavior resource (probe "animres")

namespace {

// A character's animation object (character+0x120) only switches the character on
// (+0x1990 block, flag +0x18; 3F0D88 from E7D070) once both its animation resource (+0x1870)
// and its behavior resource (+0x1878) are set. 459474 is the animation object's virtual
// "set behavior resource": anim+0x4B0 = res, vfunc +0x30, anim+0x1878 = res.
using SetBehavior_t = void(__fastcall*)(void* anim, void* res);
SetBehavior_t g_set_behavior = nullptr;
volatile LONG g_animres_pending = 0;
uintptr_t g_animres_char = 0, g_animres_res = 0;

}  // namespace

bool AnimResPending() { return g_animres_pending != 0; }

void RunAnimRes() {
  if (!InterlockedExchange(&g_animres_pending, 0)) return;
  g_set_behavior(reinterpret_cast<void*>(g_animres_char + 0x120), reinterpret_cast<void*>(g_animres_res));
  SowLog(L"sim: behavior resource of abs:%llX set to abs:%llX", (unsigned long long)g_animres_char,
         (unsigned long long)g_animres_res);
}

bool SimSetBehaviorResource(uintptr_t fn_rva, uintptr_t character, uintptr_t res, std::string* err) {
  const uint8_t* f = Base() + fn_rva;
  if (!Head(f, "48895C2408574883EC20488991B0040000")) {
    *err = "behavior-resource setter bytes differ from the known build; refused";
    return false;
  }
  uintptr_t vt = 0;
  if (!ReadQ(character, &vt) || !InImage(vt) || !res) {
    *err = "not a character, or no resource";
    return false;
  }
  g_set_behavior = reinterpret_cast<SetBehavior_t>(const_cast<uint8_t*>(f));
  g_animres_char = character;
  g_animres_res = res;
  InterlockedExchange(&g_animres_pending, 1);
  return true;
}

// coop_puppet_spawn: a character of type made at pos, given this copy's own player's
// behavior resource (anim+0x1878 = player+0x1998), so a cutscene "Talion" (no AI behavior of
// its own) is switched on and animates with the player's graph; then used as the puppet.
bool SimSpawnPuppet(const SimSpawnRvas& r, const char* type, const float pos[3], std::string* report, bool run_now) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(Base());
  uintptr_t gc = 0, player = 0, res = 0, obj = 0;
  if (!ReadQ(base + r.gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &player) || !player ||
      !ReadQ(player + 0x1998, &res) || !res) {
    *report = "no player behavior resource yet";
    return false;
  }
  // Without the table's global, the spawn type is found through an AI character.
  if (!r.spawn_table_rva && !g_puppet && !g_spawn_table_src && !SimPickNearestPuppet(r.gameclient_rva, report, false))
    return false;
  if (!SimSpawn(r, type, pos, nullptr, report, &obj, run_now)) return false;
  std::string err;
  if (!SimSetBehaviorResource(r.behavior_rva, obj, res, &err) || !SimSetPuppet(obj, &err)) {
    *report += "; " + err;
    return false;
  }
  *report += "; given the player's behavior resource; now the puppet";
  return true;
}

// ---------------------------------------------------------------- animation inputs (coop, probe "puppet mirror")

namespace {

// The player's animation graph is driven by named float variables that the player's
// per-frame update (30BA10, and 514484 for turning and the camera) sets on its animation
// object (player+0x120) through 176D78: the stick, speed, turning, camera. An AI character
// gets none of them, so a puppet with the player's graph (the Talion stand-in) only holds a
// pose. An entry hook on 176D78 records this copy's player's values as they are set (the
// handler gets the setter's own rcx, rdx and xmm2); a puppet is given values once per frame
// through 176D78 itself.
using SetFloatVar_t = void(__fastcall*)(void* anim, const char* name, float value);
SetFloatVar_t g_set_float_var = nullptr;
const char* const kInputNames[kAnimInputs] = {
    "RawMoveMag",           "RawMoveYaw",          "Game_MovementAxisX",        "Game_MovementAxisY",
    "PlayerVelocity",       "PlayerSmoothVelocity", "DeltaYaw",                 "YawVelocityInDegsPerSec",
    "CameraDeltaYaw",       "DeltaPitch",          "PitchVelocityInDegsPerSec", "PitchVelocityAbs",
    "CharToCamDeltaYawInDegs", "CharToCamDeltaPitchInDegs", "CameraYawMinusPlayerYaw", "PlayerCameraDeltaYaw",
    "Game_CameraAxisX",     "Game_CameraAxisY",    "Game_CameraPitch",          "RawCameraMag",
    "RawLeftTrigger",       "RawRightTrigger",     "Game_SimTimer_TimeScale",   "Player_Focus",
    "MountWarg_Yaw"};

uintptr_t g_input_gc = 0;                  // the GameClient global
volatile uintptr_t g_input_owner = 0;      // this copy's player's animation object, every frame
const char* g_input_ptr[kAnimInputs] = {};  // the game's own name pointers, learned when first seen
float g_inputs[kAnimInputs] = {};
volatile LONG g_input_mask = 0;
volatile LONG g_input_hits = 0;
volatile LONG g_input_state = 0;  // 0 none, 1 installed
volatile LONG g_mirror = 0;       // probe "puppet mirror on": the puppet gets this copy's own inputs
volatile LONG g_inputs_applied = 0;
constexpr int kOtherInputs = 64;
const char* g_other_ptr[kOtherInputs] = {};  // other float names set on the player (diagnostics)
float g_other[kOtherInputs] = {};

void __fastcall AnimInputHit(void* anim, const char* name, float value) {
  if (!anim || reinterpret_cast<uintptr_t>(anim) != g_input_owner || !name) return;
  for (int i = 0; i < kAnimInputs; ++i) {
    if (g_input_ptr[i] == name || (!g_input_ptr[i] && strcmp(name, kInputNames[i]) == 0)) {
      g_input_ptr[i] = name;
      g_inputs[i] = value;
      InterlockedOr(&g_input_mask, 1L << i);
      InterlockedIncrement(&g_input_hits);
      return;
    }
  }
  // Any other name the player gets: kept for probe "inputs" (finding inputs worth sending).
  for (int i = 0; i < kOtherInputs; ++i) {
    if (g_other_ptr[i] == name || !g_other_ptr[i]) {
      g_other_ptr[i] = name;
      g_other[i] = value;
      return;
    }
  }
}

}  // namespace

void RefreshInputOwner() {
  uintptr_t gc = 0, player = 0;
  g_input_owner = g_input_gc && ReadQ(g_input_gc, &gc) && gc && ReadQ(gc + 0x888, &player) && player ? player + 0x120 : 0;
}

bool InputsOn() { return g_input_state == 1; }

void ApplyMirror() {
  std::string err;
  if (g_mirror && g_input_state == 1) SimApplyPuppetInputs(0, g_inputs, (uint32_t)g_input_mask, &err);
}

bool SimAnimInputsInstall(uintptr_t setter_rva, uintptr_t gameclient_rva, std::string* err) {
  if (g_input_state == 1) return true;
  uint8_t* const f = const_cast<uint8_t*>(Base()) + setter_rva;
  if (!Head(f, "48895C241055488BEC4883EC3080B98818000000")) {
    *err = "animation-variable setter bytes differ from the known build; refused";
    return false;
  }
  const uintptr_t target = reinterpret_cast<uintptr_t>(f), aligned = target & ~uintptr_t(7);
  const size_t o = target - aligned;
  uint8_t* page = SimNearPage(3);
  const int64_t rel = page ? reinterpret_cast<intptr_t>(page) - (intptr_t)(target + 5) : 0;
  if (!page || o > 3 || rel < INT32_MIN || rel > INT32_MAX) {
    *err = "no code page near the setter";
    return false;
  }
  // The stub: flags and volatile registers saved, AnimInputHit(rcx, rdx, xmm2) called, all
  // restored; then the setter's first instruction (mov [rsp+10h],rbx, 5 bytes) and a jump back.
  std::vector<uint8_t> s = {0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52,
                            0x41, 0x53, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x7F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x48, 0xB8});
  void* hit = reinterpret_cast<void*>(&AnimInputHit);
  const auto* h = reinterpret_cast<const uint8_t*>(&hit);
  s.insert(s.end(), h, h + 8);
  s.insert(s.end(), {0xFF, 0xD0});
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x6F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
                     0x5A, 0x59, 0x58, 0x9D});
  if (s.size() != 122) {
    *err = "stub size mismatch";
    return false;
  }
  s.insert(s.end(), f, f + 5);
  s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  const uint64_t back = target + 5;
  const auto* bb = reinterpret_cast<const uint8_t*>(&back);
  s.insert(s.end(), bb, bb + 8);
  DWORD old = 0;
  if (!VirtualProtect(page, 4096, PAGE_READWRITE, &old)) {
    *err = "cannot write the stub page";
    return false;
  }
  memcpy(page, s.data(), s.size());
  memcpy(page + 160, kPreStubUnwind, sizeof kPreStubUnwind);
  auto* fn = reinterpret_cast<RUNTIME_FUNCTION*>(page + 192);
  fn->BeginAddress = 0;
  fn->EndAddress = 160;
  fn->UnwindData = 160;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  RtlAddFunctionTable(fn, 1, reinterpret_cast<DWORD64>(page));
  g_input_gc = reinterpret_cast<uintptr_t>(Base()) + gameclient_rva;
  g_set_float_var = reinterpret_cast<SetFloatVar_t>(f);
  RefreshInputOwner();
  // The 5-byte jmp goes in with one aligned 8-byte write (the bytes after it are left as they
  // were; the stub runs the first instruction instead).
  uint64_t old_q = 0, new_q = 0;
  memcpy(&old_q, reinterpret_cast<const void*>(aligned), 8);
  new_q = old_q;
  auto* nb = reinterpret_cast<uint8_t*>(&new_q);
  nb[o] = 0xE9;
  const int32_t rel32 = (int32_t)rel;
  memcpy(nb + o + 1, &rel32, 4);
  DWORD prot = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(aligned), 8, PAGE_EXECUTE_READWRITE, &prot)) {
    *err = "cannot make the setter's entry writable";
    return false;
  }
#ifndef SOWGUARD_MUTANT  // negative control: the setter is never hooked
  InterlockedExchange64(reinterpret_cast<volatile LONG64*>(aligned), (LONG64)new_q);
#endif
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(aligned), 8);
  VirtualProtect(reinterpret_cast<void*>(aligned), 8, prot, &old);
  InterlockedExchange(&g_input_state, 1);
  SowLog(L"sim: animation inputs of the player recorded at rva:%llX (%d names)", (unsigned long long)setter_rva,
         kAnimInputs);
  return true;
}

bool SimAnimInputs(float out[kAnimInputs], uint32_t* mask) {
  if (g_input_state != 1) return false;
  memcpy(out, g_inputs, sizeof g_inputs);
  *mask = (uint32_t)g_input_mask;
  return true;
}

bool SimApplyPuppetInputs(uintptr_t setter_rva, const float in[kAnimInputs], uint32_t mask, std::string* err) {
  if (!g_set_float_var) {  // a receiver that does not record (only the sender installs the hook)
    uint8_t* const f = const_cast<uint8_t*>(Base()) + setter_rva;
    if (!Head(f, "48895C241055488BEC4883EC3080B98818000000")) {
      *err = "animation-variable setter bytes differ from the known build; refused";
      return false;
    }
    g_set_float_var = reinterpret_cast<SetFloatVar_t>(f);
  }
  if (!mask || GetCurrentThreadId() != InputGameThread()) return false;
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner || !PuppetAlive(owner, vt, health)) return false;
  void* anim = reinterpret_cast<void*>(owner + 0x120);
  for (int i = 0; i < kAnimInputs; ++i)
    if (mask & (1u << i)) g_set_float_var(anim, kInputNames[i], in[i]);
  InterlockedIncrement(&g_inputs_applied);
  return true;
}

void SimSetMirror(bool on) { InterlockedExchange(&g_mirror, on ? 1 : 0); }

std::string SimInputsReport() {
  std::ostringstream out;
  out << "ok inputs=" << (g_input_state == 1 ? "recorded" : "off") << " hits=" << g_input_hits
      << " applied=" << g_inputs_applied << " mirror=" << (g_mirror ? "on" : "off") << "\n";
  const LONG mask = g_input_mask;
  for (int i = 0; i < kAnimInputs; ++i) {
    char b[96];
    snprintf(b, sizeof b, "%s=%s%.3f\n", kInputNames[i], (mask & (1L << i)) ? "" : "(never) ", g_inputs[i]);
    out << b;
  }
  for (int i = 0; i < kOtherInputs && g_other_ptr[i]; ++i) {
    char b[128];
    snprintf(b, sizeof b, "other %.64s=%.3f\n", g_other_ptr[i], g_other[i]);
    out << b;
  }
  return out.str();
}

// ---------------------------------------------------------------- root-motion speed (coop_anim_speed)

namespace {

// A character moves by its animation's root motion: each frame the motion its graph produced
// is copied to [character+0x11D8] (local space) and, for an AI character, on to its movement
// controller ([character+0x2C78]+0x9C, 2FDF68); the movement component's per-frame step
// (vtable slot 0x80, 191868 for an AI character, called from 3012C9) reads it there (119BAC
// through 11AE84, unless [controller+0x12C] is set). A Talion stand-in's graph, given the
// sender's inputs, runs no faster than ~230 units/s while the sender free-runs at 600-750
// (seen 2026-10-22: the sender's own graph gets the free-run elsewhere). So the stand-in's
// movement component gets a copy of its vtable with slot 0x80 wrapped: before the step, the
// motion is scaled by the sender's root-motion speed (smoothed over a tenth of a second, so
// it follows a free-run's quick start, and faster when it drops, so the stand-in does not run
// on past a sudden stop) plus a catch-up, over the stand-in's own (smoothed over a quarter
// second, so its stride keeps its own rhythm). It engages only while the sender is faster
// than the stand-in's own run can be (a free-run: over 300 units/s, and 1.2 times the
// stand-in's) and for half a second after: walks and runs are left to the stand-in's own
// motion, which matches the sender's there (scaling them too, at every start and corner,
// made steered walks drift more). Only that one object uses the copy; other characters keep
// the class's vtable.
constexpr unsigned kMoverSlots = 80;          // copied (the class uses at least 0x130 / 8)
constexpr float kRootWantTau = 0.1f, kRootWantDropTau = 0.04f, kRootOwnTau = 0.25f;  // seconds
constexpr float kRootMinOwn = 30.0f;          // slower than this its direction is unknown
constexpr float kRootMinRatio = 0.5f, kRootMaxRatio = 4.0f;
constexpr float kRootEngage = 1.2f, kRootEngageSpeed = 300.0f, kRootHold = 0.5f;  // ratio; units/s; seconds
using MoverStep_t = void(__fastcall*)(uintptr_t mover, void* target);
MoverStep_t g_mover_step = nullptr;   // the class's slot 0x80
volatile uintptr_t g_speed_mover = 0;  // the puppet's movement component, with the copied vtable
uintptr_t g_speed_owner = 0;          // the puppet it belongs to
volatile float g_root_target = -1.0f;  // the sender's root-motion speed (units/s); < 0: off
volatile float g_root_extra = 0.0f;    // added after smoothing (catch-up)
float g_root_want = 0, g_root_own = 0;  // smoothed
float g_root_clock = 0, g_root_engaged_until = -1;  // seconds of the stand-in's frames
volatile float g_root_ratio = 1.0f;
volatile LONG g_root_frames = 0;       // frames scaled
std::string g_root_err;

void ScaleRootMotion(uintptr_t mover) {
  const float target = g_root_target;
  if (target < 0) return;
  __try {
    const uintptr_t ch = *reinterpret_cast<const uintptr_t*>(mover + 0x520);
    const uintptr_t c = *reinterpret_cast<const uintptr_t*>(ch + 0x2C78);
    if (!c || *reinterpret_cast<const uint8_t*>(c + 0x12C)) return;  // no root motion this frame
    float* d = reinterpret_cast<float*>(c + 0x9C);
    const float dt = *reinterpret_cast<const float*>(ch + 0x1118);
    if (!(dt > 0.0f && dt < 0.5f)) return;
    const float own = std::sqrt(d[0] * d[0] + d[2] * d[2]) / dt;
    const float tau = target < g_root_want ? kRootWantDropTau : kRootWantTau;
    g_root_want += (target - g_root_want) * (1.0f - std::exp(-dt / tau));
    g_root_own += (own - g_root_own) * (1.0f - std::exp(-dt / kRootOwnTau));
    g_root_clock += dt;
    if (g_root_want > kRootEngageSpeed && g_root_want > kRootEngage * g_root_own)
      g_root_engaged_until = g_root_clock + kRootHold;
    if (g_root_clock > g_root_engaged_until) return;  // not a free-run: its own motion
    if (own < kRootMinOwn || g_root_own < kRootMinOwn) return;
    const float want = std::max(0.0f, g_root_want + g_root_extra);
    const float r = std::min(std::max(want / g_root_own, kRootMinRatio), kRootMaxRatio);
    d[0] *= r;
    d[2] *= r;
    g_root_ratio = r;
    InterlockedIncrement(&g_root_frames);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

volatile LONG g_skip_step = 0;     // the puppet's movement step and update are skipped (sender in the air)
volatile LONG g_skipped_steps = 0;
// Slot 0x100 (2FFC1C, the same in the player's movement class): the frame update, which writes
// the character's transform from the movement's own state (250C1C -> 14554C); with only the
// step skipped, it pulled a stand-in held in the air back to the ground (session an).
using MoverUpdate_t = void(__fastcall*)(uintptr_t mover, void* state);
MoverUpdate_t g_mover_update = nullptr;

void __fastcall PuppetMoverUpdate(uintptr_t mover, void* state) {
  if (mover == g_speed_mover && g_skip_step) return;
  g_mover_update(mover, state);
}

void __fastcall PuppetMoverStep(uintptr_t mover, void* target) {
  if (mover == g_speed_mover && g_skip_step) {
    InterlockedIncrement(&g_skipped_steps);
    return;
  }
  if (mover == g_speed_mover) ScaleRootMotion(mover);
  g_mover_step(mover, target);
}

// On the simulation thread: give the current puppet's movement component the copied vtable.
bool WrapPuppetMover(uintptr_t step_rva, std::string* err) {
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner || !PuppetAlive(owner, vt, health)) {
    *err = "no puppet";
    return false;
  }
  uintptr_t mover = 0, back = 0, mvt = 0, slot = 0, c = 0, c_back = 0;
  if (!ReadQ(owner + 0x24A0, &mover) || !mover || !ReadQ(mover + 0x520, &back) || back != owner ||
      !ReadQ(mover, &mvt)) {
    *err = "the puppet has no movement component of the known shape";
    return false;
  }
  if (owner == g_speed_owner && mover == g_speed_mover) return true;
  if (!InImage(mvt) || !ReadQ(mvt + 0x80, &slot) || !ReadQ(owner + 0x2C78, &c) || !c || !ReadQ(c, &c_back) ||
      c_back != owner) {
    *err = "the puppet has no movement component and controller of the known shape";
    return false;
  }
  const uint8_t* f = Base() + step_rva;
  if (slot != reinterpret_cast<uintptr_t>(f) || !Head(f, "488BC448895808488970104889781855488BEC4883EC70")) {
    *err = "the movement component's step (slot 0x80) differs from the known build; refused";
    return false;
  }
  // The copy keeps the type information before the table (vt[-1]).
  auto* copy = static_cast<uintptr_t*>(VirtualAlloc(nullptr, (kMoverSlots + 1) * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!copy) {
    *err = "no memory for the vtable copy";
    return false;
  }
  for (unsigned i = 0; i <= kMoverSlots; ++i) {
    if (!ReadQ(mvt - 8 + 8 * i, &copy[i])) {
      VirtualFree(copy, 0, MEM_RELEASE);
      *err = "the movement component's vtable is not readable";
      return false;
    }
  }
  g_mover_step = reinterpret_cast<MoverStep_t>(slot);
  copy[1 + 0x80 / 8] = reinterpret_cast<uintptr_t>(&PuppetMoverStep);
  const auto* update = reinterpret_cast<const uint8_t*>(copy[1 + 0x100 / 8]);
  if (InImage(reinterpret_cast<uintptr_t>(update)) && Head(update, "48895C24084889742410" "48897C241855488BEC4883EC70")) {
    g_mover_update = reinterpret_cast<MoverUpdate_t>(const_cast<uint8_t*>(update));
    copy[1 + 0x100 / 8] = reinterpret_cast<uintptr_t>(&PuppetMoverUpdate);
  } else {
    SowLog(L"sim: the movement component's update (slot 0x100) differs from the known build; not skipped "
           L"in the air");
  }
  DWORD old = 0;
  VirtualProtect(copy, (kMoverSlots + 1) * 8, PAGE_READONLY, &old);
  g_root_want = g_root_own = 0;
  g_root_engaged_until = -1;
  g_speed_mover = mover;
  g_speed_owner = owner;
#ifndef SOWGUARD_MUTANT  // negative control: the stand-in keeps its own speed
  InterlockedExchange64(reinterpret_cast<volatile LONG64*>(mover), (LONG64)(copy + 1));
#endif
  SowLog(L"sim: puppet movement component abs:%llX: step wrapped (root-motion speed of the sender)",
         (unsigned long long)mover);
  return true;
}

}  // namespace

void SimSetPuppetRootSpeed(uintptr_t step_rva, float speed, float extra) {
  if (speed >= 0) {
    std::string err;
    if (!WrapPuppetMover(step_rva, &err)) {
      if (err != g_root_err && err != "no puppet") SowLog(L"sim: the puppet keeps its own speed: %S", err.c_str());
      g_root_err = err;
      speed = -1;
    }
  }
  g_root_extra = extra;
  g_root_target = speed;
}

bool SimSkipPuppetStep(uintptr_t step_rva, bool skip) {
  if (skip) {
    std::string err;
    if (!WrapPuppetMover(step_rva, &err)) {
      if (err != g_root_err && err != "no puppet") SowLog(L"sim: the puppet's step is not skipped: %S", err.c_str());
      g_root_err = err;
      skip = false;
    }
  }
  InterlockedExchange(&g_skip_step, skip ? 1 : 0);
  return skip;
}

long SimSkippedSteps() { return g_skipped_steps; }

std::string SimRootSpeedReport() {
  char b[128];
  snprintf(b, sizeof b, "root_speed=%s/%ld ratio=%.2f want=%.0f extra=%.0f own=%.0f", g_root_target < 0 ? "off" : "on",
           g_root_frames, g_root_ratio, g_root_want, g_root_extra, g_root_own);
  return b;
}

// ---------------------------------------------------------------- game actions (buttons for the graph)

namespace {

// The player's update (30C276) fills, every frame, three lists of the 377 game actions on its
// character: +0x5F8 (a count, then a byte per action: held), +0x778 (a count, then a byte per
// action: pressed) and +0x8F8 (a count, then a float per action); 1B1840 copies them into the
// character's animation job. The graph's input nodes (644AD8) read its buttons there (states
// A_Button_JustPressed -> A_Button_WaitingForUnpress -> Sprint), but only while the job's copy
// of character+0x113A is set and the graph counts as player-driven (character+0x1129, both set
// to 1 by the player's update every frame); then the lists must be full (count 377), or the
// game's bounds check (FA9C20) writes to address 0. An AI character's lists stay empty and its
// flags 0, so a Talion stand-in walks where the player free-runs (its stick states come from
// the float inputs). So the stand-in gets the sender's lists every frame, full, with both
// flags; switched off, flags and lists go back together (one simulation-thread write before
// the job copies them).
constexpr unsigned kActions = 377;
constexpr uintptr_t kHeld = 0x5F8, kPressed = 0x778, kValue = 0x8F8, kFlagInput = 0x113A, kFlagPlayer = 0x1129;
volatile LONG g_actions_applied = 0;
uintptr_t g_actions_owner = 0;  // the puppet that has them

void WriteActions(uintptr_t ch, const SimAction* a, int n, bool on) {
  __try {
    auto* held = reinterpret_cast<uint8_t*>(ch + kHeld + 4);
    auto* pressed = reinterpret_cast<uint8_t*>(ch + kPressed + 4);
    auto* value = reinterpret_cast<float*>(ch + kValue + 4);
    if (!on) {
      *reinterpret_cast<uint8_t*>(ch + kFlagInput) = 0;
      *reinterpret_cast<uint8_t*>(ch + kFlagPlayer) = 0;
    }
    *reinterpret_cast<uint32_t*>(ch + kHeld) = on ? kActions : 0;
    *reinterpret_cast<uint32_t*>(ch + kPressed) = on ? kActions : 0;
    *reinterpret_cast<uint32_t*>(ch + kValue) = on ? kActions : 0;
    memset(held, 0, kActions);
    memset(pressed, 0, kActions);
    memset(value, 0, kActions * sizeof(float));
    for (int i = 0; on && i < n; ++i) {
      if (a[i].index >= kActions) continue;
      held[a[i].index] = a[i].held;
      pressed[a[i].index] = a[i].pressed;
      value[a[i].index] = a[i].value;
    }
    if (on) {
      *reinterpret_cast<uint8_t*>(ch + kFlagInput) = 1;
      *reinterpret_cast<uint8_t*>(ch + kFlagPlayer) = 1;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Probe "puppet hold": experiment, the listed actions held for a while (pressed on the first
// frame), without the flags.
constexpr int kMaxHeld = 16;
SimAction g_hold[kMaxHeld] = {};
int g_hold_count = 0;
volatile ULONGLONG g_hold_until = 0;  // GetTickCount64 deadline; 0: off
bool g_hold_first = false;

}  // namespace

int SimPlayerActions(uintptr_t gameclient_rva, SimAction* out, int max) {
  uintptr_t gc = 0, player = 0;
  if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &player) ||
      !player)
    return 0;
  int n = 0;
  __try {
    const uint32_t count = std::min(*reinterpret_cast<const uint32_t*>(player + kHeld), kActions);
    const auto* held = reinterpret_cast<const uint8_t*>(player + kHeld + 4);
    const auto* pressed = reinterpret_cast<const uint8_t*>(player + kPressed + 4);
    const auto* value = reinterpret_cast<const float*>(player + kValue + 4);
    for (uint32_t i = 0; i < count && n < max; ++i) {
      if (!held[i] && !pressed[i] && value[i] == 0.0f) continue;
      out[n].index = (uint16_t)i;
      out[n].held = held[i];
      out[n].pressed = pressed[i];
      out[n].value = value[i];
      ++n;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  return n;
}

bool SimApplyPuppetActions(const SimAction* a, int n, std::string* err) {
  if (GetCurrentThreadId() != InputGameThread()) {
    *err = "not on the simulation thread";
    return false;
  }
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner || !PuppetAlive(owner, vt, health)) {
    g_actions_owner = 0;
    *err = "no puppet";
    return false;
  }
  WriteActions(owner, a, n, true);
  g_actions_owner = owner;
  InterlockedIncrement(&g_actions_applied);
  return true;
}

void SimReleasePuppetActions() {
  const uintptr_t had = g_actions_owner;
  g_actions_owner = 0;
  if (!had || GetCurrentThreadId() != InputGameThread()) return;
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (owner == had && PuppetAlive(owner, vt, health)) WriteActions(owner, nullptr, 0, false);
}

long SimPuppetActionFrames() { return g_actions_applied; }

bool HoldPending() { return g_hold_until != 0; }

void RunHold() {
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner || !PuppetAlive(owner, vt, health)) {
    g_hold_until = 0;
    return;
  }
  const bool on = GetTickCount64() < g_hold_until;
  for (int i = 0; i < g_hold_count; ++i) g_hold[i].pressed = g_hold_first ? 1 : 0;
  g_hold_first = false;
  // The flags stay as they are (a probe "write" sets them for experiments).
  __try {
    const uint8_t input = *reinterpret_cast<const uint8_t*>(owner + kFlagInput);
    const uint8_t player = *reinterpret_cast<const uint8_t*>(owner + kFlagPlayer);
    WriteActions(owner, g_hold, g_hold_count, on);
    *reinterpret_cast<uint8_t*>(owner + kFlagInput) = input;
    *reinterpret_cast<uint8_t*>(owner + kFlagPlayer) = player;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (!on) g_hold_until = 0;
}

std::string SimPuppetHold(unsigned ms, const int* index, const float* values, int n) {
  if (!g_installed) return "err simulation-thread hook not installed (sim_hook)\n";
  if (!g_puppet) return "err no puppet\n";
  if (n < 1 || n > kMaxHeld || !ms || ms > 30000)
    return "err usage: puppet hold <1..30000 ms> <index 0..376>[=value] (at most 16)\n";
  for (int i = 0; i < n; ++i)
    if (index[i] < 0 || index[i] >= (int)kActions) return "err action index out of range (0..376)\n";
  g_hold_until = 0;
  Sleep(50);
  for (int i = 0; i < n; ++i) g_hold[i] = {(uint16_t)index[i], 1, 0, values[i]};
  g_hold_count = n;
  g_hold_first = true;
  g_hold_until = GetTickCount64() + ms;
  return "ok holding on the puppet\n";
}

// ---------------------------------------------------------------- the puppet as the player's follower

namespace {

// A stand-in within ~300 units puts the local player in combat: the game's combat log says
// InCombat TRUE and Sheathed FALSE, and the player's graph walks in Hand_Holding (session ak).
// The character's player-follower byte (+0x23B8: getter 1156AF0, script SetIsPlayerFollower,
// setter 102F0C, which also tells the faction, UI and squad systems) set to 1 on the stand-in
// kept InCombat FALSE and Sheathed TRUE beside it, with no Hand_Holding, and the game counts
// it as a player ally (IsPlayerAlly, 366B88, reads this byte). Only the byte is written: the
// setter's notifications changed nothing more. The player can still strike it (two hits kill
// it, setter or not; session al).
constexpr uintptr_t kFollower = 0x23B8;
uintptr_t g_follower_owner = 0;  // the puppet last marked
int g_follower_build = 0;        // the getter's bytes: 0 unchecked, 1 known, -1 differ
volatile LONG g_follower_marks = 0;

// Follower or not, the player's attacks still picked the stand-in as their target and one
// strike killed it (session al): the game lets the player kill allies. Every attack node
// checks each candidate target with 18626C (rcx the node, rdx the attacker's context, r8 the
// candidate, [r8+0x20] the candidate's health object; a read watch on the stand-in's health
// object during a strike went through it). The game's own "targetable" flag
// (character+0x2C16, script SetTargetable) at 0 did not stop the strikes. A stub at the
// check's second instruction (186271: the first ends 4 bytes into an aligned qword, so a
// 5-byte jump there would need two writes) returns false (rejected) for the puppet's health
// object; both callers then go on to the next candidate. That stopped the strikes aiming at
// it, but a swing without a target still hit it (the sword's shape cast) and killed it. Write
// watches on the stand-in's health showed two ways in, a targeted hit (2C642C -> 2C658C, which
// logs APPLYDAMAGE) and an untargeted one (1F6FE0 -> 2C966C -> ... -> 2C8150), both through
// 2C70C0, which applies a hit: rcx a component of the attacker, rdx one of the target
// ([rdx+0x28] the target character; then 2EE394 -> EAA190 -> 2E4B34 write its health). A
// second stub returns from 2C70C0 at once for the puppet.
volatile uintptr_t g_untargeted = 0;  // the health object the check stub rejects (0: none)
volatile uintptr_t g_unhit = 0;       // the character the hit stub skips (0: none)
volatile uintptr_t g_aoe_skip = 0;    // the character the area filter leaves out of an area attack (0: none)
volatile uintptr_t g_decoy = 0;       // the decoy ally that enemies attack in the stand-in's place (hits on it are reported like the stand-in's)
volatile LONG g_standin_hurt = 0;     // swings that reach the stand-in are reported (SimDamageReports) instead of skipped
volatile LONG g_dmg_dups = 0;         // second reports of one swing that reached both the stand-in and the decoy (not sent)
volatile LONG g_target_rejects = 0;   // candidates the check stub rejected
volatile LONG g_hit_drops = 0;        // hits the hit stub dropped
// Area attacks (fire, explosions, auras) do not go through either: a large enemy's fire took a
// stand-in from 298 to 3 with both stubs in (session as). Each area tick asks 2E5718 about every
// character near it (rcx the tick, rdx the character; it logs "AOETick = %s; %s Evaluating
// Object." and every reason to skip one, MustBeAlive, MustBeAlly, ...) and adds the ones it
// accepts to the tick's hit list; it returns nothing. A third stub returns from it at once for the
// puppet.
volatile LONG g_aoe_skips = 0;        // area ticks the stub kept off the puppet
volatile LONG g_target_hook = 0;      // 1 once both stubs are in

}  // namespace

namespace {

// A 5-byte jump to stub at `at`, written with one aligned 8-byte write (the other bytes of that
// qword stay as they are).
bool PatchJump(uintptr_t at, const uint8_t* stub, std::string* err) {
  const uintptr_t aligned = at & ~uintptr_t(7);
  const size_t o = at - aligned;
  const int64_t rel = reinterpret_cast<intptr_t>(stub) - (intptr_t)(at + 5);
  if (o > 3 || rel < INT32_MIN || rel > INT32_MAX) {
    *err = "the jump does not fit (alignment or reach)";
    return false;
  }
  uint64_t q = 0;
  memcpy(&q, reinterpret_cast<const void*>(aligned), 8);
  auto* nb = reinterpret_cast<uint8_t*>(&q);
  nb[o] = 0xE9;
  const int32_t rel32 = (int32_t)rel;
  memcpy(nb + o + 1, &rel32, 4);
  DWORD prot = 0, old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(aligned), 8, PAGE_EXECUTE_READWRITE, &prot)) {
    *err = "cannot make the game's code writable";
    return false;
  }
#ifndef SOWGUARD_MUTANT  // negative control: the player's attacks still pick and hit the puppet
  InterlockedExchange64(reinterpret_cast<volatile LONG64*>(aligned), (LONG64)q);
#endif
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(aligned), 8);
  VirtualProtect(reinterpret_cast<void*>(aligned), 8, prot, &old);
  return true;
}

void PutQ(std::vector<uint8_t>* s, uintptr_t v) {
  const auto* b = reinterpret_cast<const uint8_t*>(&v);
  s->insert(s->end(), b, b + 8);
}

}  // namespace

namespace {

// Every hit goes through 2C70C0(attacker's health object, victim's health object, position floats, attack record).
// A stub at its entry calls HitFilter with those four; true drops the hit (the callers ignore the result).
//   - a hit on the stand-in is dropped (the stand-in is a picture of the other player);
//   - a hit on a proxy is dropped too: a proxy is a picture of the host's enemy and takes no damage here. When
//     the attacker is this copy's own player, it is also reported (SimHitReports), so the host can apply it.
struct HitReportSlot {
  uintptr_t owner;
  uint32_t attack_id;
  float pos[3];
};
constexpr int kHitRing = 32;
HitReportSlot g_hit_ring[kHitRing];
volatile LONG g_hit_count = 0;       // reports queued so far
LONG g_hit_taken = 0;                // reports handed out so far
volatile LONG g_hit_watch = 0;
uintptr_t g_hit_gc = 0;
volatile LONG g_proxy_hits = 0, g_proxy_hits_other = 0;
volatile LONG g_proxy_react = 0;     // a hit of this copy's player on a proxy is also let through (reactions, counters, finishers)
HitReportSlot g_dmg_ring[kHitRing];  // swings of enemies that reached the stand-in
volatile LONG g_dmg_count = 0;
LONG g_dmg_taken = 0;

// Diagnostic (probe "coopset hitlog <n>"): the next n hits that reach the hit function are logged with both characters' types,
// health and roles (who hits whom among the enemies near the decoy, session cg).
volatile LONG g_hit_log_left = 0;
volatile LONG g_drop_standin_hits = 0, g_standin_hit_drops = 0;
volatile LONG g_drop_ally_hits = 1, g_ally_hit_drops = 0;  // hits of the host's allies on the stand-in or the decoy

void LogHitSeen(uintptr_t attacker, uintptr_t victim_owner, uintptr_t rec) {
  uintptr_t att_owner = 0, gc = 0, pl = 0;
  if (attacker) ReadQ(attacker + 0x28, &att_owner);
  ReadQ(g_hit_gc, &gc);
  if (gc) ReadQ(gc + 0x888, &pl);
  SimEnemyInfo a{}, v{};
  if (!att_owner || !SimEnemyRead(att_owner, &a)) strcpy_s(a.type, "?");
  if (!SimEnemyRead(victim_owner, &v)) strcpy_s(v.type, "?");
  uint32_t id = 0;
  __try {
    if (rec && *reinterpret_cast<const uint32_t*>(rec) == 0xA15) id = *reinterpret_cast<const uint32_t*>(rec + 4);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  auto role = [&](uintptr_t o) -> const char* {
    if (!o) return "";
    if (o == pl) return " [this player]";
    if (o == g_decoy) return " [decoy]";
    if (o == g_unhit) return " [stand-in]";
    if (ProxyHas(o)) return " [proxy]";
    return "";
  };
  SowLog(L"sim: hit seen: %S%S abs:%llX %.0f/%.0f <- %S%S abs:%llX %.0f/%.0f, attack %08X", v.type, role(victim_owner),
         (unsigned long long)victim_owner, v.hp, v.max_hp, a.type, role(att_owner), (unsigned long long)att_owner, a.hp, a.max_hp, id);
}

bool __fastcall HitFilter(uintptr_t attacker, uintptr_t victim, const float* pos, uintptr_t rec) {
  uintptr_t owner = 0;
  if (!victim || !ReadQ(victim + 0x28, &owner) || !owner) return false;
  if (g_hit_log_left > 0 && InterlockedDecrement(&g_hit_log_left) >= 0) LogHitSeen(attacker, owner, rec);
  if (g_drop_standin_hits && g_unhit && attacker) {
    uintptr_t att_owner = 0;
    if (ReadQ(attacker + 0x28, &att_owner) && att_owner == g_unhit) {
      InterlockedIncrement(&g_standin_hit_drops);
      return true;
    }
  }
  if (owner == g_unhit || (g_decoy && owner == g_decoy)) {
    InterlockedIncrement(&g_hit_drops);
    if (g_standin_hurt) {
      // A swing of an enemy that reached the stand-in: not taken here, told to the client, whose player it hits.
      uintptr_t att_owner = 0, gc0 = 0, pl0 = 0, mine0 = 0;
      const bool by_player = ReadQ(g_hit_gc, &gc0) && gc0 && ReadQ(gc0 + 0x888, &pl0) && pl0 && ReadQ(pl0 + 0x26A8, &mine0) &&
                             mine0 && attacker == mine0;
      if (attacker && !by_player && ReadQ(attacker + 0x28, &att_owner) && att_owner && att_owner != owner) {
        HitReportSlot s{};
        s.owner = att_owner;
        __try {
          if (rec && *reinterpret_cast<const uint32_t*>(rec) == 0xA15) s.attack_id = *reinterpret_cast<const uint32_t*>(rec + 4);
          if (pos) memcpy(s.pos, pos, sizeof s.pos);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        // The decoy stands where the stand-in stands, so one swing can reach both: two reports, and the client took the swing
        // twice (session cm: 12 of 31 swings came in pairs 15-63 ms apart, the same attacker and attack). A swing of the same
        // attacker and attack that reaches the other of the two within 300 ms is not reported again.
        static cooprules::SwingsSeen seen;
        // The host's own allies reach the stand-in and the decoy too: a tamed warg's running lunge (AI_Warg_WhileMoving) passed
        // through both and was sent to the client as damage, twice within 125 ms (session cm, coopset hitlog). An attacker of a
        // bystander kind (tamed, follower, prisoner, slave, captive) is not reported.
        SimEnemyInfo ai{};
        if (g_drop_ally_hits && SimEnemyRead(att_owner, &ai) && cooprules::BystanderKind(ai.type, sizeof ai.type)) {
          InterlockedIncrement(&g_ally_hit_drops);
        } else {
          EnterCriticalSection(&g_lock);
          if (cooprules::SameSwingAgain(&seen, att_owner, s.attack_id, owner, GetTickCount64())) {
            InterlockedIncrement(&g_dmg_dups);
          } else {
            g_dmg_ring[g_dmg_count % kHitRing] = s;
            InterlockedIncrement(&g_dmg_count);
          }
          LeaveCriticalSection(&g_lock);
        }
      }
    }
    return true;
  }
  if (!g_hit_watch || !ProxyHas(owner)) return false;
  uintptr_t gc = 0, player = 0, mine = 0;
  const bool by_me = ReadQ(g_hit_gc, &gc) && gc && ReadQ(gc + 0x888, &player) && player && ReadQ(player + 0x26A8, &mine) &&
                     mine && attacker == mine;
  if (!by_me) {
    InterlockedIncrement(&g_proxy_hits_other);
    return true;
  }
  HitReportSlot s{};
  s.owner = owner;
  __try {
    if (rec && *reinterpret_cast<const uint32_t*>(rec) == 0xA15) s.attack_id = *reinterpret_cast<const uint32_t*>(rec + 4);
    if (pos) memcpy(s.pos, pos, sizeof s.pos);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  EnterCriticalSection(&g_lock);
  g_hit_ring[g_hit_count % kHitRing] = s;
  InterlockedIncrement(&g_hit_count);
  LeaveCriticalSection(&g_lock);
  InterlockedIncrement(&g_proxy_hits);
  return g_proxy_react == 0;
}

}  // namespace

bool SimAttackHooksInstall(uintptr_t check_rva, uintptr_t hit_rva, uintptr_t aoe_rva, std::string* err) {
  if (g_target_hook == 1) return true;
  uint8_t* const check = const_cast<uint8_t*>(Base()) + check_rva;
  uint8_t* const hit = const_cast<uint8_t*>(Base()) + hit_rva;
  uint8_t* const aoe = const_cast<uint8_t*>(Base()) + aoe_rva;
  if (!Head(check, "48895C240844884C24204889542410555657415441554156415748" "8BEC4883EC704D8B7020")) {
    *err = "the attack target check's bytes differ from the known build; refused";
    return false;
  }
  if (!Head(hit, "48895C240848896C24184889742420574156" "4157")) {
    *err = "the hit's bytes differ from the known build; refused";
    return false;
  }
  if (!Head(aoe, "488BC448895810555657415441554156" "4157")) {
    *err = "the area-attack filter's bytes differ from the known build; refused";
    return false;
  }
  uint8_t* page = SimNearPage(4);
  if (!page) {
    *err = "no code page near the game image";
    return false;
  }
  // At the check's second instruction (check+5; the first ends 4 bytes into an aligned qword):
  // mov rax,[r8+20h]; mov r10,&g_untargeted; cmp rax,[r10]; jne +17;
  // mov r11,&g_target_rejects; lock inc dword [r11]; xor eax,eax; ret (rejected);
  // then that instruction (mov [rsp+20h],r9b) and a jump back after it.
  std::vector<uint8_t> s = {0x49, 0x8B, 0x40, 0x20, 0x49, 0xBA};
  PutQ(&s, reinterpret_cast<uintptr_t>(&g_untargeted));
  s.insert(s.end(), {0x49, 0x3B, 0x02, 0x75, 0x11, 0x49, 0xBB});
  PutQ(&s, reinterpret_cast<uintptr_t>(&g_target_rejects));
  s.insert(s.end(), {0xF0, 0x41, 0xFF, 0x03, 0x31, 0xC0, 0xC3});
  s.insert(s.end(), check + 5, check + 10);
  s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  PutQ(&s, reinterpret_cast<uintptr_t>(check + 10));
  // At the hit's entry (on an aligned qword): the argument registers and rflags saved, HitFilter(rcx, rdx, r8, r9)
  // called (rsp is 16-aligned after the seven pushes and the shadow space); a non-zero result is a dropped hit:
  // xor eax,eax; ret (the callers ignore rax). Otherwise its first instruction (mov [rsp+8],rbx) and a jump back.
  std::vector<uint8_t> t = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x9C, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8};
  PutQ(&t, reinterpret_cast<uintptr_t>(&HitFilter));
  t.insert(t.end(), {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x9D, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,
                     0x84, 0xC0, 0x74, 0x03, 0x31, 0xC0, 0xC3});
  t.insert(t.end(), hit, hit + 5);
  t.insert(t.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  PutQ(&t, reinterpret_cast<uintptr_t>(hit + 5));
  // At the area filter's entry (on an aligned qword): test rdx,rdx; je +30; mov r11,&g_unhit;
  // cmp rdx,[r11]; jne +15; mov r11,&g_aoe_skips; lock inc dword [r11]; ret (not in the tick);
  // then its first two instructions (mov rax,rsp; mov [rax+10h],rbx) and a jump back after them.
  std::vector<uint8_t> u = {0x48, 0x85, 0xD2, 0x74, 0x1E, 0x49, 0xBB};
  PutQ(&u, reinterpret_cast<uintptr_t>(&g_aoe_skip));
  u.insert(u.end(), {0x49, 0x3B, 0x13, 0x75, 0x0F, 0x49, 0xBB});
  PutQ(&u, reinterpret_cast<uintptr_t>(&g_aoe_skips));
  u.insert(u.end(), {0xF0, 0x41, 0xFF, 0x03, 0xC3});
  u.insert(u.end(), aoe, aoe + 7);
  u.insert(u.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  PutQ(&u, reinterpret_cast<uintptr_t>(aoe + 7));
  if (s.size() > 128 || t.size() > 128 || u.size() > 128) {
    *err = "stub size mismatch";
    return false;
  }
  DWORD old = 0;
  if (!VirtualProtect(page, 4096, PAGE_READWRITE, &old)) {
    *err = "cannot write the stub page";
    return false;
  }
  memcpy(page, s.data(), s.size());
  memcpy(page + 128, t.data(), t.size());
  memcpy(page + 256, u.data(), u.size());
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  if (!PatchJump(reinterpret_cast<uintptr_t>(check) + 5, page, err) ||
      !PatchJump(reinterpret_cast<uintptr_t>(hit), page + 128, err) ||
      !PatchJump(reinterpret_cast<uintptr_t>(aoe), page + 256, err))
    return false;
  InterlockedExchange(&g_target_hook, 1);
  SowLog(L"sim: attack target check rva:%llX, hit rva:%llX and area filter rva:%llX skip the puppet",
         (unsigned long long)check_rva, (unsigned long long)hit_rva, (unsigned long long)aoe_rva);
  return true;
}

void SimKeepPuppetUntargeted(bool on) {
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  const bool skip = on && owner && PuppetAlive(owner, vt, health);
  g_untargeted = skip ? health : 0;
  g_unhit = skip ? owner : 0;
  g_aoe_skip = skip && !g_standin_hurt ? owner : 0;  // hurt mode: an enemy's area swing may reach the stand-in
}

void SimHitWatch(uintptr_t gameclient_rva, bool on) {
  g_hit_gc = reinterpret_cast<uintptr_t>(Base()) + gameclient_rva;
  InterlockedExchange(&g_hit_watch, on ? 1 : 0);
}

size_t SimHitReports(SimHitReport* out, size_t max) {
  size_t n = 0;
  EnterCriticalSection(&g_lock);
  if (g_hit_count - g_hit_taken > kHitRing) g_hit_taken = g_hit_count - kHitRing;  // the oldest were overwritten
  while (g_hit_taken < g_hit_count && n < max) {
    const HitReportSlot& s = g_hit_ring[g_hit_taken % kHitRing];
    out[n].owner = s.owner;
    out[n].attack_id = s.attack_id;
    memcpy(out[n].pos, s.pos, sizeof out[n].pos);
    ++g_hit_taken;
    ++n;
  }
  LeaveCriticalSection(&g_lock);
  return n;
}

long SimProxyHits() { return g_proxy_hits; }

void SimProxyReact(bool on) { InterlockedExchange(&g_proxy_react, on ? 1 : 0); }

void SimStandinHurt(bool on) { InterlockedExchange(&g_standin_hurt, on ? 1 : 0); }

void SimSetDecoy(uintptr_t owner) { g_decoy = owner; }
void SimHitLog(long n) { InterlockedExchange(&g_hit_log_left, n); }
void SimDropStandinHits(bool on) { InterlockedExchange(&g_drop_standin_hits, on ? 1 : 0); }
long SimStandinHitDrops() { return g_standin_hit_drops; }
bool SimPresentationActive() {
  __try {
    const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(Base() + 0x26FFB70);
    return mgr && *reinterpret_cast<const volatile uint8_t*>(mgr + 0x1810) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
void SimDropAllyHits(bool on) { InterlockedExchange(&g_drop_ally_hits, on ? 1 : 0); }
long SimAllyHitDrops() { return g_ally_hit_drops; }
long SimDamageDups() { return g_dmg_dups; }

size_t SimDamageReports(SimHitReport* out, size_t max) {
  size_t n = 0;
  EnterCriticalSection(&g_lock);
  if (g_dmg_count - g_dmg_taken > kHitRing) g_dmg_taken = g_dmg_count - kHitRing;
  while (g_dmg_taken < g_dmg_count && n < max) {
    const HitReportSlot& s = g_dmg_ring[g_dmg_taken % kHitRing];
    out[n].owner = s.owner;
    out[n].attack_id = s.attack_id;
    memcpy(out[n].pos, s.pos, sizeof out[n].pos);
    ++g_dmg_taken;
    ++n;
  }
  LeaveCriticalSection(&g_lock);
  return n;
}

long SimTargetRejects() { return g_target_rejects; }

long SimHitDrops() { return g_hit_drops; }

long SimAoeSkips() { return g_aoe_skips; }

// ---------------------------------------------------------------- put-backs keep the puppet's animation

namespace {

// Put-backs are the game's queued teleport. Its handler (41B6B4, called at 4C4C45 with the
// message's flag as its last argument) on a full teleport tells the AI ("AI teleported.") and
// resets the character's animation controller: 41BB78 calls FA7E10 on [character+1970h], which
// queues a blend to the graph's start action, IdleL. With the weapon out the restarted graph
// then sheathes it (IdleL.To_WeaponSheatheLow); the sheathe's step took the puppet over 15 units
// off again and the next put-back restarted it: the sheathe loop, which also cut the puppet's
// swings short (session bd). Without the full flag the AI is not told and the puppet stayed where
// it was. A stub at FA7E10's entry returns at once for the puppet's controller: a full teleport
// without the restart.
volatile uintptr_t g_keep_anim_ctrl = 0;  // the controller the stub skips (0: none)
volatile LONG g_anim_resets_skipped = 0;
volatile LONG g_keep_anim_hook = 0;

}  // namespace

bool SimAnimResetHookInstall(uintptr_t reset_rva, std::string* err) {
  if (g_keep_anim_hook == 1) return true;
  uint8_t* const fn = const_cast<uint8_t*>(Base()) + reset_rva;
  if (!Head(fn, "48895C2408574883EC30" "33FF488BD9")) {
    *err = "the animation reset's bytes differ from the known build; refused";
    return false;
  }
  uint8_t* page = SimNearPage(4);
  if (!page) {
    *err = "no code page near the game image";
    return false;
  }
  // At its entry (on an aligned qword): test rcx,rcx; je +30; mov r11,&g_keep_anim_ctrl;
  // cmp rcx,[r11]; jne +15; mov r11,&g_anim_resets_skipped; lock inc dword [r11]; ret;
  // then its first instruction (mov [rsp+8],rbx) and a jump back after it.
  std::vector<uint8_t> s = {0x48, 0x85, 0xC9, 0x74, 0x1E, 0x49, 0xBB};
  PutQ(&s, reinterpret_cast<uintptr_t>(&g_keep_anim_ctrl));
  s.insert(s.end(), {0x49, 0x3B, 0x0B, 0x75, 0x0F, 0x49, 0xBB});
  PutQ(&s, reinterpret_cast<uintptr_t>(&g_anim_resets_skipped));
  s.insert(s.end(), {0xF0, 0x41, 0xFF, 0x03, 0xC3});
  s.insert(s.end(), fn, fn + 5);
  s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  PutQ(&s, reinterpret_cast<uintptr_t>(fn + 5));
  DWORD old = 0;
  // Execute stays on: the attack stubs on the same page may be running on other threads.
  if (s.size() > 128 || !VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot write the stub";
    return false;
  }
  memcpy(page + 384, s.data(), s.size());
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  if (!PatchJump(reinterpret_cast<uintptr_t>(fn), page + 384, err)) return false;
  InterlockedExchange(&g_keep_anim_hook, 1);
  SowLog(L"sim: the animation reset rva:%llX skips the puppet (put-backs keep its animation)",
         (unsigned long long)reset_rva);
  return true;
}

// The ambient spawner's per-candidate test (166DF8, "[AI|SPAWNAMBIENT|REJECT]" in its log) returns al: 1 to
// spawn, 0 to reject. With the gate on, every candidate is rejected: no new ambient characters appear. (A client
// that shows the host's enemies as stand-ins keeps its own population away this way; what already stands in the
// world is not touched.)
volatile LONG g_ambient_block = 0;
volatile LONG g_ambient_hook = 0;
volatile LONG g_ambient_rejected = 0;

bool SimDespawnOffscreen(uintptr_t fn_rva, uintptr_t character, std::string* err) {
  const uint8_t* f = Base() + fn_rva;
  if (!Head(f, "48895C240848897424105748" "83EC208BDA488BF9")) {
    *err = "the despawn request's bytes differ from the known build; refused";
    return false;
  }
  uintptr_t vt = 0, ai = 0;
  if (!ReadQ(character, &vt) || !InImage(vt) || !ReadQ(character + 0x2CE8, &ai) || !ai) {
    *err = "not an AI character";
    return false;
  }
  g_despawn_fn = reinterpret_cast<uintptr_t>(f);
  g_despawn_char = character;
  InterlockedExchange(&g_despawn_pending, 1);
  return true;
}

bool SimAmbientHookInstall(uintptr_t fn_rva, std::string* err) {
  if (g_ambient_hook == 1) return true;
  uint8_t* const fn = const_cast<uint8_t*>(Base()) + fn_rva;
  if (!Head(fn, "488BC448895808488968104889701857")) {
    *err = "the ambient spawn test's bytes differ from the known build; refused";
    return false;
  }
  uint8_t* page = SimNearPage(4);
  if (!page) {
    *err = "no code page near the game image";
    return false;
  }
  // mov r11,&g_ambient_block; cmp byte [r11],0; je +17; mov r11,&g_ambient_rejected; lock inc dword [r11];
  // xor eax,eax; ret; then the first two instructions (mov rax,rsp; mov [rax+8],rbx = 7 bytes) and a jump back.
  std::vector<uint8_t> s = {0x49, 0xBB};
  PutQ(&s, reinterpret_cast<uintptr_t>(&g_ambient_block));
  s.insert(s.end(), {0x41, 0x80, 0x3B, 0x00, 0x74, 0x11, 0x49, 0xBB});
  PutQ(&s, reinterpret_cast<uintptr_t>(&g_ambient_rejected));
  s.insert(s.end(), {0xF0, 0x41, 0xFF, 0x03, 0x31, 0xC0, 0xC3});
  s.insert(s.end(), fn, fn + 7);
  s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  PutQ(&s, reinterpret_cast<uintptr_t>(fn + 7));
  DWORD old = 0;
  if (s.size() > 128 || !VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot write the stub";
    return false;
  }
  memcpy(page + 640, s.data(), s.size());
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  if (!PatchJump(reinterpret_cast<uintptr_t>(fn), page + 640, err)) return false;
  InterlockedExchange(&g_ambient_hook, 1);
  SowLog(L"sim: the ambient spawn test rva:%llX can reject every candidate", (unsigned long long)fn_rva);
  return true;
}

// Faction.Ambush.Enable: a byte in the game's data (1: ambushes may start, 0: each attempt logs "is False (ignorning)"
// and ends). The client turns it off: the host's ambushes show up as its enemies.
bool SimAmbushEnable(uintptr_t flag_rva, bool on, std::string* err) {
  volatile uint8_t* p = const_cast<volatile uint8_t*>(Base() + flag_rva);
  if (reinterpret_cast<uintptr_t>(p) < reinterpret_cast<uintptr_t>(Base()) || reinterpret_cast<uintptr_t>(p) >= ImageEnd()) {
    *err = "the ambush switch is outside the game's image";
    return false;
  }
  __try {
    if (*p > 1) {
      *err = "the ambush switch holds neither 0 nor 1; left alone";
      return false;
    }
    *p = on ? 1 : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *err = "the ambush switch is not writable";
    return false;
  }
  return true;
}

// The game's audio manager crashes the game on purpose when its queue of future keys is full (512: "m_vFutureKeys is at capacity
// of %u. Game will crash. Set convar "GameAudioManager.FatalErrorOnFutureKeyFull" to false to get around this.", D1EE40 called
// from D20D9E). That convar is the byte the check reads (D1EE5C: cmp byte [rip+x],0 -> rva 26708C5; 0 skips the crash). The
// client crashed on it twice in session cm, both right after it entered the arena with the host's enemies about.
bool SimAudioKeysNonFatal(std::string* err) {
  const uint8_t* f = Base() + 0xD1EE5C;
  if (!Head(f, "803D")) {
    *err = "the audio key check (rva D1EE5C) is not this build's";
    return false;
  }
  int32_t disp = 0;
  memcpy(&disp, f + 2, 4);
  const uintptr_t target = reinterpret_cast<uintptr_t>(f + 7) + (intptr_t)disp;
  if (target != reinterpret_cast<uintptr_t>(Base()) + 0x26708C5 || f[6] != 0 || target >= ImageEnd()) {
    *err = "the audio key check does not read the convar at rva 26708C5";
    return false;
  }
  __try {
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(target);
    if (*p > 1) {
      *err = "the convar holds neither 0 nor 1; left alone";
      return false;
    }
    *p = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *err = "the convar is not writable";
    return false;
  }
  return true;
}

void SimAmbientBlock(bool on) { InterlockedExchange(&g_ambient_block, on ? 1 : 0); }

std::string SimAmbientReport() {
  std::ostringstream out;
  out << "ok ambient hook=" << (g_ambient_hook ? "in" : "off") << " block=" << (g_ambient_block ? "on" : "off")
      << " rejected=" << g_ambient_rejected << "\n";
  return out.str();
}

// Nemesis events. HandleFactionEventAction (40C304) is the one place every applied event goes through (promotions
// included): `mgr` is the faction manager, `action` the event (subject ref +0x00, other ref +0x10, event record +0x20,
// flags +0x56/+0x57). The stub sits on the handler's second instruction (the first only stores rbx in the shadow space; the
// entry is not on a position a 5-byte jump can be written at) and calls FactionFilter(mgr, action); a non-zero result
// drops the event (a ret: nothing has been pushed yet).
void TraceObjectName(uintptr_t obj, char out[65]);

namespace {

constexpr int kFeRing = 256;
SimFactionEvent g_fe_ring[kFeRing];
volatile LONG g_fe_count = 0;
LONG g_fe_taken = 0;
volatile LONG g_fe_hook = 0, g_fe_drop = 0, g_fe_dropped = 0, g_fe_seen = 0;
uintptr_t g_fe_lookup = 0;
uintptr_t g_fe_mgr = 0;  // the faction manager the handler last ran on

bool CopyMem(void* out, uintptr_t at, size_t n) {
  __try {
    memcpy(out, reinterpret_cast<const void*>(at), n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void MemberName(uintptr_t mgr, uintptr_t ref_at, char out[40]) {
  out[0] = 0;
  __try {
    const uintptr_t m = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(g_fe_lookup)(mgr, ref_at);
    if (m) {
      const char* s = reinterpret_cast<const char*>(m + 0x498);
      size_t i = 0;
      for (; i + 1 < 40 && s[i] && (unsigned char)s[i] >= 32; ++i) out[i] = s[i];
      out[i] = 0;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
  }
}

constexpr uintptr_t kEventTableGlobal = 0x2701A20;    // the event records (type 0xA4D)
constexpr uintptr_t kFactionTableGlobal = 0x2701AD8;  // the faction (region) records (type 0xA51)
volatile LONG g_fe_replaying = 0;

constexpr uintptr_t kRegionVtable = 0x1FDDF78;  // the region (faction) objects, 0x290 bytes each
constexpr uintptr_t kRegionRecord = 0xD8;       // their own faction record
constexpr uintptr_t kMemberVtable = 0x1FD5EA0;  // a FactionMember (a captain's Nemesis record), name inline at +0x498
constexpr uintptr_t kMemberName = 0x498;

// The name hash of a region object's record (0: not a region object).
uint32_t RegionHash(uintptr_t obj) {
  uintptr_t vt = 0, rec = 0;
  uint32_t head[2] = {};
  if (!obj || !ReadQ(obj, &vt) || vt != reinterpret_cast<uintptr_t>(Base()) + kRegionVtable || !ReadQ(obj + kRegionRecord, &rec) || !rec ||
      !CopyMem(head, rec, 8) || head[0] != 0xA51)
    return 0;
  return head[1];
}

uintptr_t g_regions[64];  // the region objects found by SimFactionPrime
unsigned g_region_count = 0;

uintptr_t FindRegion(uint32_t hash) {
  for (int pass = 0; pass < 2; ++pass) {
    for (unsigned i = 0; i < g_region_count; ++i)
      if (RegionHash(g_regions[i]) == hash) return g_regions[i];
    if (pass == 0) SimFactionPrime();
  }
  return 0;
}

// A reference as the game holds it is {faction record pointer, 0, member id, extra}; the pointer means nothing in another
// copy, so it becomes the record's name hash (+4).
void PortableRef(const uint8_t* raw, uint8_t out[16]) {
  memcpy(out, raw, 16);
  uintptr_t rec = 0;
  memcpy(&rec, raw, 8);
  uint32_t head[2] = {};
  memset(out, 0, 8);
  if (rec && CopyMem(head, rec, 8) && head[0] == 0xA51) memcpy(out, &head[1], 4);
}

bool __fastcall FactionFilter(uintptr_t mgr, uintptr_t action) {
  InterlockedIncrement(&g_fe_seen);
  g_fe_mgr = mgr;
  if (g_fe_replaying) return false;  // an event this copy was asked to run: not recorded, not dropped
  SimFactionEvent e{};
  if (!CopyMem(e.raw, action, sizeof e.raw)) return false;
  PortableRef(e.raw, e.subj);
  PortableRef(e.raw + 16, e.other);
  e.flags[0] = e.raw[0x56];
  e.flags[1] = e.raw[0x57];
  uintptr_t rec = 0;
  memcpy(&rec, e.raw + 0x20, 8);
  uint32_t head[2] = {};
  if (rec && CopyMem(head, rec, 8) && head[0] == 0xA4D) e.event_hash = head[1];
  e.region_hash = RegionHash(mgr);
  char name[65];
  TraceObjectName(rec, name);
  strncpy_s(e.event, name, _TRUNCATE);
  MemberName(mgr, action, e.subj_name);
  MemberName(mgr, action + 0x10, e.other_name);
  EnterCriticalSection(&g_lock);
  g_fe_ring[g_fe_count % kFeRing] = e;
  InterlockedIncrement(&g_fe_count);
  LeaveCriticalSection(&g_lock);
  if (g_fe_drop) {
    InterlockedIncrement(&g_fe_dropped);
    return true;
  }
  return false;
}

}  // namespace

bool SimFactionHookInstall(uintptr_t handler_rva, uintptr_t lookup_rva, std::string* err) {
  if (g_fe_hook == 1) return true;
  uint8_t* const fn = const_cast<uint8_t*>(Base()) + handler_rva;
  if (!Head(fn, "48895C240848896C24184889742420") || !Head(Base() + lookup_rva, "8B5208E9")) {
    *err = "the Nemesis event handler's bytes differ from the known build; refused";
    return false;
  }
  uint8_t* page = SimNearPage(4);
  if (!page) {
    *err = "no code page near the game image";
    return false;
  }
  g_fe_lookup = reinterpret_cast<uintptr_t>(Base()) + lookup_rva;
  // The argument registers and rflags saved, FactionFilter(rcx, rdx) called; non-zero: xor eax,eax; ret (the caller
  // ignores rax). Otherwise the handler's second instruction (mov [rsp+18h],rbp) and a jump back after it.
  std::vector<uint8_t> t = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x9C, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8};
  PutQ(&t, reinterpret_cast<uintptr_t>(&FactionFilter));
  t.insert(t.end(), {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x9D, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,
                     0x84, 0xC0, 0x74, 0x03, 0x31, 0xC0, 0xC3});
  t.insert(t.end(), fn + 5, fn + 10);
  t.insert(t.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  PutQ(&t, reinterpret_cast<uintptr_t>(fn + 10));
  DWORD old = 0;
  if (t.size() > 128 || !VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot write the stub";
    return false;
  }
  memcpy(page + 1024, t.data(), t.size());
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  if (!PatchJump(reinterpret_cast<uintptr_t>(fn) + 5, page + 1024, err)) return false;
  InterlockedExchange(&g_fe_hook, 1);
  SowLog(L"sim: Nemesis events (rva:%llX) are recorded", (unsigned long long)handler_rva);
  return true;
}

void SimFactionPrime() {
  uintptr_t hits[64];
  const size_t n = SimScanHeap(reinterpret_cast<uintptr_t>(Base()) + kRegionVtable, hits, 64, nullptr);
  g_region_count = 0;
  for (size_t i = 0; i < n; ++i)
    if (RegionHash(hits[i])) g_regions[g_region_count++] = hits[i];
  SowLog(L"sim: %u Nemesis region objects found", g_region_count);
}

// A Nemesis member by its faction record's name hash and its id: the region object of that faction holds the member table
// (+0x20; object with the array of member pointers from +0x18 to +0x20, the id the index), the same lookup the game makes.
uintptr_t SimFindMember(uint32_t faction_hash, uint32_t id, char name[40]) {
  static ULONGLONG last_prime = 0;
  uintptr_t region = 0;
  for (unsigned i = 0; i < g_region_count && !region; ++i)
    if (RegionHash(g_regions[i]) == faction_hash) region = g_regions[i];
  if (!region) {
    const ULONGLONG now = GetTickCount64();
    if (last_prime && now - last_prime < 30000) return 0;  // the scan takes seconds: not on every frame
    last_prime = now;
    SimFactionPrime();
    for (unsigned i = 0; i < g_region_count && !region; ++i)
      if (RegionHash(g_regions[i]) == faction_hash) region = g_regions[i];
  }
  uintptr_t table = 0, begin = 0, end = 0, member = 0, vt = 0;
  if (!region || !ReadQ(region + 0x20, &table) || !table || !ReadQ(table + 0x18, &begin) || !ReadQ(table + 0x20, &end) || end <= begin ||
      id >= (end - begin) / 8 || !ReadQ(begin + 8 * (uintptr_t)id, &member) || !member || !ReadQ(member, &vt) ||
      vt != reinterpret_cast<uintptr_t>(Base()) + kMemberVtable)
    return 0;
  if (name) {
    memset(name, 0, 40);
    if (CopyMem(name, member + kMemberName, 39))
      for (int i = 0; i < 39 && name[i]; ++i)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126) name[i] = '?';
  }
  return member;
}

// ---- a Nemesis member as text, and a member made from it -------------------------------------------------------------
// The game saves a member into a node tree (FactionMember.Save writes it to a text file) and builds one from it again
// (its own world load). Both are called here with the same sequence as the game's: save 1C5BAC4, load F99FF0, table
// slot replacement 1BD2E20. The text goes through a file in the temp folder (the game's functions read and write files).
namespace {

constexpr uintptr_t kDocInit = 0x18786F0, kDocAddRoot = 0x1878CA0, kMemberSave = 0x1BC45A8, kDocSaveFile = 0x187A3CC,
                    kDocDtor = 0x1878A2C, kDocLoadFile = 0x1879AC4, kNodeChild = 0x18791F0, kMemAlloc = 0x3DE238,
                    kMemberCtor = 0x1BB4C1C, kMemberLoad2 = 0x1BC20B0, kMemberLoad1 = 0x1BC159C, kSlotLive = 0x1BD16D4,
                    kMgrRemove = 0xF67E70, kSlotReset = 0x1BD522C, kMgrRegister = 0xF67970, kSlotAssign = 0x1BD2220,
                    kHolderRelease = 0x19EF15C, kGameGlobal = 0x26FFAA8;
constexpr size_t kMemberObjectBytes = 0x528;
const char kMemberRoot[] = "FACTIONMEMBER_SINGLESAVE";

bool MemberFnsOk() {
  const uint8_t* b = Base();
  return Head(b + kDocInit, "33C04889014889410889411088") && Head(b + kDocAddRoot, "48895C2410574883EC20488BF9488BDA") &&
         Head(b + kMemberSave, "48895C2410488974242055574156488BEC") && Head(b + kDocSaveFile, "48895C240848897C241055488D6C24A0") &&
         Head(b + kDocDtor, "40534883EC20488B5108488BD9") && Head(b + kDocLoadFile, "48895C2408574883EC60488364242000") &&
         Head(b + kNodeChild, "40534883EC20488BD94885D27443") && Head(b + kMemAlloc, "E903000000CCCCCC40534883EC20") &&
         Head(b + kMemberCtor, "48895C241048896C2418565741564883EC20") && Head(b + kMemberLoad2, "4889742408574883EC20488BC2488BF9") &&
         Head(b + kMemberLoad1, "48895C2410488974241855574156488BEC") && Head(b + kSlotLive, "33C04839010F95C0C3") &&
         Head(b + kMgrRemove, "4883EC284C8B82780100004C8BD2") && Head(b + kSlotReset, "4883EC28488B014889114885C074") &&
         Head(b + kMgrRegister, "48895C2410488974241848897C2420") && Head(b + kSlotAssign, "40534883EC20488BC2488BD9483BCA74") &&
         Head(b + kHolderRelease, "4883EC28488B094885C9740A");
}

std::string MemberTempPath() {
  char dir[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, dir);
  char path[MAX_PATH + 40];
  snprintf(path, sizeof path, "%ssowcoop-member-%lu.txt", dir, GetCurrentProcessId());
  return path;
}

bool MemberExportRaw(uintptr_t member, const char* path) {
  alignas(16) uint8_t doc[0x100] = {};
  const uint8_t* b = Base();
  bool ok = false;
  __try {
    reinterpret_cast<void(__fastcall*)(void*)>(const_cast<uint8_t*>(b + kDocInit))(doc);
    void* root = reinterpret_cast<void*(__fastcall*)(void*, const char*)>(const_cast<uint8_t*>(b + kDocAddRoot))(doc, kMemberRoot);
    if (root) {
      reinterpret_cast<void(__fastcall*)(void*, void*)>(const_cast<uint8_t*>(b + kMemberSave))(reinterpret_cast<void*>(member), root);
      ok = (reinterpret_cast<uint8_t(__fastcall*)(void*, const char*)>(const_cast<uint8_t*>(b + kDocSaveFile))(doc, path) & 1) != 0;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(const_cast<uint8_t*>(b + kDocDtor))(doc);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return ok;
}

struct MemberImportCtx {
  uintptr_t member = 0;
};

// Called by the game's file loader with the parsed document.
void __fastcall MemberImportCb(void* doc, void* arg) {
  MemberImportCtx* ctx = static_cast<MemberImportCtx*>(arg);
  const uint8_t* b = Base();
  __try {
    void* node = reinterpret_cast<void*(__fastcall*)(void*, const char*)>(const_cast<uint8_t*>(b + kNodeChild))(doc, kMemberRoot);
    if (!node) return;
    void* mem = reinterpret_cast<void*(__fastcall*)(size_t)>(const_cast<uint8_t*>(b + kMemAlloc))(kMemberObjectBytes);
    if (!mem) return;
    mem = reinterpret_cast<void*(__fastcall*)(void*)>(const_cast<uint8_t*>(b + kMemberCtor))(mem);
    reinterpret_cast<void(__fastcall*)(void*, void*)>(const_cast<uint8_t*>(b + kMemberLoad2))(mem, node);
    reinterpret_cast<void(__fastcall*)(void*, void*)>(const_cast<uint8_t*>(b + kMemberLoad1))(mem, node);
    ctx->member = reinterpret_cast<uintptr_t>(mem);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

bool MemberLoadRaw(const char* path, MemberImportCtx* ctx) {
  const uint8_t* b = Base();
  __try {
    reinterpret_cast<uint8_t(__fastcall*)(const char*, void*, void*, void*)>(const_cast<uint8_t*>(b + kDocLoadFile))(
        path, nullptr, reinterpret_cast<void*>(&MemberImportCb), ctx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return ctx->member != 0;
}

// The tail of the game's table allocation (1BD2E20) for slot n, with the member already built: a live member in the slot
// is removed from the faction manager and released, the manager registers the slot, the new member takes it.
bool MemberPlaceRaw(uintptr_t table, uint32_t n, uintptr_t member) {
  const uint8_t* b = Base();
  uintptr_t begin = 0, game = 0, mgr = 0, reg_arg = 0;
  if (!ReadQ(table + 0x18, &begin) || !ReadQ(reinterpret_cast<uintptr_t>(b) + kGameGlobal, &game) || !game ||
      !ReadQ(game + 0x6C68, &mgr) || !mgr || !ReadQ(table + 0xA8, &reg_arg))
    return false;
  const uintptr_t slot = begin + 8 * (uintptr_t)n;
  __try {
    if (reinterpret_cast<uint8_t(__fastcall*)(void*)>(const_cast<uint8_t*>(b + kSlotLive))(reinterpret_cast<void*>(slot)) & 1) {
      uintptr_t old = *reinterpret_cast<uintptr_t*>(slot);
      reinterpret_cast<void(__fastcall*)(void*, void*)>(const_cast<uint8_t*>(b + kMgrRemove))(reinterpret_cast<void*>(mgr), reinterpret_cast<void*>(old));
      reinterpret_cast<void(__fastcall*)(void*, void*)>(const_cast<uint8_t*>(b + kSlotReset))(reinterpret_cast<void*>(slot), nullptr);
    }
    reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t)>(const_cast<uint8_t*>(b + kMgrRegister))(reinterpret_cast<void*>(mgr),
                                                                                                         reinterpret_cast<void*>(reg_arg), n);
    uintptr_t holder = member;
    reinterpret_cast<void(__fastcall*)(void*, void*)>(const_cast<uint8_t*>(b + kSlotAssign))(reinterpret_cast<void*>(slot), &holder);
    *reinterpret_cast<uint32_t*>(member + 0x174) = n;
    reinterpret_cast<void(__fastcall*)(void*)>(const_cast<uint8_t*>(b + kHolderRelease))(&holder);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const DWORD size = GetFileSize(h, nullptr);
  out->assign(size == INVALID_FILE_SIZE ? 0 : size, '\0');
  DWORD got = 0;
  const bool ok = size != INVALID_FILE_SIZE && (size == 0 || ReadFile(h, &(*out)[0], size, &got, nullptr)) && got == size;
  CloseHandle(h);
  return ok;
}

bool WriteWholeFile(const std::string& path, const std::string& data) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  const bool ok = data.empty() || WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr);
  CloseHandle(h);
  return ok && put == data.size();
}

}  // namespace

bool SimMemberCharacter(uint32_t faction_hash, uint32_t id, uintptr_t* member, uintptr_t* character, float pos[3]) {
  *member = SimFindMember(faction_hash, id, nullptr);
  *character = 0;
  if (!*member) return false;
  uintptr_t ch = 0;
  if (ReadQ(*member + 0x48, &ch) && ch && ReadFloats(ch + 0xAC, pos, 3)) *character = ch;
  return true;
}

// A member's character (+0x48) is the target of a game reference at member+0x28 (vtable 1DB6BE0, listfix.h; the member's
// constructor 1BB4C1C builds it with 4CA490), linked into the character's list of references. Until session cm this wrote +0x48
// alone: setting 0 when a captain's proxy was freed left the reference linked into the proxy's list with no target, and the
// game's destructor looped for ever when it removed the proxy (the client's hang, sessions ck-cm: the stuck reference was at
// +0x28 of an object with the member's vtable 1FD5EA0). It is now set with the game's assign (3F52B8: unlink, then link into
// the new character's list), so the proxy's destructor clears it the game's way.
volatile LONG g_member_link_assigned = 0, g_member_link_raw = 0;

void SimMemberLink(uint32_t faction_hash, uint32_t id, uintptr_t character, uintptr_t only) {
  const uintptr_t member = SimFindMember(faction_hash, id, nullptr);
  if (!member) return;
  const uintptr_t ref = member + 0x28;
  static int assign_ok = -1;
  if (assign_ok < 0) assign_ok = Head(Base() + kRefAssign, kRefAssignHead);
  __try {
    volatile uintptr_t* slot = reinterpret_cast<volatile uintptr_t*>(member + 0x48);
    const uintptr_t want = character ? character : 0;
    if (character ? *slot == character : *slot != only) return;
    const bool is_ref = *reinterpret_cast<const uintptr_t*>(ref + 0x18) == ref &&
                        *reinterpret_cast<const uintptr_t*>(ref) == reinterpret_cast<uintptr_t>(Base()) + 0x1DB6BE0;
    if (assign_ok && is_ref) {
      reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t)>(const_cast<uint8_t*>(Base() + kRefAssign))(ref, want);
      InterlockedIncrement(&g_member_link_assigned);
    } else {
      *slot = want;
      InterlockedIncrement(&g_member_link_raw);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void SimMemberLinkStats(long* assigned, long* raw) {
  *assigned = g_member_link_assigned;
  *raw = g_member_link_raw;
}

namespace {
constexpr unsigned kTapRing = 4096;
volatile LONG g_tap_idx = 0;
uintptr_t g_tap_graph[kTapRing] = {}, g_tap_def[kTapRing] = {};
LONG g_tap_read = 0;
volatile LONG g_tap_hook = 0;
}  // namespace

bool SimActionTapInstall(uintptr_t fn_rva, std::string* err) {
  if (g_tap_hook) return true;
  uint8_t* const fn = const_cast<uint8_t*>(Base()) + fn_rva;
  if (!Head(fn, "488BC4488958104889701848897820" "55")) {
    *err = "start-action bytes differ from the known build; refused";
    return false;
  }
  uint8_t* page = SimNearPage(4);
  if (!page) {
    *err = "no code page near the game image";
    return false;
  }
  // push rax; push r10; push r11; mov r11,&idx; mov eax,1; lock xadd [r11],eax; and eax,0FFFh; mov r10,[rsp+18h] (unused);
  // mov r11,&graph; mov [r11+rax*8],rcx; mov r10,[rdx+60h]; mov r11,&def; mov [r11+rax*8],r10; pop r11; pop r10; pop rax;
  // the first 7 bytes of the function and a jump after them.
  std::vector<uint8_t> s = {0x50, 0x41, 0x52, 0x41, 0x53, 0x49, 0xBB};
  PutQ(&s, reinterpret_cast<uintptr_t>(&g_tap_idx));
  s.insert(s.end(), {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x41, 0x0F, 0xC1, 0x03, 0x25, 0xFF, 0x0F, 0x00, 0x00, 0x49, 0xBB});
  PutQ(&s, reinterpret_cast<uintptr_t>(g_tap_graph));
  s.insert(s.end(), {0x49, 0x89, 0x0C, 0xC3, 0x4C, 0x8B, 0x52, 0x60, 0x49, 0xBB});
  PutQ(&s, reinterpret_cast<uintptr_t>(g_tap_def));
  s.insert(s.end(), {0x4D, 0x89, 0x14, 0xC3, 0x41, 0x5B, 0x41, 0x5A, 0x58});
  s.insert(s.end(), fn, fn + 7);
  s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  PutQ(&s, reinterpret_cast<uintptr_t>(fn + 7));
  DWORD old = 0;
  if (s.size() > 256 || !VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot write the stub";
    return false;
  }
  memcpy(page + 1792, s.data(), s.size());
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  if (!PatchJump(reinterpret_cast<uintptr_t>(fn), page + 1792, err)) return false;
  g_tap_read = g_tap_idx;
  InterlockedExchange(&g_tap_hook, 1);
  SowLog(L"sim: graph action starts are recorded (rva:%llX)", (unsigned long long)fn_rva);
  return true;
}

size_t SimActionTapDrain(SimActionStart* out, size_t max) {
  if (!g_tap_hook) return 0;
  LONG now = g_tap_idx;
  if (now - g_tap_read > (LONG)kTapRing - 8) g_tap_read = now - (LONG)kTapRing + 8;  // fell behind: the oldest are gone
  size_t n = 0;
  while (g_tap_read != now && n < max) {
    const unsigned slot = (unsigned)g_tap_read & (kTapRing - 1);
    ++g_tap_read;
    const uintptr_t graph = g_tap_graph[slot], def = g_tap_def[slot];
    uintptr_t owner = 0, ch = 0;
    if (!graph || !def || !ReadQ(graph + 0x28, &owner) || !owner || !ReadQ(owner + 0x40, &ch) || !ch) continue;
    char name[65];
    TraceObjectName(def, name);
    if (!name[0]) continue;
    out[n].character = ch;
    memset(out[n].name, 0, sizeof out[n].name);
    strncpy_s(out[n].name, name, _TRUNCATE);
    ++n;
  }
  return n;
}

void SimSlowMoCap(uintptr_t request_rva, unsigned max_ms) {
  static uintptr_t seen[16] = {};
  static ULONGLONG since[16] = {};
  const uint8_t* fn = Base() + request_rva;
  uintptr_t mgr = 0, begin = 0, end = 0;
  if (!Head(fn, "488BC448895808448848204C894018895010574883EC40") ||
      !ReadQ(reinterpret_cast<uintptr_t>(Base()) + 0x2A86888, &mgr) || !mgr || !ReadQ(mgr, &begin) || !ReadQ(mgr + 8, &end) || end < begin ||
      end - begin > 16 * 8)
    return;
  const ULONGLONG now = GetTickCount64();
  uintptr_t now_seen[16] = {};
  ULONGLONG now_since[16] = {};
  unsigned n = 0;
  for (uintptr_t p = begin; p < end && n < 16; p += 8) {
    uintptr_t req = 0, id = 0;
    if (!ReadQ(p, &req) || !req || !ReadQ(req + 0x80, &id)) continue;
    ULONGLONG first = now;
    for (int i = 0; i < 16; ++i)
      if (seen[i] == req) first = since[i];
    now_seen[n] = req;
    now_since[n] = first;
    ++n;
    if (now - first > max_ms) {
      __try {
        reinterpret_cast<void(__fastcall*)(void*, uint32_t, void*, uint8_t)>(const_cast<uint8_t*>(fn))(reinterpret_cast<void*>(mgr), (uint32_t)id,
                                                                                                         nullptr, 0);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      static int logged = 0;
      if (++logged <= 10) SowLog(L"sim: a slow-motion request (source %u) was ended after %u ms", (unsigned)(id & 0xFFFFFFFF), max_ms);
      now_since[n - 1] = now + 1000000;  // not again while it ramps out
    }
  }
  memcpy(seen, now_seen, sizeof seen);
  memcpy(since, now_since, sizeof since);
}

uint64_t SimMemberSeed(uintptr_t member) {
  uintptr_t seed = 0;
  return member && ReadQ(member + 0x168, &seed) ? (uint64_t)seed : 0;
}

bool SimMemberExport(uintptr_t member, std::string* text, std::string* err) {
  if (!MemberFnsOk()) {
    *err = "the member save functions differ from the known build; refused";
    return false;
  }
  const std::string path = MemberTempPath();
  DeleteFileA(path.c_str());
  if (!MemberExportRaw(member, path.c_str())) {
    *err = "the game could not save the member";
    return false;
  }
  const bool ok = ReadWholeFile(path, text);
  DeleteFileA(path.c_str());
  if (!ok) *err = "the saved member could not be read back";
  return ok;
}

bool SimMemberImport(uint32_t faction_hash, uint32_t id, const std::string& text, std::string* err) {
  if (!MemberFnsOk()) {
    *err = "the member load functions differ from the known build; refused";
    return false;
  }
  uintptr_t region = 0;
  for (int pass = 0; pass < 2 && !region; ++pass) {
    for (unsigned i = 0; i < g_region_count && !region; ++i)
      if (RegionHash(g_regions[i]) == faction_hash) region = g_regions[i];
    if (!region && pass == 0) SimFactionPrime();
  }
  uintptr_t table = 0, begin = 0, end = 0, limit_q = 0;
  if (!region || !ReadQ(region + 0x20, &table) || !table || !ReadQ(table + 0x18, &begin) || !ReadQ(table + 0x20, &end) ||
      !ReadQ(table + 0xB0, &limit_q)) {
    *err = "no member table for that faction";
    return false;
  }
  if (id >= (end - begin) / 8 || id >= (uint32_t)limit_q) {
    *err = "the member's slot is beyond this copy's table";
    return false;
  }
  const std::string path = MemberTempPath();
  if (!WriteWholeFile(path, text)) {
    *err = "cannot write the member's text";
    return false;
  }
  MemberImportCtx ctx;
  const bool loaded = MemberLoadRaw(path.c_str(), &ctx);
  DeleteFileA(path.c_str());
  if (!loaded) {
    *err = "the game could not read the member";
    return false;
  }
  if (!MemberPlaceRaw(table, id, ctx.member)) {
    *err = "the member could not be put into its slot";
    return false;
  }
  return true;
}

bool SimCall(const std::function<void()>& fn, unsigned timeout_ms) {
  HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!ev) return false;
  EnterCriticalSection(&g_lock);
  g_tasks.emplace_back(fn, ev);
  InterlockedExchange(&g_task_count, 1);
  LeaveCriticalSection(&g_lock);
  if (WaitForSingleObject(ev, timeout_ms) != WAIT_OBJECT_0) return false;  // the handle is left to the task
  CloseHandle(ev);
  return true;
}

bool SimFactionReplay(uintptr_t handler_rva, const SimFactionEvent& e, std::string* err) {
  if (!g_fe_hook) {
    *err = "the Nemesis hook is not in";
    return false;
  }
  const uintptr_t region = FindRegion(e.region_hash);
  if (!region) {
    *err = "the event's region is not known here";
    return false;
  }
  uint8_t action[0x100] = {};
  memcpy(action, e.subj, 16);
  memcpy(action + 16, e.other, 16);
  auto record = [&](uintptr_t global, uint32_t kind, uint32_t hash) -> uintptr_t {
    uintptr_t table = 0;
    if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + global, &table)) return 0;
    return reinterpret_cast<uintptr_t>(SearchTypeTable(table, kind, hash));
  };
  for (int i = 0; i < 2; ++i) {
    uint32_t hash = 0;
    memcpy(&hash, action + 16 * i, 4);
    uintptr_t rec = 0;
    memset(action + 16 * i, 0, 8);
    if (hash) {
      rec = record(kFactionTableGlobal, 0xA51, hash);
      if (!rec) {
        *err = "a faction record of the host's event is not known here";
        return false;
      }
    }
    memcpy(action + 16 * i, &rec, 8);
  }
  const uintptr_t ev = e.event_hash ? record(kEventTableGlobal, 0xA4D, e.event_hash) : 0;
  if (!ev) {
    *err = "the event record is not known here";
    return false;
  }
  memcpy(action + 0x20, &ev, 8);
  action[0x56] = e.flags[0];
  action[0x57] = e.flags[1];
  const auto fn = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(const_cast<uint8_t*>(Base()) + handler_rva);
  InterlockedExchange(&g_fe_replaying, 1);
  bool ok = true;
  __try {
    fn(region, reinterpret_cast<uintptr_t>(action));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
    *err = "the handler faulted";
  }
  InterlockedExchange(&g_fe_replaying, 0);
  return ok;
}

bool SimFactionResolve(const uint8_t ref[16], char name[40]) {
  name[0] = 0;
  if (!g_fe_hook || !g_fe_mgr) return false;
  uint8_t copy[16];
  memcpy(copy, ref, 16);
  MemberName(g_fe_mgr, reinterpret_cast<uintptr_t>(copy), name);
  return name[0] != 0;
}

void SimFactionDrop(bool on) { InterlockedExchange(&g_fe_drop, on ? 1 : 0); }

size_t SimFactionTake(SimFactionEvent* out, size_t max) {
  size_t n = 0;
  EnterCriticalSection(&g_lock);
  if (g_fe_count - g_fe_taken > kFeRing) g_fe_taken = g_fe_count - kFeRing;  // the oldest were overwritten
  while (g_fe_taken < g_fe_count && n < max) out[n++] = g_fe_ring[g_fe_taken++ % kFeRing];
  LeaveCriticalSection(&g_lock);
  return n;
}

std::string SimFactionReport() {
  std::ostringstream out;
  out << "ok faction hook=" << (g_fe_hook ? "in" : "off") << " drop=" << (g_fe_drop ? "on" : "off") << " seen=" << g_fe_seen
      << " recorded=" << g_fe_count << " dropped=" << g_fe_dropped << " mgr=" << std::hex << g_fe_mgr << std::dec << "\n";
  return out.str();
}

// Slow motion (SlowMoUtil): every slow-motion record runs as an instance whose per-frame update (5CF6C4) walks five
// timers (player, simulation, ...). For a timer whose "wanted" byte ([slot-4]) is set it applies the record's scale
// (5CF8CC, then 5CF834); for one whose byte is clear it sets the timer back to 1/1. The `jne` after that byte test
// (75 42 at 5CF709) is the whole difference: with it turned into two nops every timer is set to 1/1 each frame, so no
// record slows anything, and nothing else (the timers' other users, the records, their sounds) is touched.
bool SimSlowMoOff(uintptr_t jne_rva, std::string* err) {
  uint8_t* const at = const_cast<uint8_t*>(Base()) + jne_rva;
  if (Head(at, "9090") && Head(at - 4, "807EFC00")) return true;  // already off
  if (!Head(at - 4, "807EFC00" "7542")) {
    *err = "slow motion's per-timer test differs from the known build; refused";
    return false;
  }
  DWORD old = 0;
  if (!VirtualProtect(at, 2, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot make the game's code writable";
    return false;
  }
#ifndef SOWGUARD_MUTANT  // negative control: slow motion still runs
  const uint16_t nops = 0x9090;
  InterlockedExchange16(reinterpret_cast<volatile SHORT*>(at), (SHORT)nops);
#endif
  FlushInstructionCache(GetCurrentProcess(), at, 2);
  VirtualProtect(at, 2, old, &old);
  SowLog(L"sim: slow motion is off (rva:%llX)", (unsigned long long)jne_rva);
  return true;
}

bool SimSlowMoStart(uintptr_t fn_rva, const std::string& record, std::string* err) {
  const uint8_t* f = Base() + fn_rva;
  if (!Head(f, "4883EC284533C0E8")) {
    *err = "the slow-motion start's bytes differ from the known build; refused";
    return false;
  }
  if (record.empty() || record.size() >= sizeof g_slowmo_start_name) {
    *err = "bad record name";
    return false;
  }
  memcpy(g_slowmo_start_name, record.c_str(), record.size() + 1);
  g_slowmo_start_fn = reinterpret_cast<uintptr_t>(f);
  g_slowmo_set_fn = reinterpret_cast<uintptr_t>(Base() + 0x5D09E0);
  if (!Head(Base() + 0x5D09E0, "48895C240848897424105748" "83EC20")) {
    *err = "the world UI slow motion setter's bytes differ from the known build; refused";
    return false;
  }
  InterlockedExchange(&g_slowmo_start_pending, 1);
  return true;
}

// The five timers' rates: scale (num/den) of the rate object each timer points to.
std::string SimSlowMoReport(uintptr_t jne_rva) {
  static const uintptr_t kGlobals[5] = {0x2702598, 0x2702548, 0x2702570, 0x2702558, 0x2702578};
  std::ostringstream out;
  out << "ok timers";
  for (int i = 0; i < 5; ++i) {
    uintptr_t timer = 0, rate = 0;
    float ratio = 0;
    if (ReadQ(reinterpret_cast<uintptr_t>(Base()) + kGlobals[i], &timer) && timer && ReadQ(timer + 8, &rate) && rate &&
        ReadFloats(rate + 0x30, &ratio, 1))
      out << " " << i << "=" << ratio;
    else
      out << " " << i << "=?";
  }
  const uint8_t* const at = Base() + jne_rva;
  out << " patch=" << (Head(at, "9090") ? "off" : Head(at, "7542") ? "slow-motion-on" : "unknown") << "\n";
  return out.str();
}

void SimKeepPuppetAnimOnTeleport(bool on) {
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  uintptr_t ctrl = 0;
  if (!on || !owner || !PuppetAlive(owner, vt, health) || !ReadQ(owner + 0x1970, &ctrl)) ctrl = 0;
  g_keep_anim_ctrl = ctrl;
}

long SimAnimResetsSkipped() { return g_anim_resets_skipped; }

// The character's health object holds the max at +0x428 and the current health at +0x42C (0
// once dead: the local player's strikes kill a stand-in, follower or not, session al).
bool SimForgetGonePuppet() {
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner) return false;
  const bool there = PuppetAlive(owner, vt, health);
  float now = 1;
  if (there && !(ReadFloats(health + 0x42C, &now, 1) && now <= 0)) return false;
  EnterCriticalSection(&g_lock);
  const bool forgot = g_puppet == owner;
  if (forgot) g_puppet = 0;
  LeaveCriticalSection(&g_lock);
  if (forgot) g_untargeted = g_unhit = g_aoe_skip = 0;
  if (forgot)
    SowLog(there ? L"sim: puppet abs:%llX is dead (health 0); no puppet now"
                 : L"sim: puppet abs:%llX is gone (unloaded or freed); no puppet now",
           (unsigned long long)owner);
  return forgot;
}

// The stand-in is let go so that a new one is made (its AI table nears the game's 256, session cm): no puppet from now on, the
// old one taken 20,000 units down with health 0 and kept in the proxy set (its tree skipped) so that it does nothing; the caller
// asks the game to despawn it. Returns the old one (0: none).
uintptr_t SimRetirePuppet() {
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet;
  g_puppet = 0;
  LeaveCriticalSection(&g_lock);
  if (!owner) return 0;
  g_untargeted = g_unhit = g_aoe_skip = 0;
  SimProxyAdopt(owner, false);
  float p[3] = {};
  if (ReadFloats(owner + 0xAC, p, 3)) {
    Xform x{};
    x.pos[0] = p[0];
    x.pos[1] = p[1] - 20000.0f;
    x.pos[2] = p[2];
    x.rot[3] = 1.0f;
    WriteXform(owner, x);
  }
  SimProxyHealth(owner, 0.0f);
  SowLog(L"sim: puppet abs:%llX retired; a new one is made", (unsigned long long)owner);
  return owner;
}

bool SimKeepPuppetFollower(uintptr_t getter_rva, bool on, std::string* err) {
  if (GetCurrentThreadId() != InputGameThread()) {
    *err = "not on the simulation thread";
    return false;
  }
  if (!on) {
    const uintptr_t had = g_follower_owner;
    g_follower_owner = 0;
    EnterCriticalSection(&g_lock);
    const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
    LeaveCriticalSection(&g_lock);
    if (had && owner == had && PuppetAlive(owner, vt, health)) {
      *reinterpret_cast<volatile uint8_t*>(owner + kFollower) = 0;
      SowLog(L"sim: puppet abs:%llX no longer marked as the player's follower", (unsigned long long)owner);
    }
    *err = "off";
    return false;
  }
  if (!g_follower_build)
    g_follower_build = Head(Base() + getter_rva, "33C03881B82300000F95C0C3") ? 1 : -1;
  if (g_follower_build < 0) {
    *err = "the player-follower getter differs from the known build; refused";
    return false;
  }
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner || !PuppetAlive(owner, vt, health)) {
    *err = "no puppet";
    return false;
  }
  __try {
#ifndef SOWGUARD_MUTANT  // negative control: the stand-in stays a stranger
    volatile uint8_t* flag = reinterpret_cast<volatile uint8_t*>(owner + kFollower);
    if (*flag != 1) {
      *flag = 1;
      InterlockedIncrement(&g_follower_marks);
    }
#endif
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *err = "the puppet's follower byte is not writable";
    return false;
  }
  if (owner != g_follower_owner) {
    g_follower_owner = owner;
    SowLog(L"sim: puppet abs:%llX marked as the player's follower (+0x23B8), out of the player's combat",
           (unsigned long long)owner);
  }
  return true;
}

long SimFollowerMarks() { return g_follower_marks; }

// ---------------------------------------------------------------- call trace (probe "trace")

namespace {

// Who calls a function, at any address (no patch: an execute breakpoint in debug register 1
// of the simulation thread). Each hit with the wanted rcx counts its return address; the
// breakpoint resumes with the resume flag. Armed and cleared from the simulation thread.
// Write mode (probe "wtrace"): a data breakpoint in debug register 2 counts the instructions
// that write the address (the hit's rip is the instruction after the write) and keeps the
// call stack of each one's first hit. Names mode (an execute trace of a variable setter such
// as 176D78): counts by the second argument (the name) instead, with the last value (xmm2)
// and the return address of the first call. Args mode: counts by the qword at r9+0x28 (0
// when r9 is 0 or unreadable; for a function whose fourth argument is a component of a
// character), with the first call's caller, rcx, [rcx+0x28] and r9.
constexpr int kTraceSites = 64, kTraceDepth = 10;
struct TraceSite {
  uintptr_t ret;
  LONG count;
  uintptr_t stack[kTraceDepth];
  int depth;
  float value;
  uintptr_t first_ret;
  uintptr_t rcx, rcx28, r9;
  uintptr_t rdx;
  uint8_t rdx_mem[0x80];  // args mode: what rdx points at (0x80 bytes, zeros if unreadable)
  float r8v[3];           // and three floats at r8
  uintptr_t arg5, arg6;   // the fifth and sixth arguments (the stack at rsp+0x28 and +0x30 on entry)
};
TraceSite g_trace_sites[kTraceSites];
volatile LONG g_trace_sites_used = 0, g_trace_hits = 0, g_trace_matched = 0, g_trace_dropped = 0;
uintptr_t g_trace_fn = 0, g_trace_rcx = 0;
unsigned g_trace_len = 0;  // 0: execute (DR1); 1, 2, 4 or 8: write of that many bytes (DR2)
bool g_trace_names = false;
bool g_trace_args = false;
bool g_trace_r8 = false;  // with args: counted by r8 instead (an object whose name is at +0x20)
bool g_trace_reads = false;  // with g_trace_len: reads too
volatile LONG g_trace_state = 0;  // 0 idle, 1 arm wanted, 2 armed, 3 clear wanted
PVOID g_trace_handler = nullptr;

void TraceStack(CONTEXT c, TraceSite* s) {
  s->depth = 0;
  __try {
    for (int k = 0; k < 40 && s->depth < kTraceDepth; ++k) {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
      if (f) {
        void* hd = nullptr;
        DWORD64 fr = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &hd, &fr, nullptr);
      } else {
        c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
        c.Rsp += 8;
      }
      if (c.Rip < 0x10000) break;
      s->stack[s->depth++] = c.Rip;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

LONG CALLBACK TraceHandler(EXCEPTION_POINTERS* e) {
  CONTEXT* c = e->ContextRecord;
  const bool write = g_trace_len != 0;
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & (write ? 4 : 2)) ||
      (!write && c->Rip != g_trace_fn))
    return EXCEPTION_CONTINUE_SEARCH;
  InterlockedIncrement(&g_trace_hits);
  if (write || !g_trace_rcx || c->Rcx == g_trace_rcx) {
    InterlockedIncrement(&g_trace_matched);
    const uintptr_t caller = *reinterpret_cast<const uintptr_t*>(c->Rsp);
    uintptr_t r9_28 = 0, rcx_28 = 0;
    if (g_trace_args) {
      __try {
        if (c->R9) r9_28 = *reinterpret_cast<const uintptr_t*>(c->R9 + 0x28);
        if (c->Rcx) rcx_28 = *reinterpret_cast<const uintptr_t*>(c->Rcx + 0x28);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    const uintptr_t ret = write ? c->Rip : g_trace_names ? c->Rdx : g_trace_r8 ? c->R8 : g_trace_args ? r9_28 : caller;
    float value = 0;
    memcpy(&value, &c->Xmm2, sizeof value);
    LONG i = 0;
    const LONG used = g_trace_sites_used;
    while (i < used && g_trace_sites[i].ret != ret) ++i;
    if (i < used) {
      ++g_trace_sites[i].count;
      g_trace_sites[i].value = value;
    } else if (used < kTraceSites) {
      g_trace_sites[used].ret = ret;
      g_trace_sites[used].count = 1;
      g_trace_sites[used].depth = 0;
      g_trace_sites[used].value = value;
      g_trace_sites[used].first_ret = caller;
      g_trace_sites[used].rcx = c->Rcx;
      g_trace_sites[used].rcx28 = rcx_28;
      g_trace_sites[used].r9 = c->R9;
      g_trace_sites[used].rdx = c->Rdx;
      memset(g_trace_sites[used].rdx_mem, 0, sizeof g_trace_sites[used].rdx_mem);
      memset(g_trace_sites[used].r8v, 0, sizeof g_trace_sites[used].r8v);
      if (g_trace_args) {
        __try {
          if (c->Rdx) memcpy(g_trace_sites[used].rdx_mem, reinterpret_cast<const void*>(c->Rdx), sizeof g_trace_sites[used].rdx_mem);
          if (c->R8) memcpy(g_trace_sites[used].r8v, reinterpret_cast<const void*>(c->R8), sizeof g_trace_sites[used].r8v);
          g_trace_sites[used].arg5 = *reinterpret_cast<const uintptr_t*>(c->Rsp + 0x28);
          g_trace_sites[used].arg6 = *reinterpret_cast<const uintptr_t*>(c->Rsp + 0x30);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
      }
      if (write) TraceStack(*c, &g_trace_sites[used]);
      g_trace_sites_used = used + 1;
    } else {
      InterlockedIncrement(&g_trace_dropped);
    }
  }
  c->Dr6 = 0;
  if (!write) c->EFlags |= 0x10000;  // resume flag: run the instruction once without breaking again
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool TracePending() { return g_trace_state == 1 || g_trace_state == 3; }

void RunTrace() {
  CONTEXT c{};
  c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (!GetThreadContext(GetCurrentThread(), &c)) return;
  if (g_trace_state == 1) {
    if (!g_trace_handler) g_trace_handler = AddVectoredExceptionHandler(1, TraceHandler);
    if (g_trace_len) {
      // length bits: 00 one byte, 01 two, 11 four, 10 eight
      const DWORD64 len = g_trace_len == 1 ? 0 : g_trace_len == 2 ? 1 : g_trace_len == 4 ? 3 : 2;
      c.Dr2 = g_trace_fn;
      const DWORD64 rw = g_trace_reads ? 3 : 1;  // 01 write, 11 read or write
      c.Dr7 = (c.Dr7 & ~((DWORD64)0xF << 24)) | (1 << 4) | (rw << 24) | (len << 26);  // local enable 2
    } else {
      c.Dr1 = g_trace_fn;
      c.Dr7 = (c.Dr7 & ~((DWORD64)0xF << 20)) | (1 << 2);  // local enable 1; execute, length 1
    }
    SetThreadContext(GetCurrentThread(), &c);
    InterlockedExchange(&g_trace_state, 2);
  } else if (g_trace_state == 3) {
    if (g_trace_len) {
      c.Dr2 = 0;
      c.Dr7 &= ~(DWORD64)(3 << 4);
    } else {
      c.Dr1 = 0;
      c.Dr7 &= ~(DWORD64)(3 << 2);
    }
    SetThreadContext(GetCurrentThread(), &c);
    InterlockedExchange(&g_trace_state, 0);
  }
}

// A name for probe "trace ... names": the C string at p (at most 64 characters), or "?" if
// unreadable.
void TraceName(uintptr_t p, char out[65]) {
  memset(out, 0, 65);
  __try {
    for (int i = 0; i < 64; ++i) {
      out[i] = reinterpret_cast<const char*>(p)[i];
      if (!out[i]) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '?';
    out[1] = 0;
  }
}

// An object's name by the generic name header (bit 2 of the byte at +1, then the name pointer
// at +0x20); empty if it has none.
void TraceObjectName(uintptr_t obj, char out[65]) {
  uintptr_t np = 0;
  __try {
    if (obj && (reinterpret_cast<const uint8_t*>(obj)[1] & 2)) np = *reinterpret_cast<const uintptr_t*>(obj + 0x20);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  memset(out, 0, 65);
  if (np) TraceName(np, out);
}

std::string SimTraceCalls(uintptr_t fn, uintptr_t rcx, unsigned ms, unsigned write_len, bool names, bool reads,
                          bool args, bool by_r8) {
  if (!g_installed) return "err simulation-thread hook not installed (sim_hook)\n";
  if (!write_len && (fn < reinterpret_cast<uintptr_t>(Base()) || fn >= ImageEnd())) return "err not in the game's image\n";
  if (write_len && (fn & (write_len - 1))) return "err the address is not aligned to the length\n";
  if (g_trace_state != 0) return "err a trace is running\n";
  g_trace_fn = fn;
  g_trace_rcx = rcx;
  g_trace_len = write_len;
  g_trace_names = names && !write_len;
  g_trace_args = args && !write_len && !names;
  g_trace_r8 = g_trace_args && by_r8;
  g_trace_reads = reads && write_len;
  g_trace_sites_used = g_trace_hits = g_trace_matched = g_trace_dropped = 0;
  InterlockedExchange(&g_trace_state, 1);
  for (int i = 0; i < 200 && g_trace_state == 1; ++i) Sleep(10);
  if (g_trace_state != 2) {
    InterlockedExchange(&g_trace_state, 0);
    return "err the simulation thread did not arm the trace\n";
  }
  Sleep(ms);
  InterlockedExchange(&g_trace_state, 3);
  for (int i = 0; i < 200 && g_trace_state == 3; ++i) Sleep(10);
  const uintptr_t base = reinterpret_cast<uintptr_t>(Base()), end = ImageEnd();
  std::ostringstream out;
  out << "ok trace hits=" << g_trace_hits << " matched=" << g_trace_matched << " sites=" << g_trace_sites_used
      << " dropped=" << g_trace_dropped << (g_trace_state ? " (not cleared yet)" : "") << "\n";
  for (LONG i = 0; i < g_trace_sites_used; ++i) {
    const uintptr_t r = g_trace_sites[i].ret;
    char b[64];
    if (g_trace_names) {
      const uintptr_t f = g_trace_sites[i].first_ret;
      char v[96];
      if (f >= base && f < end) snprintf(v, sizeof v, " x%ld last=%.3f from rva:%llX\n", g_trace_sites[i].count,
                                         g_trace_sites[i].value, (unsigned long long)(f - base));
      else snprintf(v, sizeof v, " x%ld last=%.3f from abs:%llX\n", g_trace_sites[i].count, g_trace_sites[i].value,
                    (unsigned long long)f);
      char name[65];
      TraceName(r, name);
      out << "name " << name << v;
      continue;
    }
    if (g_trace_r8) {
      char name[65];
      TraceObjectName(r, name);
      const uintptr_t f = g_trace_sites[i].first_ret;
      char v[260];
      snprintf(v, sizeof v, "r8 abs:%llX '%s' x%ld (first: rcx abs:%llX rcx+28 abs:%llX r9 abs:%llX from %s:%llX)\n",
               (unsigned long long)r, name, g_trace_sites[i].count, (unsigned long long)g_trace_sites[i].rcx,
               (unsigned long long)g_trace_sites[i].rcx28, (unsigned long long)g_trace_sites[i].r9,
               f >= base && f < end ? "rva" : "abs", (unsigned long long)(f >= base && f < end ? f - base : f));
      out << v;
      continue;
    }
    if (g_trace_args) {
      const uintptr_t f = g_trace_sites[i].first_ret;
      char v[200];
      snprintf(v, sizeof v, "r9+28 abs:%llX x%ld (first: rcx abs:%llX rcx+28 abs:%llX r9 abs:%llX from %s:%llX)\n",
               (unsigned long long)r, g_trace_sites[i].count, (unsigned long long)g_trace_sites[i].rcx,
               (unsigned long long)g_trace_sites[i].rcx28, (unsigned long long)g_trace_sites[i].r9,
               f >= base && f < end ? "rva" : "abs", (unsigned long long)(f >= base && f < end ? f - base : f));
      out << v;
      char x[400];
      int n = snprintf(x, sizeof x, "  arg5 %llX arg6 %llX rdx abs:%llX r8 %.2f %.2f %.2f mem:", (unsigned long long)g_trace_sites[i].arg5,
                       (unsigned long long)g_trace_sites[i].arg6, (unsigned long long)g_trace_sites[i].rdx,
                       g_trace_sites[i].r8v[0], g_trace_sites[i].r8v[1], g_trace_sites[i].r8v[2]);
      for (int k = 0; k < 0x80 && n < (int)sizeof x - 4; k += 8) {
        uint64_t q;
        memcpy(&q, g_trace_sites[i].rdx_mem + k, 8);
        n += snprintf(x + n, sizeof x - n, " %llX", (unsigned long long)q);
      }
      out << x << "\n";
      continue;
    }
    if (r >= base && r < end) snprintf(b, sizeof b, "site rva:%llX x%ld\n", (unsigned long long)(r - base), g_trace_sites[i].count);
    else snprintf(b, sizeof b, "site abs:%llX x%ld\n", (unsigned long long)r, g_trace_sites[i].count);
    out << b;
    if (g_trace_sites[i].depth) {
      out << " ";
      for (int k = 0; k < g_trace_sites[i].depth; ++k) {
        const uintptr_t f = g_trace_sites[i].stack[k];
        if (f >= base && f < end) snprintf(b, sizeof b, " rva:%llX", (unsigned long long)(f - base));
        else snprintf(b, sizeof b, " abs:%llX", (unsigned long long)f);
        out << b;
      }
      out << "\n";
    }
  }
  return out.str();
}

// ---------------------------------------------------------------- the sender's attacks (probe "puppet attack")

namespace {

// An attack starts in 2E297C (it logs "%s Start Attack: %s, Node: '%s', Target: '%s', Anim:
// '%s'"): rcx the attacker's health object ([character+0x26A8]), rdx a node or null, r8 the
// attack record, r9 the target or null, then on the stack a float (1.0 in the calls seen), two
// bytes and a pointer (0). The player's strikes call it through 2D6B3C (from 2C4161), which
// zeroes the health object's +F0, +F8, +108 and +110 and jumps there. The stand-in gets no
// attack input of its own: its combat log stays empty while the sender strikes (session aq).
// Attack records (type 0xA15) are the 40-byte entries of one table, [27008F8] (count +0x28,
// array +0x38, sorted by the name hash at +4, name at +0x20; the game's find-by-name 1B078DC
// hashes with 423E70 and searches with 278540). Each copy has the table at its own address,
// but the hashes are the same, so a hash names an attack across copies.
// The game's own callers stop an attack with 2EA790(health, next attack or 0), and only while
// it is still the one at health+0xE0 and nothing holds the character (+0xD8, a recoil, is 0).
// Without that the stand-in's attack never ends (its combat object has no player link at
// +0x40/+0x50 to end it; the next start logs "Duplicate attack").
using StartAttack_t = void(__fastcall*)(uintptr_t health, uintptr_t node, uintptr_t record, uintptr_t target,
                                        float scale, uint8_t a6, uint8_t a7, uintptr_t a8);
using StopAttack_t = void(__fastcall*)(uintptr_t health, uintptr_t next);
StartAttack_t g_start_attack = nullptr;
StopAttack_t g_stop_attack = nullptr;
volatile LONG64 g_attack_record = 0;  // queued for the puppet (0: none)
volatile LONG g_attacks_started = 0;
volatile LONG g_attacks_stopped = 0;
volatile LONG g_attack_faults = 0;
// SimFollowAttack: the sender's attack last seen, and what was started on which puppet.
uint32_t g_follow_id = 0, g_follow_stamp = 0;
uintptr_t g_follow_record = 0, g_follow_health = 0;

const char kThunkHead[] = "33C0488981F00000008981F8000000488981080100008981100100" "00E9";
const char kStartHead[] = "488BC448895818488950105556574154415541564157";
const char kStopHead[] = "48895C24105556574154415541564157" "4881ECC0000000";

bool StartAttackKnown(uintptr_t thunk_rva) {
  const uint8_t* f = Base() + thunk_rva;
  if (!Head(f, kThunkHead) || !Head(Rel32Target(f + 0x1C), kStartHead)) return false;
  g_start_attack = reinterpret_cast<StartAttack_t>(const_cast<uint8_t*>(f));
  return true;
}

bool CallStartAttack(uintptr_t health, uintptr_t rec, uintptr_t target = 0) {
  __try {
    g_start_attack(health, 0, rec, target, 1.0f, 0, 1, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedIncrement(&g_attack_faults);
    return false;
  }
  InterlockedIncrement(&g_attacks_started);
  return true;
}

// Stops the puppet's attack if it is still rec and nothing holds it.
void CallStopAttack(uintptr_t health, uintptr_t rec) {
  __try {
    if (*reinterpret_cast<const uintptr_t*>(health + 0xE0) != rec || *reinterpret_cast<const uintptr_t*>(health + 0xD8))
      return;
    g_stop_attack(health, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedIncrement(&g_attack_faults);
    return;
  }
  InterlockedIncrement(&g_attacks_stopped);
}

}  // namespace

uintptr_t SimAttackRecord(uintptr_t table_rva, uint32_t id) {
  uintptr_t table = 0, array = 0;
  if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + table_rva, &table) || !table || !ReadQ(table + 0x38, &array) ||
      !array)
    return 0;
  __try {
    int lo = 0, hi = (int)*reinterpret_cast<const uint32_t*>(table + 0x28) - 1;
    while (lo <= hi) {
      const int mid = lo + (hi - lo) / 2;
      const uintptr_t rec = array + 40ull * mid;
      const uint32_t h = *reinterpret_cast<const uint32_t*>(rec + 4);
      if (h == id) return *reinterpret_cast<const uint32_t*>(rec) == 0xA15 ? rec : 0;
      if (h < id) lo = mid + 1;
      else hi = mid - 1;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 0;
}

bool AttackPending() { return g_attack_record != 0; }

namespace {
uintptr_t g_attack_player_global = 0;  // the GameClient global when the queued attack is for the player
volatile LONG g_attack_at_player = 0;  // the queued puppet attack gets the player as its target
}  // namespace

void RunAttack() {
  const uintptr_t rec = static_cast<uintptr_t>(InterlockedExchange64(&g_attack_record, 0));
  if (!rec) return;
  if (g_attack_player_global) {
    uintptr_t gc = 0, player = 0, health = 0;
    if (ReadQ(g_attack_player_global, &gc) && gc && ReadQ(gc + 0x888, &player) && player &&
        ReadQ(player + 0x26A8, &health) && health)
      CallStartAttack(health, rec);
    return;
  }
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner || !PuppetAlive(owner, vt, health)) return;
  uintptr_t target = 0;
  if (g_attack_at_player) {  // probe "puppet attack <id> at": the AI's own start passes its opponent's health object
    uintptr_t gc = 0, player = 0;
    if (ReadQ(reinterpret_cast<uintptr_t>(Base()) + 0x26FFA98, &gc) && gc && ReadQ(gc + 0x888, &player) && player)
      ReadQ(player + 0x26A8, &target);
  }
  CallStartAttack(health, rec, target);
}

std::string SimPuppetAttack(uintptr_t thunk_rva, uintptr_t table_rva, uint32_t id, uintptr_t gameclient_rva, bool at_player) {
  g_attack_at_player = at_player ? 1 : 0;
  if (!g_installed) return "err simulation-thread hook not installed (sim_hook)\n";
  if (!g_puppet && !gameclient_rva) return "err no puppet\n";
  if (!g_start_attack && !StartAttackKnown(thunk_rva))
    return "err attack functions differ from the known build; refused\n";
  const uintptr_t rec = SimAttackRecord(table_rva, id);
  if (!rec) return "err no attack with that id\n";
  char name[65];
  TraceObjectName(rec, name);
  const LONG before = g_attacks_started, faults = g_attack_faults;
  g_attack_player_global = gameclient_rva ? reinterpret_cast<uintptr_t>(Base()) + gameclient_rva : 0;
  InterlockedExchange64(&g_attack_record, static_cast<LONG64>(rec));
  for (int i = 0; i < 100 && g_attack_record; ++i) Sleep(10);
  char b[200];
  snprintf(b, sizeof b, "ok attack '%s' (abs:%llX) %s\n", name, (unsigned long long)rec,
           g_attack_faults != faults        ? "faulted"
           : g_attacks_started == before    ? "not run"
           : gameclient_rva                 ? "started on this copy's player"
                                            : "started on the puppet");
  return b;
}

// With an attack started on the puppet, also play its move on the puppet's animation controller
// (below; an AI character's attack start does not), probe "puppet moves on|off". The move leaves the
// graph holding the sword out; once the attack has ended, ~1.2 s later the graph is sent to IdleL,
// whose transitions sheathe it (session bd).
volatile LONG g_attack_moves_on = 1;
volatile LONG g_attack_moves_played = 0;
ULONGLONG g_sheathe_at = 0;
bool PlayAttackMoveOn(uintptr_t character, uintptr_t record);
void PlaySheatheNow(uintptr_t character);

bool SimFollowAttack(uintptr_t thunk_rva, uintptr_t stop_rva, uintptr_t table_rva, uint32_t id, uint32_t stamp,
                     std::string* err) {
  if (g_sheathe_at && GetTickCount64() >= g_sheathe_at) {
    g_sheathe_at = 0;
    EnterCriticalSection(&g_lock);
    const uintptr_t o = g_puppet, v = g_puppet_vt, h = g_puppet_health;
    LeaveCriticalSection(&g_lock);
    if (o && PuppetAlive(o, v, h)) PlaySheatheNow(o);
  }
  if (id == g_follow_id && stamp == g_follow_stamp) return true;
  g_follow_id = id;
  g_follow_stamp = stamp;
  if (!g_stop_attack) {
    if (!StartAttackKnown(thunk_rva) || !Head(Base() + stop_rva, kStopHead)) {
      g_follow_id = g_follow_stamp = 0;  // so the next call says so again (once per error is logged)
      *err = "attack functions differ from the known build; refused";
      return false;
    }
    g_stop_attack = reinterpret_cast<StopAttack_t>(const_cast<uint8_t*>(Base() + stop_rva));
  }
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!owner || !PuppetAlive(owner, vt, health)) {
    g_follow_record = 0;
    *err = "no puppet";
    return false;
  }
  if (!id) {
    if (g_follow_record && g_follow_health == health) CallStopAttack(health, g_follow_record);
    if (g_follow_record && g_attack_moves_on) g_sheathe_at = GetTickCount64() + 1200;
    g_follow_record = 0;
    return true;
  }
  g_sheathe_at = 0;
  const uintptr_t rec = SimAttackRecord(table_rva, id);
  if (!rec) {
    *err = "no attack with the sender's id";
    return false;
  }
#ifndef SOWGUARD_MUTANT  // negative control: the stand-in never attacks
  if (CallStartAttack(health, rec)) {
    g_follow_record = rec;
    g_follow_health = health;
  }
  if (g_attack_moves_on && PlayAttackMoveOn(owner, rec)) InterlockedIncrement(&g_attack_moves_played);
#endif
  return true;
}

long SimAttacksStarted() { return g_attacks_started; }

long SimAttacksStopped() { return g_attacks_stopped; }

// ---------------------------------------------------------------- graph actions by name (probe "puppet play")

namespace {

// 664CA4(graph, action name, flag) plays a named action on an animation graph: 665018 builds the
// action from the name, 66538C finds it if it already runs (then it is only marked), else 195988
// starts it (195988 also records the name at graph+0x48). Its only caller, 664C6C(?, graph id,
// name, flag), is reached through a function table (no direct calls), likely a script binding.
// A character's graph is the entry of the animation manager's list ([[26FFAA8]+0x6CB0]: count
// +0x6720, pointers +0x6728) whose owner (+0x28) points back to the character at +0x40.
using PlayAction_t = void(__fastcall*)(uintptr_t graph, const char* name, uint8_t flag);
PlayAction_t g_play_action = nullptr;
char g_play_name[96] = {};
volatile LONG g_play_pending = 0;
volatile LONG g_play_flag = 0;
volatile LONG g_play_result = 0;  // 1 played, -1 no graph, -2 faulted

uintptr_t GraphOf(uintptr_t character) {
  uintptr_t mgr = 0, holder = 0;
  if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + 0x26FFAA8, &holder) || !holder || !ReadQ(holder + 0x6CB0, &mgr) ||
      !mgr)
    return 0;
  uint32_t n = 0;
  if (!ReadFloats(mgr + 0x6720, reinterpret_cast<float*>(&n), 1)) return 0;
  for (uint32_t i = 0; i < n && i < 1024; ++i) {
    uintptr_t g = 0, owner = 0, ch = 0;
    if (ReadQ(mgr + 0x6728 + 8ull * i, &g) && g && ReadQ(g + 0x28, &owner) && owner && ReadQ(owner + 0x40, &ch) &&
        ch == character)
      return g;
  }
  return 0;
}

bool CallPlayAction(uintptr_t graph, const char* name, uint8_t flag) {
  __try {
    g_play_action(graph, name, flag);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

}  // namespace

bool PlayPending() { return g_play_pending != 0; }

void RunPlay() {
  if (!InterlockedExchange(&g_play_pending, 0)) return;
  EnterCriticalSection(&g_lock);
  const uintptr_t owner = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  const uintptr_t graph = owner && PuppetAlive(owner, vt, health) ? GraphOf(owner) : 0;
  if (!graph) {
    g_play_result = -1;
    return;
  }
  g_play_result = CallPlayAction(graph, g_play_name, (uint8_t)g_play_flag) ? 1 : -2;
}

std::string SimPuppetPlay(uintptr_t fn_rva, const char* name, int flag) {
  if (!g_installed) return "err simulation-thread hook not installed (sim_hook)\n";
  if (!g_puppet) return "err no puppet\n";
  const uint8_t* f = Base() + fn_rva;
  if (!Head(f, "4885D20F84CF000000488" "95C2408488974241857")) return "err play-action function differs from the known build; refused\n";
  if (strlen(name) >= sizeof g_play_name) return "err name too long\n";
  g_play_action = reinterpret_cast<PlayAction_t>(const_cast<uint8_t*>(f));
  strcpy_s(g_play_name, name);
  g_play_flag = flag;
  g_play_result = 0;
  InterlockedExchange(&g_play_pending, 1);
  for (int i = 0; i < 100 && g_play_pending; ++i) Sleep(10);
  const LONG r = g_play_result;
  return r == 1 ? "ok played on the puppet's graph\n" : r == -1 ? "err the puppet has no graph\n" : r == -2 ? "err faulted\n" : "err not run\n";
}

// ---------------------------------------------------------------- an attack's move on the controller (probe "puppet move")

namespace {

// A player's strike plays its move on the animation controller, not as a graph action: the
// combat code (2C545C, on the player's combat-move object [character+2FA0h]+1A0h, which an AI
// character such as the puppet does not have) takes the name 4CA9A0(attack record, 0, 1) and
// the animation object 2E17D8(record), and calls 2C614C(controller, -, name, animation object,
// 0, game time, flag, speed), which registers the request (41A1E0) and queues it for the
// animation threads (session bd). Experiment: the same call for the puppet's controller.
using RecordAnim_t = uintptr_t(__fastcall*)(uintptr_t record);
using AnimName_t = const char*(__fastcall*)(uintptr_t record, uintptr_t override_name, uint8_t flag);
using PlayMove_t = int(__fastcall*)(uintptr_t ctrl, uintptr_t unused, const char* name, uintptr_t anim, int start,
                                    int now, uint8_t flag, float speed);
volatile LONG g_mv_pending = 0;
uintptr_t g_mv_record = 0, g_mv_character = 0;
float g_mv_speed = 1.0f;
LONG g_mv_result = 0;  // 1 played, -1 no controller, -2 faulted, -3 no name
int g_mv_id = 0;
char g_mv_name[96] = {};

bool CallPlayMove(uintptr_t ctrl, uintptr_t record, float speed, int* id, char name_out[96]) {
  const uint8_t* b = Base();
  __try {
    const uintptr_t anim = reinterpret_cast<RecordAnim_t>(const_cast<uint8_t*>(b + 0x2E17D8))(record);
    const char* name = reinterpret_cast<AnimName_t>(const_cast<uint8_t*>(b + 0x4CA9A0))(record, 0, 1);
    if (!name || !name[0]) return false;
    strncpy_s(name_out, 96, name, _TRUNCATE);
    uintptr_t clock = 0, t = 0;
    int now = 0;
    if (ReadQ(reinterpret_cast<uintptr_t>(b) + 0x2702598, &clock) && clock && ReadQ(clock + 8, &t) && t)
      now = *reinterpret_cast<const int*>(t + 0x18);
    *id = reinterpret_cast<PlayMove_t>(const_cast<uint8_t*>(b + 0x2C614C))(ctrl, 0, name, anim, 0, now, 1, speed);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

}  // namespace

// The sender's running jump is a chain of graph states, Sprint through RunningJump_AutoPicker_3m,
// and there its graph takes To_JumpAssisted_55v_35d into RunningJump_Assisted_Wraith, which lasts
// through the fall to the landing. The puppet's graph follows the chain to 3m (it gets the
// sender's inputs) but takes FreeJmp, loops the chain and ends in WallStop_To_Idle (its own
// movement is blocked at the ledge): no jump showed (session be). While the sender is in the air
// and the puppet's graph sits in those states, the assisted jump is played on it by name.
ULONGLONG g_assist_at = 0;
volatile LONG g_assist_plays = 0, g_assist_ended = 0;
// The action played on the puppet is the sender's own jump, RunningJump_Assisted_Wraith. Playing it made the
// puppet vanish from sight (frames of B's window, session be): while it plays, three floats at
// puppet+0x10AC (1.0 otherwise) are 0, a scale most likely. They are put back to 1.0 every frame.
char g_assist_name[64] = "RunningJump_Assisted_Wraith";  // probe "puppet assist <action>|off"
// Off by default: the Wraith jump makes the puppet vanish and no field forced so far brings it back.
volatile LONG g_assist_on = 0;
volatile LONG g_vis_mask = 0;  // probe "puppet vis <hex>": experiment, which fields are forced (below)

// The play function (664CA4) first looks the action up in the graph's list of running ones (a ring at
// graph+0x260: nodes with the action at +0x10 and its instance at +0x38, whose state at +0x74 is 2 when
// it has ended) and, if it finds one, only marks it. The first fall's assisted jump stayed in that list
// (the stand-in is teleported back before it ends), so on every later jump the play did nothing and the
// chain ended in WallStop. Such an instance is marked ended before the play.
void EndStaleAssist(uintptr_t graph) {
  __try {
    const uintptr_t head = graph + 0x260;
    uintptr_t node = *reinterpret_cast<const uintptr_t*>(graph + 0x268);
    for (int i = 0; i < 64 && node && node != head; ++i) {
      const uintptr_t def = *reinterpret_cast<const uintptr_t*>(node + 0x10);
      const uintptr_t inst = *reinterpret_cast<const uintptr_t*>(node + 0x38);
      char nm[65];
      TraceObjectName(def, nm);
      if (inst && !strcmp(nm, g_assist_name) && *reinterpret_cast<const int*>(inst + 0x74) != 2) {
        *reinterpret_cast<int*>(inst + 0x74) = 2;
        InterlockedIncrement(&g_assist_ended);
      }
      node = *reinterpret_cast<const uintptr_t*>(node + 8);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// The three floats at puppet+0x10AC go to 0 while RunningJump_Assisted_Wraith plays and the puppet
// is not drawn; they are 1.0 otherwise.
void SimKeepPuppetVisible() {
  EnterCriticalSection(&g_lock);
  const uintptr_t o = g_puppet, v = g_puppet_vt, h = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!o || !PuppetAlive(o, v, h)) return;
  const LONG m = g_vis_mask;
  __try {
    float* f = reinterpret_cast<float*>(o + 0x10AC);
    if ((m & 1) && (f[0] == 0.0f || f[1] == 0.0f || f[2] == 0.0f)) f[0] = f[1] = f[2] = 1.0f;
    if (m & 2) *reinterpret_cast<uint8_t*>(o + 0x225A) = 1;
    if (m & 4) *reinterpret_cast<uint8_t*>(o + 0x1133) = 1;
    if (m & 8) *reinterpret_cast<uint32_t*>(o + 0x2440) = 1;
    if (m & 16) *reinterpret_cast<uint32_t*>(o + 0x2C30) = 1;
    if (m & 32) *reinterpret_cast<uint32_t*>(o + 0x2D5C) = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void SimSetVisMask(unsigned mask) { InterlockedExchange(&g_vis_mask, (LONG)mask); }

void SimSetAssistJump(const char* action) {
  if (!strcmp(action, "off")) {
    InterlockedExchange(&g_assist_on, 0);
    return;
  }
  strncpy_s(g_assist_name, action, _TRUNCATE);
  InterlockedExchange(&g_assist_on, 1);
}

void SimAssistJump(bool sender_in_air) {
  if (!g_assist_on) return;
  SimKeepPuppetVisible();
  EnterCriticalSection(&g_lock);
  const uintptr_t o = g_puppet, v = g_puppet_vt, h = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!o || !PuppetAlive(o, v, h)) return;
  const ULONGLONG now = GetTickCount64();
  const uintptr_t graph = GraphOf(o);
  if (!graph) return;
  char name[48] = {};
  __try {
    memcpy(name, reinterpret_cast<const void*>(graph + 0x48), 40);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  // The whole approach chain (it loops every few ms, and a read once per frame saw only some of its
  // states: with four of the nine listed, one jump in five was caught).
  static const char* const kApproach[] = {
      "Runningjump_Autopicker_2M_POS", "Runningjump_Autopicker_1M_POS", "RunningJump_AutoPicker_0M",
      "RunningJump_AutoPicker_1M", "RunningJump_AutoPicker_2m", "RunningJump_AutoPicker_3m",
      "RunningJump_AutoPicker_FreeJmp", "RunningJump_Autopicker_CloseTap", "RunningJump_Autopicker_Hold"};
  bool in = false;
  for (const char* k : kApproach) in = in || !strcmp(name, k);
  (void)sender_in_air;
  if (!in) return;
  const uint8_t* f = Base() + 0x664CA4;
  if (!Head(f, "4885D20F84CF000000488" "95C2408488974241857")) return;
  g_play_action = reinterpret_cast<PlayAction_t>(const_cast<uint8_t*>(f));
  EndStaleAssist(graph);
  // Every frame it is seen there: Sprint re-enters the chain several times, overriding one play.
  if (CallPlayAction(graph, g_assist_name, 0)) {
    g_assist_at = now;
    if (InterlockedIncrement(&g_assist_plays) <= 8)
      SowLog(L"sim: the puppet's graph was in '%S': assisted jump played (%ld stale ones ended so far)", name, g_assist_ended);
  }
}

void PlaySheatheNow(uintptr_t character) {
  const uint8_t* f = Base() + 0x664CA4;
  if (!Head(f, "4885D20F84CF000000488" "95C2408488974241857")) return;
  const uintptr_t graph = GraphOf(character);
  if (!graph) return;
  g_play_action = reinterpret_cast<PlayAction_t>(const_cast<uint8_t*>(f));
  CallPlayAction(graph, "IdleL", 0);
}

bool PlayAttackMoveOn(uintptr_t character, uintptr_t record) {
  const uint8_t* b = Base();
  if (!Head(b + 0x2C614C, "488BC4488958084889701048897818554154415541564157") ||
      !Head(b + 0x2E17D8, "4883EC284C8BD1E840000000") || !Head(b + 0x4CA9A0, "40534883EC20488BD94885D27516"))
    return false;
  uintptr_t ctrl = 0;
  int id = 0;
  char name[96];
  return ReadQ(character + 0x1970, &ctrl) && ctrl && CallPlayMove(ctrl, record, 1.0f, &id, name);
}

void SimSetAttackMoves(bool on) { InterlockedExchange(&g_attack_moves_on, on ? 1 : 0); }

long SimAttackMovesPlayed() { return g_attack_moves_played; }

bool MovePending() { return g_mv_pending != 0; }

void RunMove() {
  if (!InterlockedExchange(&g_mv_pending, 0)) return;
  uintptr_t ctrl = 0;
  if (!g_mv_character || !ReadQ(g_mv_character + 0x1970, &ctrl) || !ctrl) {
    g_mv_result = -1;
    return;
  }
  g_mv_name[0] = 0;
  g_mv_result = CallPlayMove(ctrl, g_mv_record, g_mv_speed, &g_mv_id, g_mv_name) ? 1 : g_mv_name[0] ? -2 : -3;
}

std::string SimPuppetMove(uintptr_t table_rva, uint32_t id, float speed, uintptr_t gameclient_rva) {
  if (!g_installed) return "err simulation-thread hook not installed (sim_hook)\n";
  const uint8_t* b = Base();
  if (!Head(b + 0x2C614C, "488BC4488958084889701048897818554154415541564157") ||
      !Head(b + 0x2E17D8, "4883EC284C8BD1E840000000") || !Head(b + 0x4CA9A0, "40534883EC20488BD94885D27516"))
    return "err move functions differ from the known build; refused\n";
  const uintptr_t rec = SimAttackRecord(table_rva, id);
  if (!rec) return "err no attack record with that id\n";
  uintptr_t ch = 0;
  if (gameclient_rva) {
    uintptr_t gc = 0;
    if (!ReadQ(reinterpret_cast<uintptr_t>(b) + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &ch)) ch = 0;
  } else {
    EnterCriticalSection(&g_lock);
    ch = g_puppet && PuppetAlive(g_puppet, g_puppet_vt, g_puppet_health) ? g_puppet : 0;
    LeaveCriticalSection(&g_lock);
  }
  if (!ch) return "err no character\n";
  g_mv_record = rec;
  g_mv_character = ch;
  g_mv_speed = speed;
  g_mv_result = 0;
  InterlockedExchange(&g_mv_pending, 1);
  for (int i = 0; i < 100 && g_mv_pending; ++i) Sleep(10);
  Sleep(20);
  char out[200];
  const LONG r = g_mv_result;
  if (r == 1)
    snprintf(out, sizeof out, "ok move '%s' requested (id %X) on %s's controller\n", g_mv_name, g_mv_id,
             gameclient_rva ? "this copy's player" : "the puppet");
  else
    snprintf(out, sizeof out, "err %s\n", r == -1 ? "no controller" : r == -2 ? "faulted" : r == -3 ? "no name" : "not run");
  return out;
}

// ---------------------------------------------------------------- graph action starts on any thread (probe "graphwatch")

namespace {

// The game starts graph actions in 195988 (rcx the graph, rdx the action) on its animation worker
// threads, which the probe's trace (an execute breakpoint on the simulation thread) does not see.
// A stub at its entry (an aligned qword, `mov rax,rsp; mov [rax+10h],rbx` first) records, for one
// chosen graph, each call's return address and action in a 256-entry ring, without calling out.
// It also keeps the action's target ([rdx+60h], read at entry: the state machine builds the action
// on its stack) and the caller's r12 and r15, which in the state machine 192524 are the state it
// leaves and the transition that fired.
volatile uintptr_t g_gw_graph = 0;
volatile LONG g_gw_idx = 0;
uintptr_t g_gw_ret[256] = {}, g_gw_rdx[256] = {}, g_gw_def[256] = {}, g_gw_r12[256] = {}, g_gw_r15[256] = {};
volatile LONG g_gw_hook = 0;

}  // namespace

std::string SimGraphWatch(uintptr_t fn_rva, bool player, uintptr_t gameclient_rva, unsigned ms,
                          const std::function<void()>& armed) {
  if (!g_installed) return "err simulation-thread hook not installed (sim_hook)\n";
  if (!ms || ms > 20000) return "err usage: graphwatch <1..20000 ms> [player]\n";
  uint8_t* const fn = const_cast<uint8_t*>(Base()) + fn_rva;
  if (g_tap_hook) return "err the action tap holds the start-action function\n";
  if (!g_gw_hook) {
    if (!Head(fn, "488BC4488958104889701848897820" "55")) return "err start-action bytes differ from the known build; refused\n";
    uint8_t* page = SimNearPage(4);
    if (!page) return "err no code page near the game image\n";
    std::vector<uint8_t> s = {0x50, 0x41, 0x52, 0x41, 0x53, 0x49, 0xBB};
    PutQ(&s, reinterpret_cast<uintptr_t>(&g_gw_graph));
    s.insert(s.end(), {0x49, 0x3B, 0x0B, 0x75, 0x68, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(&g_gw_idx));
    s.insert(s.end(), {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x41, 0x0F, 0xC1, 0x03, 0x25, 0xFF, 0x00, 0x00, 0x00,
                       0x4C, 0x8B, 0x54, 0x24, 0x18, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_gw_ret));
    s.insert(s.end(), {0x4D, 0x89, 0x14, 0xC3, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_gw_rdx));
    s.insert(s.end(), {0x49, 0x89, 0x14, 0xC3, 0x4C, 0x8B, 0x52, 0x60, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_gw_def));
    s.insert(s.end(), {0x4D, 0x89, 0x14, 0xC3, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_gw_r12));
    s.insert(s.end(), {0x4D, 0x89, 0x24, 0xC3, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_gw_r15));
    s.insert(s.end(), {0x4D, 0x89, 0x3C, 0xC3, 0x41, 0x5B, 0x41, 0x5A, 0x58});
    s.insert(s.end(), fn, fn + 7);
    s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    PutQ(&s, reinterpret_cast<uintptr_t>(fn + 7));
    DWORD old = 0;
    if (s.size() > 256 || !VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old)) return "err cannot write the stub\n";
    memcpy(page + 1536, s.data(), s.size());
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    std::string err;
    if (!PatchJump(reinterpret_cast<uintptr_t>(fn), page + 1536, &err)) return "err " + err + "\n";
    InterlockedExchange(&g_gw_hook, 1);
  }
  uintptr_t ch = 0;
  if (player) {
    uintptr_t gc = 0;
    if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &ch)) ch = 0;
  } else {
    ch = g_puppet;
  }
  const uintptr_t graph = ch ? GraphOf(ch) : 0;
  if (!graph) return "err no graph\n";
  memset(g_gw_ret, 0, sizeof g_gw_ret);
  memset(g_gw_rdx, 0, sizeof g_gw_rdx);
  memset(g_gw_def, 0, sizeof g_gw_def);
  memset(g_gw_r12, 0, sizeof g_gw_r12);
  memset(g_gw_r15, 0, sizeof g_gw_r15);
  InterlockedExchange(&g_gw_idx, 0);
  g_gw_graph = graph;
  if (armed) armed();
  Sleep(ms);
  g_gw_graph = 0;
  const LONG n = std::min<LONG>((LONG)g_gw_idx, 256L);
  std::ostringstream out;
  out << "ok " << g_gw_idx << " starts on " << (player ? "this copy's player's" : "the puppet's") << " graph\n";
  const uintptr_t base = reinterpret_cast<uintptr_t>(Base());
  for (LONG i = 0; i < n; ++i) {
    char def[65], st[65], tr[65];
    TraceObjectName(g_gw_def[i], def);
    TraceObjectName(g_gw_r12[i], st);
    TraceObjectName(g_gw_r15[i], tr);
    char b[400];
    snprintf(b, sizeof b, "  %3ld from rva:%llX def abs:%llX '%s' r12 abs:%llX '%s' r15 abs:%llX '%s'\n", i,
             (unsigned long long)(g_gw_ret[i] - base), (unsigned long long)g_gw_def[i], def,
             (unsigned long long)g_gw_r12[i], st, (unsigned long long)g_gw_r15[i], tr);
    out << b;
  }
  return out.str();
}

// ---------------------------------------------------------------- controller move requests on any thread (probe "ctrlwatch")

namespace {

// A player's strike does not start graph actions: its moves are requests on the character's
// animation controller ([character+1970h]), registered in 41A1E0 (rcx the controller, rdx the
// request; 7 callers, among them 2C614C from the combat code and 41A038) and played later on the
// animation threads (session bd). A stub at its entry records, for one chosen controller, each
// call's return address, the request's first 8 qwords and the caller's rsi and rbx, in a
// 256-entry ring, and the first 48 stack qwords at entry (registers the callers saved) in a
// 64-entry one, without calling out.
volatile uintptr_t g_cw_ctrl = 0;
volatile LONG g_cw_idx = 0;
uintptr_t g_cw_ret[256] = {}, g_cw_rsi[256] = {}, g_cw_rbx[256] = {}, g_cw_req[256 * 8] = {};
uintptr_t g_cw_stack[64 * 48] = {};
volatile LONG g_cw_hook = 0;

std::string NamedQ(uintptr_t v) {
  char n[65];
  TraceObjectName(v, n);
  if (!n[0]) return "";
  return std::string(" '") + n + "'";
}

}  // namespace

std::string SimCtrlWatch(uintptr_t fn_rva, bool player, uintptr_t gameclient_rva, unsigned ms,
                         const std::function<void()>& armed) {
  if (!g_installed) return "err simulation-thread hook not installed (sim_hook)\n";
  if (!ms || ms > 20000) return "err usage: ctrlwatch <1..20000 ms> [player]\n";
  uint8_t* const fn = const_cast<uint8_t*>(Base()) + fn_rva;
  if (!g_cw_hook) {
    if (!Head(fn, "48895C240848896C2410488974241857" "415441564883EC30")) return "err move-request bytes differ from the known build; refused\n";
    uint8_t* page = SimNearPage(4);
    if (!page) return "err no code page near the game image\n";
    // push rax; push r10; push r11; mov r11,&g_cw_ctrl; cmp rcx,[r11]; jne (rel32) skip;
    // eax = slot (lock xadd on g_cw_idx, and 0xFF); ret, rsi, rbx into their rings;
    // eax <<= 6; r11 = &g_cw_req + rax; 8 qwords from [rdx]; skip: pop r11; pop r10; pop rax;
    // the first instruction (mov [rsp+8],rbx) and a jump back after it.
    std::vector<uint8_t> s = {0x50, 0x41, 0x52, 0x41, 0x53, 0x49, 0xBB};
    PutQ(&s, reinterpret_cast<uintptr_t>(&g_cw_ctrl));
    s.insert(s.end(), {0x49, 0x3B, 0x0B, 0x0F, 0x85, 0, 0, 0, 0});
    const size_t jne_end = s.size();
    s.insert(s.end(), {0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(&g_cw_idx));
    s.insert(s.end(), {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x41, 0x0F, 0xC1, 0x03, 0x25, 0xFF, 0x00, 0x00, 0x00,
                       0x4C, 0x8B, 0x54, 0x24, 0x18, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_cw_ret));
    s.insert(s.end(), {0x4D, 0x89, 0x14, 0xC3, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_cw_rsi));
    s.insert(s.end(), {0x49, 0x89, 0x34, 0xC3, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_cw_rbx));
    s.insert(s.end(), {0x49, 0x89, 0x1C, 0xC3, 0xC1, 0xE0, 0x06, 0x49, 0xBB});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_cw_req));
    s.insert(s.end(), {0x49, 0x01, 0xC3});
    for (uint8_t k = 0; k < 8; ++k) s.insert(s.end(), {0x4C, 0x8B, 0x52, uint8_t(8 * k), 0x4D, 0x89, 0x53, uint8_t(8 * k)});
    // shr eax,6; and eax,3Fh; imul eax,eax,384; push rcx; push rsi; push rdi;
    // mov rdi,&g_cw_stack; add rdi,rax; lea rsi,[rsp+30h] (the stack at entry); mov ecx,48;
    // rep movsq; pop rdi; pop rsi; pop rcx.
    s.insert(s.end(), {0xC1, 0xE8, 0x06, 0x83, 0xE0, 0x3F, 0x69, 0xC0, 0x80, 0x01, 0x00, 0x00, 0x51, 0x56, 0x57,
                       0x48, 0xBF});
    PutQ(&s, reinterpret_cast<uintptr_t>(g_cw_stack));
    s.insert(s.end(), {0x48, 0x01, 0xC7, 0x48, 0x8D, 0x74, 0x24, 0x30, 0xB9, 0x30, 0x00, 0x00, 0x00, 0xF3, 0x48,
                       0xA5, 0x5F, 0x5E, 0x59});
    const int32_t skip = (int32_t)(s.size() - jne_end);
    memcpy(&s[jne_end - 4], &skip, 4);
    s.insert(s.end(), {0x41, 0x5B, 0x41, 0x5A, 0x58});
    s.insert(s.end(), fn, fn + 5);
    s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    PutQ(&s, reinterpret_cast<uintptr_t>(fn + 5));
    DWORD old = 0;
    if (s.size() > 512 || !VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old)) return "err cannot write the stub\n";
    memcpy(page + 1792, s.data(), s.size());
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    std::string err;
    if (!PatchJump(reinterpret_cast<uintptr_t>(fn), page + 1792, &err)) return "err " + err + "\n";
    InterlockedExchange(&g_cw_hook, 1);
  }
  uintptr_t ch = 0;
  if (player) {
    uintptr_t gc = 0;
    if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &ch)) ch = 0;
  } else {
    ch = g_puppet;
  }
  uintptr_t ctrl = 0;
  if (!ch || !ReadQ(ch + 0x1970, &ctrl) || !ctrl) return "err no animation controller\n";
  memset(g_cw_ret, 0, sizeof g_cw_ret);
  memset(g_cw_req, 0, sizeof g_cw_req);
  InterlockedExchange(&g_cw_idx, 0);
  g_cw_ctrl = ctrl;
  if (armed) armed();
  Sleep(ms);
  g_cw_ctrl = 0;
  const LONG n = std::min<LONG>((LONG)g_cw_idx, 256L);
  std::ostringstream out;
  out << "ok " << g_cw_idx << " move requests on " << (player ? "this copy's player's" : "the puppet's")
      << " controller abs:" << std::hex << std::uppercase << ctrl << std::dec << "\n";
  const uintptr_t base = reinterpret_cast<uintptr_t>(Base());
  for (LONG i = 0; i < n; ++i) {
    char b[160];
    snprintf(b, sizeof b, "  %3ld from rva:%llX rsi abs:%llX%s rbx abs:%llX\n    req", i,
             (unsigned long long)(g_cw_ret[i] - base), (unsigned long long)g_cw_rsi[i], NamedQ(g_cw_rsi[i]).c_str(),
             (unsigned long long)g_cw_rbx[i]);
    out << b;
    for (int k = 0; k < 8; ++k) {
      const uintptr_t v = g_cw_req[i * 8 + k];
      snprintf(b, sizeof b, " %llX", (unsigned long long)v);
      out << b << NamedQ(v);
    }
    out << "\n    stack";
    for (int k = 0; i < 64 && k < 48; ++k) {
      const uintptr_t v = g_cw_stack[i * 48 + k];
      if (v >= base && v < ImageEnd()) snprintf(b, sizeof b, " +%X:rva:%llX", k * 8, (unsigned long long)(v - base));
      else snprintf(b, sizeof b, " +%X:%llX", k * 8, (unsigned long long)v);
      out << b;
    }
    out << "\n";
  }
  return out.str();
}

// ---------------------------------------------------------------- enemies and proxies (STAGE2-DESIGN.md)

namespace {

constexpr int kMaxSeen = 512, kMaxProxy = 64;
struct SeenEntry {
  uintptr_t owner;
  ULONGLONG at;
};
SeenEntry g_seen[kMaxSeen];
uintptr_t g_proxy_owner[kMaxProxy];

void EnemyNote(uintptr_t owner) {
  const ULONGLONG now = GetTickCount64();
  int free_i = -1;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < kMaxSeen; ++i) {
    if (g_seen[i].owner == owner) {
      g_seen[i].at = now;
      LeaveCriticalSection(&g_lock);
      return;
    }
    if (free_i < 0 && (!g_seen[i].owner || now - g_seen[i].at > 3000)) free_i = i;
  }
  if (free_i >= 0) g_seen[free_i] = {owner, now};
  LeaveCriticalSection(&g_lock);
}

bool ProxyHas(uintptr_t owner) {
  for (int i = 0; i < kMaxProxy; ++i)
    if (g_proxy_owner[i] == owner) return true;
  return false;
}

bool CharacterHealth(uintptr_t owner, uintptr_t* health) {
  uintptr_t vt = 0, back = 0;
  return owner && ReadQ(owner, &vt) && InImage(vt) && ReadQ(owner + 0x26A8, health) && *health &&
         ReadQ(*health + 0x28, &back) && back == owner;
}

}  // namespace

bool SimSpawnBusy() { return SpawnPending(); }

bool SimEnemyRegistryStart(uintptr_t site_rva, uintptr_t tree_rva, std::string* err) {
  if (!SimRequestBrainHook(site_rva, tree_rva, err)) return false;
  InterlockedExchange(&g_registry_on, 1);
  return true;
}

size_t SimEnemyList(uintptr_t* out, size_t max) {
  const ULONGLONG now = GetTickCount64();
  size_t n = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < kMaxSeen && n < max; ++i)
    if (g_seen[i].owner && now - g_seen[i].at < 400 && !ProxyHas(g_seen[i].owner)) out[n++] = g_seen[i].owner;
  LeaveCriticalSection(&g_lock);
  return n;
}

bool SimEnemyRead(uintptr_t owner, SimEnemyInfo* out) {
  uintptr_t health = 0, rec = 0, type_rec = 0;
  if (!CharacterHealth(owner, &health)) return false;
  memset(out, 0, sizeof *out);
  if (!ReadFloats(owner + 0xAC, out->pos, 3) || !ReadFloats(owner + 0xB8, out->rot, 4) ||
      !ReadFloats(health + 0x428, &out->max_hp, 1) || !ReadFloats(health + 0x42C, &out->hp, 1))
    return false;
  __try {
    rec = *reinterpret_cast<const uintptr_t*>(health + 0xE0);
    if (rec && *reinterpret_cast<const uint32_t*>(rec) == 0xA15) out->attack_id = *reinterpret_cast<const uint32_t*>(rec + 4);
    out->attack_stamp = *reinterpret_cast<const uint32_t*>(health + 0x470);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->attack_id = 0;
  }
  if (ReadQ(owner + 0x2E68, &type_rec) && type_rec) {
    char name[65];
    TraceObjectName(type_rec, name);
    strncpy_s(out->type, name, _TRUNCATE);
  }
  // A captain keeps his Nemesis member's reference in the character: the faction record (+0x2708), the member id (+0x2710,
  // -1 for anyone else).
  out->member_id = 0xFFFFFFFFu;
  uintptr_t faction = 0, id_q = 0;
  uint32_t head[2] = {};
  if (ReadQ(owner + 0x2710, &id_q) && (uint32_t)id_q != 0xFFFFFFFFu && ReadQ(owner + 0x2708, &faction) && faction &&
      CopyMem(head, faction, 8) && head[0] == 0xA51) {
    char name[40];
    if (SimFindMember(head[1], (uint32_t)id_q, name)) {
      out->member_id = (uint32_t)id_q;
      out->faction_hash = head[1];
      strncpy_s(out->member_name, name, _TRUNCATE);
    }
  }
  return true;
}

bool SimHealthNow(uintptr_t owner, float* cur, float* max) {
  uintptr_t health = 0;
  float v[2] = {};
  if (!CharacterHealth(owner, &health) || !ReadFloats(health + 0x428, &v[0], 1) || !ReadFloats(health + 0x42C, &v[1], 1)) return false;
  *max = v[0];
  *cur = v[1];
  return true;
}

uint32_t SimAttackIdByName(const char* name) {
  const uint8_t* hash_fn = Base() + 0x423E70;
  if (!Head(hash_fn, "0FB60184C074274C8D0D")) return 0;
  return reinterpret_cast<Hash_t>(const_cast<uint8_t*>(hash_fn))(name, nullptr, 0);
}

size_t SimProxyOwners(uintptr_t* owners, uintptr_t* healths, size_t max) {
  size_t n = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < kMaxProxy && n < max; ++i)
    if (g_proxy_owner[i]) {
      uintptr_t hp = 0;
      CharacterHealth(g_proxy_owner[i], &hp);
      owners[n] = g_proxy_owner[i];
      healths[n] = hp;
      ++n;
    }
  LeaveCriticalSection(&g_lock);
  return n;
}

bool SimProxyValid(uintptr_t owner) {
  uintptr_t health = 0;
  return CharacterHealth(owner, &health);
}

bool SimProxyAdopt(uintptr_t owner, bool follower) {
  uintptr_t health = 0;
  if (!CharacterHealth(owner, &health)) return false;
  EnterCriticalSection(&g_lock);
  int slot = -1;
  for (int i = 0; i < kMaxProxy; ++i) {
    if (g_proxy_owner[i] == owner) slot = i;
    else if (slot < 0 && !g_proxy_owner[i]) slot = i;
  }
  if (slot >= 0) g_proxy_owner[slot] = owner;
  LeaveCriticalSection(&g_lock);
  if (slot < 0) return false;
  __try {
    if (follower) *reinterpret_cast<volatile uint8_t*>(owner + kFollower) = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return true;
}

void SimProxyDetachMember(uintptr_t owner) {
  __try {
    *reinterpret_cast<volatile uint32_t*>(owner + 0x2710) = 0xFFFFFFFFu;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void SimProxyDrop(uintptr_t owner) {
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < kMaxProxy; ++i)
    if (g_proxy_owner[i] == owner) g_proxy_owner[i] = 0;
  LeaveCriticalSection(&g_lock);
}

bool SimProxyMove(uintptr_t teleport_rva, uintptr_t owner, const float pos[3], const float rot[4], bool direct) {
  Xform x{};
  memcpy(x.pos, pos, sizeof x.pos);
  memcpy(x.rot, rot, sizeof x.rot);
  if (direct) return WriteXform(owner, x);
  const uint8_t* f = Base() + teleport_rva;
  if (!TeleportBytesOk(f)) return false;
  for (float v : x.pos)
    if (!std::isfinite(v)) return false;
  __try {
    reinterpret_cast<Teleport_t>(const_cast<uint8_t*>(f))(reinterpret_cast<void*>(owner), &x);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool SimProxyPlay(uintptr_t fn_rva, uintptr_t owner, const char* name) {
  const uint8_t* f = Base() + fn_rva;
  if (!Head(f, "4885D20F84CF000000488" "95C2408488974241857")) return false;
  g_play_action = reinterpret_cast<PlayAction_t>(const_cast<uint8_t*>(f));
  const uintptr_t graph = GraphOf(owner);
  return graph && CallPlayAction(graph, name, 0);
}

namespace {
volatile LONG g_proxy_moves_ok = 0, g_proxy_moves_failed = 0;
}  // namespace
bool PlayAttackMoveOn(uintptr_t character, uintptr_t record);

uintptr_t SimProxyAttack(uintptr_t thunk_rva, uintptr_t stop_rva, uintptr_t table_rva, uintptr_t owner, uint32_t id, bool at_player,
                         uintptr_t gameclient_rva) {
  uintptr_t health = 0;
  if (!CharacterHealth(owner, &health)) return 0;
  if (!g_stop_attack) {
    if (!StartAttackKnown(thunk_rva) || !Head(Base() + stop_rva, kStopHead)) return 0;
    g_stop_attack = reinterpret_cast<StopAttack_t>(const_cast<uint8_t*>(Base() + stop_rva));
  }
  if (!g_start_attack && !StartAttackKnown(thunk_rva)) return 0;
  const uintptr_t rec = SimAttackRecord(table_rva, id);
  uintptr_t target = 0;
  if (at_player) {
    uintptr_t gc = 0, player = 0;
    if (ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) && gc && ReadQ(gc + 0x888, &player) && player)
      ReadQ(player + 0x26A8, &target);
  }
  if (!rec || !CallStartAttack(health, rec, target)) return 0;
  // The swing itself is a move on the character's animation controller (an AI-off character plays none by itself): the same call
  // the stand-in's swings use.
  if (PlayAttackMoveOn(owner, rec)) InterlockedIncrement(&g_proxy_moves_ok);
  else InterlockedIncrement(&g_proxy_moves_failed);
  return rec;
}

long SimProxyMovesOk() { return g_proxy_moves_ok; }
long SimProxyMovesFailed() { return g_proxy_moves_failed; }

void SimProxyStopAttack(uintptr_t owner, uintptr_t record) {
  uintptr_t health = 0;
  if (record && g_stop_attack && CharacterHealth(owner, &health)) CallStopAttack(health, record);
}

void SimProxyHealth(uintptr_t owner, float frac) {
  uintptr_t health = 0;
  if (!CharacterHealth(owner, &health)) return;
  float max_hp = 0;
  if (!ReadFloats(health + 0x428, &max_hp, 1) || !(max_hp > 0)) return;
  __try {
    *reinterpret_cast<volatile float*>(health + 0x42C) = max_hp * std::max(0.0f, std::min(1.0f, frac));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// The host applies a hit of the client's: the game's own hit function, with attacker_owner (the decoy) as the attacker, or
// the host's own player when it is 0, and the enemy's health object as the victim.
bool SimApplyHit(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t victim_owner, const float pos[3], uint32_t attack_id,
                 uintptr_t gameclient_rva, uintptr_t attacker_owner) {
  uintptr_t victim = 0, attacker = 0;
  if (!CharacterHealth(victim_owner, &victim)) return false;
  // The stand-in's health object as the attacker faulted inside the hit (a character without the player's links,
  // session bm). The decoy (an ordinary soldier) does not: the wake-up poke has used it since session cc. An enemy hit by the
  // host's player turns to the host's player, and one the hunt then pointed back at the decoy stood still (session cg).
  if (attacker_owner && !CharacterHealth(attacker_owner, &attacker)) attacker = 0;
  const bool given = attacker != 0;  // the hit is attacker_owner's (not this copy's player's)
  if (!attacker) {
    uintptr_t gc = 0, player = 0;
    if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &player) || !player ||
        !ReadQ(player + 0x26A8, &attacker) || !attacker)
      return false;
  }
  const uintptr_t rec = SimAttackRecord(table_rva, attack_id);
  const uint8_t* f = Base() + hit_rva;
  if (!Head(f, "48895C240848896C24184889742420574156" "4157") && !(f[0] == 0xE9 || f[0] == 0xFF)) return false;  // known entry, or our stub's jump
  float p[3];
  memcpy(p, pos, sizeof p);
  // The decoy is marked as the player's follower; its hit through a player's attack record did no damage with the mark on
  // (session cg): the mark is off for the length of the call, as for a proxy's swing at the client's player.
  volatile uint8_t* mark = given ? reinterpret_cast<volatile uint8_t*>(attacker_owner + kFollower) : nullptr;
  uint8_t was = 0;
  __try {
    if (mark) {
      was = *mark;
      *mark = 0;
    }
    // Six arguments: the fifth and sixth are on the stack and the hit reads them (the AI's own calls pass 0 and 0;
    // with them left to chance the call faulted, session bm).
    reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, const float*, uintptr_t, uintptr_t, uint64_t)>(
        const_cast<uint8_t*>(f))(attacker, victim, p, rec, 0, 0);
    if (mark) *mark = was;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    __try {
      if (mark) *mark = was;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
  }
  return true;
}

// Test: a swing of the character `attacker_owner` (an enemy) at the stand-in, through the game's hit function (and so
// through our stub: it is reported and dropped, and the client takes it).
bool SimHitStandin(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t attacker_owner, uint32_t attack_id) {
  uintptr_t attacker = 0;
  if (!CharacterHealth(attacker_owner, &attacker)) return false;
  EnterCriticalSection(&g_lock);
  const uintptr_t puppet = g_puppet, vt = g_puppet_vt, health = g_puppet_health;
  LeaveCriticalSection(&g_lock);
  if (!puppet || !PuppetAlive(puppet, vt, health)) return false;
  float p[3] = {};
  if (!ReadFloats(puppet + 0xAC, p, 3)) return false;
  const uintptr_t rec = SimAttackRecord(table_rva, attack_id);
  const uint8_t* f = Base() + hit_rva;
  if (!Head(f, "48895C240848896C24184889742420574156" "4157") && !(f[0] == 0xE9 || f[0] == 0xFF)) return false;
  __try {
    reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, const float*, uintptr_t, uintptr_t, uint64_t)>(
        const_cast<uint8_t*>(f))(attacker, health, p, rec, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// The client: an enemy's swing, told by the host, hits this copy's own player (the attacker is that enemy's proxy).
bool SimApplyDamage(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t attacker_owner, const float pos[3], uint32_t attack_id,
                    uintptr_t gameclient_rva) {
  uintptr_t attacker = 0, victim = 0, gc = 0, player = 0;
  if (!CharacterHealth(attacker_owner, &attacker)) return false;
  if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &player) || !player ||
      !ReadQ(player + 0x26A8, &victim) || !victim)
    return false;
  const uintptr_t rec = SimAttackRecord(table_rva, attack_id);
  const uint8_t* f = Base() + hit_rva;
  if (!Head(f, "48895C240848896C24184889742420574156" "4157") && !(f[0] == 0xE9 || f[0] == 0xFF)) return false;
  float p[3];
  memcpy(p, pos, sizeof p);
  // A proxy is marked as the player's follower (out of the player's combat) and a follower's swing does not hurt the
  // player: the mark is off for the length of the call.
  volatile uint8_t* mark = reinterpret_cast<volatile uint8_t*>(attacker_owner + kFollower);
  uint8_t was = 1;
  __try {
    was = *mark;
    *mark = 0;
    reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, const float*, uintptr_t, uintptr_t, uint64_t)>(
        const_cast<uint8_t*>(f))(attacker, victim, p, rec, 0, 0);
    *mark = was;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    __try {
      *mark = was;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
  }
  return true;
}

bool SimKillCharacter(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t owner, uintptr_t gameclient_rva, const char* attack_name,
                      uintptr_t by) {
  uintptr_t victim = 0, gc = 0, player = 0, attacker = 0;
  if (!CharacterHealth(owner, &victim)) return false;
  if (by) {
    if (!CharacterHealth(by, &attacker)) return false;
  } else if (!ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) || !gc || !ReadQ(gc + 0x888, &player) || !player ||
             !ReadQ(player + 0x26A8, &attacker) || !attacker) {
    return false;
  }
  const uint8_t* hash_fn = Base() + 0x423E70;
  if (!Head(hash_fn, "0FB60184C074274C8D0D")) return false;
  const uint32_t id = reinterpret_cast<Hash_t>(const_cast<uint8_t*>(hash_fn))(attack_name, nullptr, 0);
  const uintptr_t rec = SimAttackRecord(table_rva, id);
  const uint8_t* f = Base() + hit_rva;
  if (!rec || (!Head(f, "48895C240848896C24184889742420574156" "4157") && !(f[0] == 0xE9 || f[0] == 0xFF))) return false;
  SimProxyDrop(owner);  // its hits are dropped while it is a proxy
  float pos[3] = {};
  if (!ReadFloats(owner + 0xAC, pos, 3)) return false;
  __try {
    *reinterpret_cast<volatile float*>(victim + 0x42C) = 1.0f;
    reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, const float*, uintptr_t, uintptr_t, uint64_t)>(const_cast<uint8_t*>(f))(
        attacker, victim, pos, rec, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  float now = 1.0f;
  ReadFloats(victim + 0x42C, &now, 1);
  return now <= 0.0f;
}

bool SimIsFollower(uintptr_t owner) {
  uint8_t f = 0;
  __try {
    f = *reinterpret_cast<const volatile uint8_t*>(owner + kFollower);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return f != 0;
}

// Counter attacks (session cm): the player's counter strikes in the game's attack table are named ..._Cntr_... (seen:
// PC_Bip_Sync_Cntr_TimedKill_F/B, PC_Bip_Sync_Cntr_Knockback_B). The table ([table_rva]: count +0x28, array +0x38 of 40-byte
// records, type 0xA15 at +0, id +4, name pointer +0x20; tools/attack_table.py) is read once.
namespace {
// The ids of the counter attacks, raw (no C++ objects here: __try).
size_t ScanCounterIds(uintptr_t table_rva, uint32_t* out, size_t max) {
  size_t n = 0;
  __try {
    const uintptr_t table = *reinterpret_cast<const uintptr_t*>(Base() + table_rva);
    const uint32_t count = table ? *reinterpret_cast<const uint32_t*>(table + 0x28) : 0;
    const uintptr_t array = table ? *reinterpret_cast<const uintptr_t*>(table + 0x38) : 0;
    for (uint32_t i = 0; array && i < count && i < 200000 && n < max; ++i) {
      const uintptr_t r = array + 40ull * i;
      if (*reinterpret_cast<const uint32_t*>(r) != 0xA15) continue;
      const char* name = *reinterpret_cast<const char* const*>(r + 0x20);
      if (name && strstr(name, "_Cntr_")) out[n++] = *reinterpret_cast<const uint32_t*>(r + 4);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}
}  // namespace

bool SimIsCounterAttack(uintptr_t table_rva, uint32_t attack_id) {
  static SRWLOCK lock = SRWLOCK_INIT;
  static std::unordered_set<uint32_t>* ids = nullptr;
  AcquireSRWLockExclusive(&lock);
  if (!ids) {
    static uint32_t raw[4096];
    const size_t n = ScanCounterIds(table_rva, raw, 4096);
    ids = new std::unordered_set<uint32_t>(raw, raw + n);
    SowLog(L"sim: %zu counter attacks found in the attack table", ids->size());
  }
  const bool yes = ids->count(attack_id) != 0;
  ReleaseSRWLockExclusive(&lock);
  return yes;
}

uint32_t SimAttackId(const char* attack_name) {
  const uint8_t* hash_fn = Base() + 0x423E70;
  if (!Head(hash_fn, "0FB60184C074274C8D0D")) return 0;
  __try {
    return reinterpret_cast<Hash_t>(const_cast<uint8_t*>(hash_fn))(attack_name, nullptr, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

bool SimPokeEnemy(uintptr_t hit_rva, uintptr_t table_rva, uintptr_t enemy_owner, uintptr_t attacker_owner, const char* attack_name) {
  uintptr_t victim = 0, attacker = 0;
  if (!CharacterHealth(enemy_owner, &victim) || !CharacterHealth(attacker_owner, &attacker)) return false;
  const uint8_t* hash_fn = Base() + 0x423E70;
  if (!Head(hash_fn, "0FB60184C074274C8D0D")) return false;
  const uint32_t id = reinterpret_cast<Hash_t>(const_cast<uint8_t*>(hash_fn))(attack_name, nullptr, 0);
  const uintptr_t rec = SimAttackRecord(table_rva, id);
  const uint8_t* f = Base() + hit_rva;
  if (!rec || (!Head(f, "48895C240848896C24184889742420574156" "4157") && !(f[0] == 0xE9 || f[0] == 0xFF))) return false;
  float pos[3], hp_before = 0;
  if (!ReadFloats(enemy_owner + 0xAC, pos, 3) || !ReadFloats(victim + 0x42C, &hp_before, 1) || !(hp_before > 0)) return false;
  __try {
    reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, const float*, uintptr_t, uintptr_t, uint64_t)>(const_cast<uint8_t*>(f))(
        attacker, victim, pos, rec, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  __try {  // what the poke took is given back (a hit that killed it cannot be: left as it is)
    float now = 0;
    now = *reinterpret_cast<volatile float*>(victim + 0x42C);
    if (now > 0 && now < hp_before) *reinterpret_cast<volatile float*>(victim + 0x42C) = hp_before;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return true;
}

// A character is asked to go once. 1A8BB74 releases the character's slot in a 256-entry table by its index (+0x2BF8, 100748),
// walks the component at +0x2D10 (361548 -> 377320, which reads +8 and the array at +0x10..+0x18) and marks its AI data
// (+0x910, +0x912 = 1). The client asked its own population again every 1.5 s until the game removed it; the guard's log then
// held hundreds of access violations inside 377320 (+8 and the array entries holding 8, -1 and other non-pointers: a component
// already taken apart), and the client twice hung in a destructor (4DD18C) whose intrusive list never emptied (session ck).
// coopset despawnonce off: asked every time again (the old way), for an A/B.
volatile LONG g_despawn_once = 1, g_despawn_calls = 0, g_despawn_repeats = 0, g_despawn_skipped = 0, g_despawn_faults = 0,
              g_despawn_repeat_faults = 0;
struct DespawnAsk {
  uintptr_t character, ai;
  ULONGLONG at;
  int times;
};
std::vector<DespawnAsk> g_despawn_asked;  // simulation thread only

// What 377320 will walk in the component at character+0x2D10: its +8 and each entry of the array +0x10..+0x18 is 0 or a
// readable object whose +0x70 is 0 or a handle whose table (+8) is 0 or readable (60B484 -> 60B4F8 reads [handle+8]). With the
// guard below, 2 of 434 first asks still faulted there (session cl: two far-off proxies freed in one frame, a handle of 0x22, a
// pointer of -1): the component had been taken apart before the ask. Such a character is not asked to go.
bool DespawnComponentSane(uintptr_t nav) {
  auto entry_ok = [](uintptr_t obj) {
    uintptr_t h = 0, table = 0, probe = 0;
    if (!obj) return true;
    if (!ReadQ(obj + 0x70, &h)) return false;
    if (!h) return true;
    if (!ReadQ(h + 8, &table)) return false;
    return !table || ReadQ(table, &probe);
  };
  uintptr_t first = 0, begin = 0, end = 0;
  if (!ReadQ(nav + 8, &first) || !entry_ok(first) || !ReadQ(nav + 0x10, &begin) || !ReadQ(nav + 0x18, &end)) return false;
  if (end < begin || end - begin > 8 * 256 || (end - begin) % 8) return false;
  for (uintptr_t at = begin; at < end; at += 8) {
    uintptr_t obj = 0;
    if (!ReadQ(at, &obj) || !entry_ok(obj)) return false;
  }
  return true;
}

volatile LONG g_despawn_refused = 0;

bool SimDespawnNow(uintptr_t fn_rva, uintptr_t character) {
  const uint8_t* f = Base() + fn_rva;
  uintptr_t ai = 0, nav = 0, probe = 0;
  // 1A8BB74 calls through [character+0x2D10]: some characters hold -1 or 0 there and the call faults (seen on
  // prisoners and others); only a readable pointer is passed on.
  if (!Head(f, "48895C240848897424105748" "83EC208BDA488BF9") || !ReadQ(character + 0x2CE8, &ai) || !ai ||
      !ReadQ(character + 0x2D10, &nav) || !nav || nav == ~uintptr_t(0) || !ReadQ(nav, &probe))
    return false;
  uint8_t marked = 0;
  __try {
    marked = *reinterpret_cast<const volatile uint8_t*>(ai + 0x910);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  const ULONGLONG now = GetTickCount64();
  DespawnAsk* rec = nullptr;
  for (auto it = g_despawn_asked.begin(); it != g_despawn_asked.end();) {
    if (now - it->at > 60000) {
      it = g_despawn_asked.erase(it);
      continue;
    }
    // By address only: the population list can still hold a character freed in the last 400 ms, whose AI data pointer is
    // garbage then; a new character at a recycled address waits up to 60 s for its ask (a decoy remade there stays as a body).
    if (it->character == character) rec = &*it;
    ++it;
  }
  const bool repeat = marked || rec;
  if (repeat) {
    InterlockedIncrement(&g_despawn_repeats);
    if (g_despawn_once) {
      InterlockedIncrement(&g_despawn_skipped);
      return false;
    }
  }
  // Already being deleted: its destructor has put back the base class's vtable (1DB7358, the reference-list owner; session cm:
  // the one caught fault in 342 asks, rdi held such a character).
  uintptr_t vt_now = 0;
  if (ReadQ(character, &vt_now) && vt_now == reinterpret_cast<uintptr_t>(Base()) + 0x1DB7358) {
    if (InterlockedIncrement(&g_despawn_refused) <= 30)
      SowLog(L"sim: abs:%llX is not asked to go: it is being deleted", (unsigned long long)character);
    return false;
  }
  if (g_despawn_once && !DespawnComponentSane(nav)) {
    if (InterlockedIncrement(&g_despawn_refused) <= 30)
      SowLog(L"sim: abs:%llX is not asked to go: its component at +0x2D10 (abs:%llX) looks taken apart", (unsigned long long)character,
             (unsigned long long)nav);
    return false;
  }
  if (rec) {
    ++rec->times;
  } else if (g_despawn_asked.size() < 4096) {
    g_despawn_asked.push_back({character, ai, now, 1});
    rec = &g_despawn_asked.back();
  }
  InterlockedIncrement(&g_despawn_calls);
  __try {
    reinterpret_cast<void(__fastcall*)(void*, int, void*)>(const_cast<uint8_t*>(f))(reinterpret_cast<void*>(character), 0, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    const LONG n = InterlockedIncrement(&g_despawn_faults);
    if (repeat) InterlockedIncrement(&g_despawn_repeat_faults);
    if (n <= 30)
      SowLog(L"sim: the game's despawn faulted on abs:%llX (asked before: %s, marked %u, time %d)", (unsigned long long)character,
             repeat ? L"yes" : L"no", marked, rec ? rec->times : 0);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- the game's reference-list destructor (session cm)
// 4DD18C (the destructor of the base class 1DB7358: a character's list of the references pointing at it, at +8) loops
//   4DD1AC: cmp [rbx],rbx / je done / mov rax,[rdi+10h] / mov rsi,[rax+10h] / mov rcx,rsi / call 18D264 / ... / jmp 4DD1AC
// and 18D264 unlinks the reference only when its target (+0x20) is set (listfix.h). A reference linked with its target 0 hung
// the client three times (sessions ck, cl, cm: the last of ten references in a character's list, vtable 1DB6BE0, a member at
// +0x28 of an object with vtable 1FD5EA0). The call at 4DD1BC goes to ReferenceUnlinkHook instead, which unlinks it.
namespace {
// 4DD1AC: cmp [rbx],rbx / je / mov rax,[rdi+10h] / mov rsi,[rax+10h] / mov rcx,rsi / call (17 bytes, the E8 last)
const char kDtorLoopHead[] = "48391B742E488B4710488B7010488BCEE8";
const char kUnlinkHead[] = "48837920004C8BC17425";  // 18D264: cmp qword [rcx+20h],0 / mov r8,rcx / je
volatile LONG g_unlink_installed = 0, g_unlink_repaired = 0, g_unlink_left = 0, g_unlink_faults = 0;

uintptr_t __fastcall ReferenceUnlinkHook(uintptr_t ref) {
  listfix::Unlink r = listfix::Unlink::kLeft;
  __try {
    r = listfix::ReferenceUnlink(ref);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedIncrement(&g_unlink_faults);
    return ref;
  }
  if (r == listfix::Unlink::kRepaired) {
    uintptr_t vt = 0, holder_vt = 0;
    ReadQ(ref, &vt);
    ReadQ(ref - 0x28, &holder_vt);
    if (InterlockedIncrement(&g_unlink_repaired) <= 20)
      SowLog(L"sim: a reference linked with no target was unlinked in the game's list destructor (abs:%llX, vtable rva:%llX; "
             L"qword at -0x28 rva:%llX): the loop would never have ended",
             (unsigned long long)ref, (unsigned long long)(vt - reinterpret_cast<uintptr_t>(Base())),
             (unsigned long long)(holder_vt - reinterpret_cast<uintptr_t>(Base())));
  } else if (r == listfix::Unlink::kLeft) {
    if (InterlockedIncrement(&g_unlink_left) <= 20)
      SowLog(L"sim: a reference with no target and no consistent links in the game's list destructor (abs:%llX): left as is",
             (unsigned long long)ref);
  }
  return ref;
}
}  // namespace

// On the simulation thread (the one that runs the game's destructors), so no call is inside the patched bytes.
bool SimUnlinkFixInstall(std::string* err) {
  if (g_unlink_installed) return true;
  uint8_t* b = const_cast<uint8_t*>(Base());
  uint8_t* site = b + 0x4DD1AC;
  uint8_t* call = site + 16;  // the E8 at 4DD1BC
  if (!Head(site, kDtorLoopHead) || !Head(b + 0x18D264, kUnlinkHead)) {
    *err = "the destructor loop (rva 4DD1AC) or the unlink (rva 18D264) is not this build's";
    return false;
  }
  int32_t rel = 0;
  memcpy(&rel, call + 1, 4);
  if (call + 5 + rel != b + 0x18D264) {
    *err = "the destructor loop (rva 4DD1BC) does not call the unlink (rva 18D264)";
    return false;
  }
  uint8_t* page = SimNearPage(5);
  const int64_t reach = page ? reinterpret_cast<intptr_t>(page) - reinterpret_cast<intptr_t>(call + 5) : 0;
  DWORD old = 0;
  if (!page || reach < INT32_MIN || reach > INT32_MAX || !VirtualProtect(page, 4096, PAGE_READWRITE, &old)) {
    *err = "no code page near the image";
    return false;
  }
  const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
  memcpy(page, jmp, sizeof jmp);
  void* fn = reinterpret_cast<void*>(&ReferenceUnlinkHook);
  memcpy(page + 6, &fn, 8);
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 16);
  if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot make the game's code writable";
    return false;
  }
#ifndef SOWGUARD_MUTANT  // negative control: the loop keeps calling the game's unlink
  const int32_t rel32 = (int32_t)reach;
  memcpy(call + 1, &rel32, 4);
#endif
  VirtualProtect(call + 1, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), call, 5);
  InterlockedExchange(&g_unlink_installed, 1);
  SowLog(L"sim: the game's list destructor (rva:4DD1BC) unlinks references through the repair");
  return true;
}

void SimUnlinkStats(long* installed, long* repaired, long* left, long* faults) {
  *installed = g_unlink_installed;
  *repaired = g_unlink_repaired;
  *left = g_unlink_left;
  *faults = g_unlink_faults;
}

// ---- A guard for the AI's attack-parameter lookup (session cm) ----
// 11A19C asks 11AA18(rcx = [attacker+0x2CF0], rdx = target) for the parameters of an attacker against a target; 11AA18 reads the
// attacker's AI data through [rcx] (its owner) and [owner+0x2CE8]+0x28. For a player-type character +0x2CE8/+0x2CF0 is an empty
// list head whose first qword points at itself: the host crashed at 11AA3A reading 0x28 with rcx = the host player+0x2CE8
// (sessions cf and cm). The call at 11A4C5 is pointed at this guard, which answers such a call the way the game answers a call
// without a target (rcx+0x118, read by the caller only) and passes every other call on unchanged.
namespace {
volatile LONG g_aiparam_installed = 0, g_aiparam_guarded = 0;
constexpr uintptr_t kAttackParams = 0x11AA18;
const char kAttackParamsHead[] = "40534883EC204C8BC94885D2";
const char kAttackParamsSite[] = "498BF1498BF84C8BF2488BD9E8";  // 11A4B9: ... mov rbx,rcx; call 11AA18 (at 11A4C5)

uintptr_t __fastcall AttackParamsGuard(uintptr_t self, uintptr_t target) {
  uintptr_t first = 0, ai = 0;
  if (target && (!ReadQ(self, &first) || !first || first == self || !ReadQ(first + 0x2CE8, &ai) || !ai)) {
    if (InterlockedIncrement(&g_aiparam_guarded) <= 10)
      SowLog(L"sim: the AI's attack-parameter lookup got abs:%llX, not an AI (first qword abs:%llX); answered as for no target",
             (unsigned long long)self, (unsigned long long)first);
    return self + 0x118;
  }
  return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t)>(const_cast<uint8_t*>(Base() + kAttackParams))(self, target);
}
}  // namespace

bool SimAiParamGuardInstall(std::string* err) {
  if (g_aiparam_installed) return true;
  uint8_t* b = const_cast<uint8_t*>(Base());
  uint8_t* site = b + 0x11A4B9;
  uint8_t* call = site + 12;  // the E8 at 11A4C5
  if (!Head(site, kAttackParamsSite) || !Head(b + kAttackParams, kAttackParamsHead)) {
    *err = "the attack-parameter call (rva 11A4C5) or its target (rva 11AA18) is not this build's";
    return false;
  }
  int32_t rel = 0;
  memcpy(&rel, call + 1, 4);
  if (call + 5 + rel != b + kAttackParams) {
    *err = "rva 11A4C5 does not call 11AA18";
    return false;
  }
  uint8_t* page = SimNearPage(6);
  const int64_t reach = page ? reinterpret_cast<intptr_t>(page) - reinterpret_cast<intptr_t>(call + 5) : 0;
  DWORD old = 0;
  if (!page || reach < INT32_MIN || reach > INT32_MAX || !VirtualProtect(page, 4096, PAGE_READWRITE, &old)) {
    *err = "no code page near the image";
    return false;
  }
  const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
  memcpy(page, jmp, sizeof jmp);
  void* fn = reinterpret_cast<void*>(&AttackParamsGuard);
  memcpy(page + 6, &fn, 8);
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 16);
  if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot make the game's code writable";
    return false;
  }
  const int32_t rel32 = (int32_t)reach;
  memcpy(call + 1, &rel32, 4);
  VirtualProtect(call + 1, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), call, 5);
  InterlockedExchange(&g_aiparam_installed, 1);
  SowLog(L"sim: the AI's attack-parameter lookup (rva:11A4C5) passes through a guard");
  return true;
}

// ---- A guard for an AI tick's look at a component's owner (session cm) ----
// In an orc's AI tick (2F7ABB ... 499084) the call at 4990D8 hands 499724 the orc's component at +0x2CF0; 499724 reads the
// component's owner ([rcx]) and that owner's +0x2D18 (a component pointer on an AI character). The host crashed at 190F7C reading
// 0x1: the component's owner was the host's player, whose +0x2D18 holds 1 (session cm, after the hunt had swapped the player into
// an enemy's AI data). The call's result is not used there, so for an owner without a component pointer the call is skipped.
namespace {
volatile LONG g_aitick_installed = 0, g_aitick_skipped = 0;
constexpr uintptr_t kTickLook = 0x499724;
const char kTickLookHead[] = "488BC4555356574154415541564157";
const char kTickLookSite[] = "488B8BF02C0000E8";  // 4990D1: mov rcx,[rbx+2CF0]; call 499724 (the E8 at 4990D8)

uintptr_t __fastcall TickLookGuard(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
  uintptr_t owner = 0, comp = 0;
  if (!ReadQ(self, &owner) || owner < 0x10000 || !ReadQ(owner + 0x2D18, &comp) || comp < 0x10000) {
    if (InterlockedIncrement(&g_aitick_skipped) <= 10)
      SowLog(L"sim: an AI tick's look at abs:%llX skipped: its owner abs:%llX has no component at +0x2D18 (%llX)",
             (unsigned long long)self, (unsigned long long)owner, (unsigned long long)comp);
    return 0;
  }
  return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(
      const_cast<uint8_t*>(Base() + kTickLook))(self, a2, a3, a4);
}
}  // namespace

bool SimAiTickGuardInstall(std::string* err) {
  if (g_aitick_installed) return true;
  uint8_t* b = const_cast<uint8_t*>(Base());
  uint8_t* site = b + 0x4990D1;
  uint8_t* call = site + 7;  // the E8 at 4990D8
  if (!Head(site, kTickLookSite) || !Head(b + kTickLook, kTickLookHead)) {
    *err = "the AI tick's call (rva 4990D8) or its target (rva 499724) is not this build's";
    return false;
  }
  int32_t rel = 0;
  memcpy(&rel, call + 1, 4);
  if (call + 5 + rel != b + kTickLook) {
    *err = "rva 4990D8 does not call 499724";
    return false;
  }
  uint8_t* page = SimNearPage(7);
  const int64_t reach = page ? reinterpret_cast<intptr_t>(page) - reinterpret_cast<intptr_t>(call + 5) : 0;
  DWORD old = 0;
  if (!page || reach < INT32_MIN || reach > INT32_MAX || !VirtualProtect(page, 4096, PAGE_READWRITE, &old)) {
    *err = "no code page near the image";
    return false;
  }
  const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
  memcpy(page, jmp, sizeof jmp);
  void* fn = reinterpret_cast<void*>(&TickLookGuard);
  memcpy(page + 6, &fn, 8);
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 16);
  if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
    *err = "cannot make the game's code writable";
    return false;
  }
  const int32_t rel32 = (int32_t)reach;
  memcpy(call + 1, &rel32, 4);
  VirtualProtect(call + 1, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), call, 5);
  InterlockedExchange(&g_aitick_installed, 1);
  SowLog(L"sim: the AI tick's look at a component's owner (rva:4990D8) passes through a guard");
  return true;
}

void SimAiTickStats(long* installed, long* skipped) {
  *installed = g_aitick_installed;
  *skipped = g_aitick_skipped;
}

void SimAiParamStats(long* installed, long* guarded) {
  *installed = g_aiparam_installed;
  *guarded = g_aiparam_guarded;
}

void SimDespawnOnce(bool on) { InterlockedExchange(&g_despawn_once, on ? 1 : 0); }

void SimDespawnStats(long* calls, long* repeats, long* skipped, long* faults, long* repeat_faults, long* refused) {
  *refused = g_despawn_refused;
  *calls = g_despawn_calls;
  *repeats = g_despawn_repeats;
  *skipped = g_despawn_skipped;
  *faults = g_despawn_faults;
  *repeat_faults = g_despawn_repeat_faults;
}

// ---------------------------------------------------------------- redirecting an enemy's target (STAGE2-DESIGN.md)

namespace {

// An AI character keeps its goal in one of six slots of its AI data ([character+0x2CE8] + 0x118, 0x170, 0x1C8, 0x220,
// 0x2D0, 0x328; a non-zero byte at the slot's start marks the active one, 11B60C returns it); the slot's +0x30 holds
// the target character. The same character and its health object appear at other places of the AI data, the
// character (+0x2CA8) and the health object (+0xA8) and are copied from the slot by the mode switch (11AEB8,
// called from the AI tick at 2F7AE4). Redirecting replaces every qword that is the old target (or its health
// object) in the AI data, the character and the health object by the new one.
constexpr int kMaxRetarget = 32;
struct Retarget {
  uintptr_t enemy, target;
};
Retarget g_retarget[kMaxRetarget];
volatile LONG g_retargeted = 0, g_retarget_left = 0, g_retarget_skipped = 0;
// The only targets a redirect replaces (SimRetargetFrom): the host's player and the decoy. Before session cg it replaced whatever
// the enemy had taken: an enemy that turned on another (an orc that hit it) had that orc overwritten by the decoy every frame, and
// one such orc stood still with "the decoy" as its target for 50 s.
volatile uintptr_t g_retarget_from[2] = {0, 0};
const uintptr_t kSlots[] = {0x118, 0x170, 0x1C8, 0x220, 0x2D0, 0x328};

// A qword the swap replaces can be a reference's target (listfix.h: { vtable; node next, prev; self; target }, linked into its
// target's list at target+8). Writing the target alone left the reference in the old target's list: when the new target (the
// decoy) is destroyed, its destructor does not find it, and the reference keeps pointing at the freed decoy. Such a reference is
// moved with the game's assign (3F52B8: unlink, then link into the new target's list), so the decoy's destructor clears it the
// way the game clears references to any character that goes. coop_retarget_refs=off writes the qword alone (before session cm).
volatile LONG g_retarget_refs_on = 1, g_retarget_refs = 0, g_retarget_raw = 0;

// Is `field` the target of a reference linked into the list of `target` (its head at target+8)?
bool IsReferenceTo(uintptr_t field, uintptr_t target) {
  __try {
    const uintptr_t ref = field - 0x20, node = ref + 8, head = target + 8;
    if (*reinterpret_cast<const uintptr_t*>(ref + 0x18) != ref) return false;
    uintptr_t n = *reinterpret_cast<const uintptr_t*>(node);
    for (int i = 0; i < 1024 && n && n != node; ++i) {
      if (n == head) return true;
      n = *reinterpret_cast<const uintptr_t*>(n);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

void SwapField(uintptr_t* q, uintptr_t from, uintptr_t to) {
  static int assign_ok = -1;
  if (assign_ok < 0) assign_ok = Head(Base() + kRefAssign, kRefAssignHead);
  if (g_retarget_refs_on && assign_ok && IsReferenceTo(reinterpret_cast<uintptr_t>(q), from)) {
    reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t)>(const_cast<uint8_t*>(Base() + kRefAssign))(
        reinterpret_cast<uintptr_t>(q) - 0x20, to);
    InterlockedIncrement(&g_retarget_refs);
  } else {
    *q = to;
    InterlockedIncrement(&g_retarget_raw);
  }
}

uintptr_t SlotTarget(uintptr_t enemy, uintptr_t* slot_out) {
  uintptr_t ai = 0, t = 0;
  if (!ReadQ(enemy + 0x2CE8, &ai) || !ai) return 0;
  for (uintptr_t s : kSlots) {
    uint8_t on = 0;
    __try {
      on = *reinterpret_cast<const uint8_t*>(ai + s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0;
    }
    if (on) {
      if (slot_out) *slot_out = ai + s;
      return ReadQ(ai + s + 0x30, &t) ? t : 0;
    }
  }
  return 0;
}

}  // namespace

uintptr_t SimEnemyTarget(uintptr_t enemy) { return SlotTarget(enemy, nullptr); }

uintptr_t SimLocalPlayer(uintptr_t gameclient_rva) {
  uintptr_t gc = 0, player = 0;
  return ReadQ(reinterpret_cast<uintptr_t>(Base()) + gameclient_rva, &gc) && gc && ReadQ(gc + 0x888, &player) ? player : 0;
}

bool SimRetarget(uintptr_t enemy, uintptr_t target) {
  EnterCriticalSection(&g_lock);
  int slot = -1;
  for (int i = 0; i < kMaxRetarget; ++i) {
    if (g_retarget[i].enemy == enemy) {
      slot = i;
      break;
    }
    if (slot < 0 && !g_retarget[i].enemy) slot = i;
  }
  bool ok = slot >= 0;
  if (ok) g_retarget[slot] = {target ? enemy : 0, target};
  LeaveCriticalSection(&g_lock);
  return ok;
}

// Each frame, on the simulation thread.
void SimApplyRetargets() {
  Retarget list[kMaxRetarget];
  EnterCriticalSection(&g_lock);
  memcpy(list, g_retarget, sizeof list);
  LeaveCriticalSection(&g_lock);
  for (const Retarget& r : list) {
    if (!r.enemy || !r.target) continue;
    uintptr_t eh = 0, th = 0, ai = 0, cur = 0;
    if (!CharacterHealth(r.enemy, &eh) || !CharacterHealth(r.target, &th) || !ReadQ(r.enemy + 0x2CE8, &ai) || !ai) continue;
    cur = SlotTarget(r.enemy, nullptr);
    if (!cur || cur == r.target) continue;
    const uintptr_t f0 = g_retarget_from[0], f1 = g_retarget_from[1];
    if ((f0 || f1) && cur != f0 && cur != f1) {  // it chose someone else: left to it
      InterlockedIncrement(&g_retarget_left);
      continue;
    }
    // Only a living enemy, and only when what it is after is a character (its health object pointing back at it). The host crashed
    // in the AI's goal code (rva 11AA3A, reading 0x28) 4 s after orcs were spawned at addresses of orcs just killed (session cg):
    // a swap of a qword that only looked like a target, in an AI that was dead or still being set up, would write the decoy there.
    uintptr_t cur_h = 0;
    float alive = 0;
    if (!CharacterHealth(cur, &cur_h) || !ReadFloats(eh + 0x42C, &alive, 1) || !(alive > 0)) {
      InterlockedIncrement(&g_retarget_skipped);
      continue;
    }
    auto swap_in = [&](uintptr_t base, size_t size) {
      __try {
        for (size_t off = 0; off < size; off += 8) {
          uintptr_t* q = reinterpret_cast<uintptr_t*>(base + off);
          if (*q == cur) SwapField(q, cur, r.target);
          else if (cur_h && *q == cur_h) SwapField(q, cur_h, th);
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    };
    swap_in(ai, 0x1400);
    swap_in(r.enemy + 0x2C80, 0x80);
    swap_in(eh, 0x300);
    InterlockedIncrement(&g_retargeted);
  }
}

long SimRetargeted() { return g_retargeted; }
void SimRetargetRefs(bool on) { InterlockedExchange(&g_retarget_refs_on, on ? 1 : 0); }
void SimRetargetRefStats(long* refs, long* raw) {
  *refs = g_retarget_refs;
  *raw = g_retarget_raw;
}
long SimRetargetLeft() { return g_retarget_left; }
long SimRetargetSkipped() { return g_retarget_skipped; }
int SimRetargetSlotsUsed(int* dead) {
  Retarget list[kMaxRetarget];
  EnterCriticalSection(&g_lock);
  memcpy(list, g_retarget, sizeof list);
  LeaveCriticalSection(&g_lock);
  int used = 0, gone = 0;
  for (const Retarget& r : list) {
    if (!r.enemy) continue;
    ++used;
    uintptr_t eh = 0;
    float hp = 0;
    if (!CharacterHealth(r.enemy, &eh) || !ReadFloats(eh + 0x42C, &hp, 1) || !(hp > 0)) ++gone;
  }
  if (dead) *dead = gone;
  return used;
}
void SimRetargetDropTarget(uintptr_t target) {
  EnterCriticalSection(&g_lock);
  for (Retarget& r : g_retarget)
    if (r.target == target) r = {0, 0};
  LeaveCriticalSection(&g_lock);
}

void SimRetargetFrom(uintptr_t a, uintptr_t b) {
  g_retarget_from[0] = a;
  g_retarget_from[1] = b;
}

// Hiding a character: its scale (character+0x10AC, three floats) to a hundredth, every frame (the game puts it back,
// so this is tried, not known to work).
void SimHideCharacter(uintptr_t owner) {
  __try {
    float* f = reinterpret_cast<float*>(owner + 0x10AC);
    f[0] = f[1] = f[2] = 0.01f;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
