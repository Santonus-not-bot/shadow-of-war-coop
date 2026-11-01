// sowprobe: lets the agent inspect the running game through a named pipe.
//
// Started by sowguard when probe=1. One thread waits until the game has unpacked itself
// (ready_pattern appears in the exe's memory) and logs how long that took; another
// serves read-only commands on \\.\pipe\sowprobe-<pid>, one client at a time, one
// command per line, every answer ending with a line "END". The pipe name is written to
// <profile>\sowprobe.txt.
//
// Addresses are rva:<hex> (relative to the exe's base) or abs:<hex>.
//   ping                         pid, base, ready flag
//   module                       base, size and the section table
//   scan <pattern> [max=N]       RVAs of every hit ("48 8B 0D ? ? ? ?"), default max 64
//   read <addr> <len>            hex bytes, len <= 4096
//   rip <addr> <dispoff> <len>   rip-relative target: addr + len + disp32 at addr+dispoff
//   deref <addr> [off ...]       v = *(addr); then v = *(v + off) for each off
//   dbg [n]                      last n captured debug-print lines (default 20, max 256)
//   dbgstats                     captured lines per category
// With probe_write=1 only:
//   write <addr> <hex bytes>     write up to 256 bytes (logged in sowguard.log)
//   teleport <x> <y> <z>         this copy's player, keeping its rotation
//   spawn <type> [x y z [look]]  a character of a spawnable type (Orc_Infantry), optionally
//                                built with another appearance (PlayerSkin_Talion)
//   stacks <addr> <entry hex> <ms> [qword]
//                                call stacks reaching a function for ms (see Stacks);
//                                with qword, only calls whose first argument points at it
//
// Memory is only read from committed, readable, non-guard pages (VirtualQuery first):
// touching a guard page would remove it, which protection code may notice.
//
// Debug print (probe_write=1 and hook_debug_print=1): in this build the game's debug
// print `(const char* category, const char* fmt, ...)` is a 12-byte stub that spills r8/r9
// and returns. Once the game is ready, those 12 bytes (checked exactly first) become a
// jmp rel32 to our stub, which keeps every register as the original does (see
// BuildStub), calls DebugPrintSink and returns. Lines go to <profile>\debugprint.log
// (capped at 64 MB) and a ring for the dbg command.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "coop.h"
#include "input.h"
#include "probe.h"
#include "simhook.h"

namespace {

ProbeConfig g_cfg;
volatile LONG g_ready = 0;
volatile LONG64 g_ready_ms = -1;

// Debug-print capture.
enum DpState : LONG { kDpOff = 0, kDpHooked = 1, kDpRefused = 2 };
volatile LONG g_dp_state = kDpOff;
HANDLE g_dp_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_dp_lock;
std::deque<std::string> g_dp_ring;
std::map<std::string, unsigned long long> g_dp_counts;
unsigned long long g_dp_lines = 0, g_dp_bytes = 0, g_dp_dropped = 0;
ULONGLONG g_dp_t0 = 0;
constexpr unsigned long long kDpFileCap = 64ull << 20;
constexpr size_t kDpRing = 256;
// Exactly what the stub must look like before we patch it (see the header comment).
constexpr uint8_t kDpExpected[12] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0xC3, 0xCC};

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

size_t ImageSize() {
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(Base());
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(Base() + dos->e_lfanew);
  return nt->OptionalHeader.SizeOfImage;
}

bool ReadableProtect(DWORD p) {
  if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
               PAGE_EXECUTE_WRITECOPY)) != 0;
}

// True when [a, a+n) lies entirely in committed, readable, non-guard memory.
bool Readable(uintptr_t a, size_t n) {
  uintptr_t end = a + n;
  if (n == 0 || end < a) return false;
  while (a < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || !ReadableProtect(mbi.Protect)) return false;
    a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

// No C++ objects here, so __try can catch a page that went away in between.
bool CopyGuarded(void* dst, uintptr_t src, size_t n) {
  __try {
    memcpy(dst, reinterpret_cast<const void*>(src), n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool SafeRead(void* dst, uintptr_t src, size_t n) { return Readable(src, n) && CopyGuarded(dst, src, n); }

// Writes n bytes at dst, making the pages writable for the copy and restoring them.
bool SafeWrite(uintptr_t dst, const void* src, size_t n) {
  if (!Readable(dst, n)) return false;
  DWORD old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(dst), n, PAGE_EXECUTE_READWRITE, &old)) return false;
  bool ok = CopyGuarded(reinterpret_cast<void*>(dst), reinterpret_cast<uintptr_t>(src), n);
  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void*>(dst), n, old, &ignored);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(dst), n);
  return ok;
}

bool ParsePattern(const std::string& text, std::vector<int>* out);
std::vector<uintptr_t> Scan(const std::vector<int>& pat, size_t max_hits);
std::string Where(uintptr_t a);

// ---------------------------------------------------------------- debug print

void NoopInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}

// Formats the game's (fmt, args) without letting a bad pointer or a bad format crash the
// game: access faults are caught, CRT format errors return -1 through a no-op handler.
int FormatGuarded(char* out, size_t n, const char* fmt, va_list ap) {
  __try {
    return _vsnprintf_s(out, n, _TRUNCATE, fmt, ap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -2;
  }
}

void CopyStringGuarded(char* out, size_t n, const char* s) {
  out[0] = '\0';
  if (!s) return;
  __try {
    strncpy_s(out, n, s, _TRUNCATE);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    strcpy_s(out, n, "<bad>");
  }
}

void TrimNewlines(char* s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

// Called by the stub with the game's category and format and a va_list pointing at the
// spilled r8/r9 slots, which run on into the caller's stack arguments.
void DebugPrintSink(const char* category_in, const char* fmt_in, va_list ap) {
  char category[96], msg[1536];
  CopyStringGuarded(category, sizeof category, category_in);
  TrimNewlines(category);
  _invalid_parameter_handler old = _set_thread_local_invalid_parameter_handler(NoopInvalidParameter);
  int r = FormatGuarded(msg, sizeof msg, fmt_in, ap);
  _set_thread_local_invalid_parameter_handler(old);
  if (r == -2 || (r < 0 && !msg[0])) {
    // Keep the format string: it still says what the game meant to report.
    char fmt[1400];
    CopyStringGuarded(fmt, sizeof fmt, fmt_in);
    snprintf(msg, sizeof msg, "%s %s", r == -2 ? "<fault while formatting>" : "<bad format>", fmt);
  }
  TrimNewlines(msg);
  InputOnDebugLine(category, msg);

  char line[1800];
  int len = snprintf(line, sizeof line, "[%9llu] [%5lu] %s%s\r\n", GetTickCount64() - g_dp_t0, GetCurrentThreadId(),
                     category, msg);
  if (len < 0) return;
  if (len >= (int)sizeof line) len = (int)sizeof line - 1;
  EnterCriticalSection(&g_dp_lock);
  ++g_dp_lines;
  ++g_dp_counts[category];
  g_dp_ring.emplace_back(line, (size_t)len - 2);
  if (g_dp_ring.size() > kDpRing) g_dp_ring.pop_front();
  if (g_dp_bytes + len <= kDpFileCap) {
    DWORD w = 0;
    WriteFile(g_dp_file, line, (DWORD)len, &w, nullptr);
    g_dp_bytes += len;
  } else {
    ++g_dp_dropped;
  }
  LeaveCriticalSection(&g_dp_lock);
}

// The stub must look to its callers exactly like the original: callers were compiled
// knowing the original changes no register (only the r8/r9 home slots), and some keep
// live values in volatile registers across the call (seen at RVA F4F3DC: r9 loaded
// before the call, used after it). So it saves rax, rcx, rdx, r8-r11, xmm0-5 and the
// flags in its own frame, writes nothing in the caller's frame but the two slots the
// original writes, and is reached by a 5-byte jmp rel32 that touches no register.
//
// Offsets (E = rsp at entry; va_list = E+18h, contiguous with the stack arguments):
//   0  4C 89 44 24 18           mov [rsp+18h], r8        ; as the original
//   5  4C 89 4C 24 20           mov [rsp+20h], r9        ; as the original
//  10  9C                       pushfq
//  11  50 51 52                 push rax, rcx, rdx
//  14  41 50 41 51 41 52 41 53  push r8, r9, r10, r11
//  22  48 81 EC 88 00 00 00     sub rsp, 88h             ; rsp = E-C8h, 16-aligned
//  29  F3 0F 7F 44 24 20 ...    movdqu [rsp+20h+16*i], xmm0..xmm5 (6 x 6 bytes)
//  65  4C 8D 84 24 E0 00 00 00  lea r8, [rsp+0E0h]       ; = E+18h (rcx, rdx unchanged)
//  73  48 B8 <imm64>            mov rax, DebugPrintSink
//  83  FF D0                    call rax
//  85  F3 0F 6F 44 24 20 ...    movdqu xmm0..xmm5, [rsp+20h+16*i]
// 121  48 81 C4 88 00 00 00     add rsp, 88h
// 128  41 5B 41 5A 41 59 41 58  pop r11, r10, r9, r8
// 136  5A 59 58                 pop rdx, rcx, rax
// 139  9D                       popfq
// 140  C3                       ret
constexpr size_t kStubSize = 141;
constexpr size_t kStubPopR9 = 132;  // offset of "41 59" (the mutant build breaks it)

std::vector<uint8_t> BuildStub() {
  std::vector<uint8_t> s = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x9C, 0x50, 0x51,
                            0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x81, 0xEC, 0x88,
                            0x00, 0x00, 0x00};
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x7F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x4C, 0x8D, 0x84, 0x24, 0xE0, 0x00, 0x00, 0x00, 0x48, 0xB8});
  void* sink = reinterpret_cast<void*>(&DebugPrintSink);
  const auto* p = reinterpret_cast<const uint8_t*>(&sink);
  s.insert(s.end(), p, p + 8);
  s.insert(s.end(), {0xFF, 0xD0});
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x6F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A,
                     0x59, 0x58, 0x9D, 0xC3});
#ifdef SOWGUARD_MUTANT  // negative control: r9 not restored (r11 gets its saved value)
  s[kStubPopR9 + 1] = 0x5B;
#endif
  return s;
}

// UNWIND_INFO for the stub: version 1, prologue 29 bytes, 10 code slots, listed from the
// last prologue instruction back: sub rsp,88h (UWOP_ALLOC_LARGE, size/8 = 17 in the next
// slot), then pushfq and seven pushes, each UWOP_ALLOC_SMALL of 8 at its end offset.
const uint8_t kStubUnwind[24] = {0x01, 29, 10, 0x00, 29, 0x01, 17, 0x00, 22, 0x02, 20, 0x02,
                                 18,   0x02, 16, 0x02, 14, 0x02, 13, 0x02, 12, 0x02, 11, 0x02};

// A page within +-2 GB of `anchor`, so a 5-byte jmp rel32 can reach it.
uint8_t* AllocNear(uintptr_t anchor) {
  const uintptr_t gran = 0x10000, reach = 0x7FF00000;
  for (int dir = -1; dir <= 1; dir += 2) {
    for (uintptr_t step = gran; step < reach; step += gran) {
      uintptr_t a = (anchor & ~(gran - 1)) + (dir < 0 ? -(intptr_t)step : (intptr_t)step);
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
      void* p = VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (p) return static_cast<uint8_t*>(p);
    }
  }
  return nullptr;
}

bool InstallDebugPrintHook() {
  std::vector<int> pat;
  if (!ParsePattern(g_cfg.debug_print_pattern, &pat)) {
    SowLog(L"probe: bad debug_print_pattern, debug print not hooked");
    return false;
  }
  auto sites = Scan(pat, 2);
  if (sites.size() != 1) {
    SowLog(L"probe: debug print call site found %zu times (need exactly 1), not hooked", sites.size());
    return false;
  }
  int32_t disp = 0;
  if (!SafeRead(&disp, sites[0] + 1, 4)) return false;
  const uintptr_t target = sites[0] + 5 + disp;
  uint8_t have[12] = {};
  if (!SafeRead(have, target, sizeof have) || memcmp(have, kDpExpected, sizeof have) != 0) {
    char hex[40] = {};
    for (int i = 0; i < 12; ++i) snprintf(hex + 3 * i, 4, "%02x ", have[i]);
    SowLog(L"probe: debug print at %S does not look like the known 12-byte stub (%S), not hooked",
           Where(target).c_str(), hex);
    InterlockedExchange(&g_dp_state, kDpRefused);
    return false;
  }

  InitializeCriticalSection(&g_dp_lock);
  g_dp_t0 = GetTickCount64();
  g_dp_file = CreateFileW((g_cfg.profile + L"\\debugprint.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_dp_file == INVALID_HANDLE_VALUE) {
    SowLog(L"probe: cannot open debugprint.log (%lu), not hooked", GetLastError());
    return false;
  }

  // Stub, unwind info and function table entry share one page near the target.
  uint8_t* page = AllocNear(target);
  if (!page) {
    SowLog(L"probe: no free page within 2 GB of the debug print, not hooked");
    return false;
  }
  const std::vector<uint8_t> stub = BuildStub();
  if (stub.size() != kStubSize || stub[kStubPopR9] != 0x41) return false;
  memcpy(page, stub.data(), stub.size());
  memcpy(page + 160, kStubUnwind, sizeof kStubUnwind);
  auto* fn = reinterpret_cast<RUNTIME_FUNCTION*>(page + 192);
  fn->BeginAddress = 0;
  fn->EndAddress = (DWORD)kStubSize;
  fn->UnwindData = 160;
  DWORD old = 0;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  RtlAddFunctionTable(fn, 1, reinterpret_cast<DWORD64>(page));

  // jmp rel32 to the stub; the rest of the 12 bytes become int3 (never reached).
  uint8_t patch[12];
  memset(patch, 0xCC, sizeof patch);
  patch[0] = 0xE9;
  const int64_t rel = reinterpret_cast<intptr_t>(page) - (intptr_t)(target + 5);
  if (rel < INT32_MIN || rel > INT32_MAX) return false;
  const int32_t rel32 = (int32_t)rel;
  memcpy(patch + 1, &rel32, 4);
  if (!SafeWrite(target, patch, sizeof patch)) {
    SowLog(L"probe: patching the debug print at %S failed", Where(target).c_str());
    return false;
  }
  InterlockedExchange(&g_dp_state, kDpHooked);
  SowLog(L"probe: debug print at %S hooked (call site %S), logging to debugprint.log", Where(target).c_str(),
         Where(sites[0]).c_str());

  // Channel flags are one byte each; release builds skip most calls while they are 0.
  std::istringstream ch(g_cfg.debug_channels);
  std::string tok;
  int set = 0, bad = 0;
  const uint8_t on = 1;
  while (ch >> tok) {
    for (char& c : tok)
      if (c == ',') c = ' ';
    std::istringstream parts(tok);
    std::string rva_text;
    while (parts >> rva_text) {
      uintptr_t rva = strtoull(rva_text.c_str(), nullptr, 16);
      if (rva && rva < ImageSize() && SafeWrite(Base() + rva, &on, 1)) ++set;
      else ++bad;
    }
  }
  if (set || bad) SowLog(L"probe: %d debug channel flag(s) set, %d rejected", set, bad);
  return true;
}

// ---------------------------------------------------------------- stacks
// Probe "stacks": for a few seconds a jmp at a function's entry leads to a stub that
// saves every volatile register and the flags, records the caller's stack, restores them,
// runs the entry instructions the jmp replaced (relocated: the caller names them, and they
// must not be RIP-relative) and jumps back behind them. The stub has unwind info, so
// WalkStack unwinds through it into the game's own frames.
//   0  9C 50 51 52 41 50 41 51 41 52 41 53   pushfq; push rax, rcx, rdx, r8-r11
//  12  48 81 EC 88 00 00 00                  sub rsp, 88h
//  19  movdqu [rsp+20h+16*i], xmm0..5        (6 x 6 bytes)
//  55  48 B8 <imm64>  FF D0                  mov rax, StackHit; call rax (rcx: first argument)
//  67  movdqu xmm0..5, [rsp+20h+16*i]
// 103  48 81 C4 88 00 00 00                  add rsp, 88h
// 110  41 5B 41 5A 41 59 41 58 5A 59 58 9D   pop r11-r8, rdx, rcx, rax; popfq
// 122  <the replaced entry bytes>
//      FF 25 00 00 00 00 <imm64>            jmp [rip+0] -> entry + their length
constexpr size_t kStackStubHead = 122;
// UNWIND_INFO: version 1, prologue 19 bytes, 10 slots: sub rsp,88h (UWOP_ALLOC_LARGE, 17 in
// the next slot), then the eight pushes as UWOP_ALLOC_SMALL 8 at their end offsets.
const uint8_t kStackStubUnwind[24] = {0x01, 19, 10, 0x00, 19, 0x01, 17, 0x00, 12, 0x02, 10, 0x02,
                                      8,    0x02, 6, 0x02, 4, 0x02, 3, 0x02, 2, 0x02, 1, 0x02};
constexpr int kStackDepth = 24, kMaxStacks = 48;
struct StackRec {
  ULONG hash;
  LONG count;
  DWORD thread;
  USHORT n;
  void* frames[kStackDepth];
  uintptr_t args[4];  // rcx, rdx, r8, r9 of the first call with this stack
};
StackRec g_stacks[kMaxStacks];
LONG g_stacks_used = 0;  // under g_stack_lock
volatile LONG g_stack_calls = 0, g_stack_matched = 0, g_stack_dropped = 0;
uintptr_t g_stack_filter = 0;  // only calls whose first argument points at this qword (0: all)
SRWLOCK g_stack_lock = SRWLOCK_INIT;

// Return addresses above the caller, by unwinding with the registered function tables
// (the stub's own included; RtlCaptureStackBackTrace stops at the stub). A frame without
// a table entry is a leaf: its return address is at rsp. skip: frames left out at the top.
USHORT WalkStack(void** out, USHORT max, USHORT skip) {
  CONTEXT ctx;
  RtlCaptureContext(&ctx);
  USHORT n = 0;
  __try {
    for (int i = 0; i < 64 && n < max; ++i) {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
      if (f) {
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler_data, &frame, nullptr);
      } else {
        ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      }
      if (ctx.Rip < 0x10000) break;
      if (i >= skip) out[n++] = reinterpret_cast<void*>(ctx.Rip);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

// The stub calls this with the hooked function's own rcx, rdx, r8 and r9 still in place.
void StackHit(uintptr_t arg, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
  InterlockedIncrement(&g_stack_calls);
  uintptr_t first = 0;
  if (g_stack_filter && (!arg || !CopyGuarded(&first, arg, sizeof first) || first != g_stack_filter)) return;
  InterlockedIncrement(&g_stack_matched);
  void* f[kStackDepth];
  const USHORT n = WalkStack(f, kStackDepth, 1);  // 1: skip the stub
  ULONG hash = n;
  for (USHORT i = 0; i < n; ++i) hash = hash * 31 + (ULONG)(reinterpret_cast<uintptr_t>(f[i]) * 2654435761u);
  AcquireSRWLockExclusive(&g_stack_lock);
  LONG i = 0;
  while (i < g_stacks_used && (g_stacks[i].hash != hash || g_stacks[i].n != n)) ++i;
  if (i < g_stacks_used) {
    ++g_stacks[i].count;
  } else if (g_stacks_used < kMaxStacks) {
    StackRec& r = g_stacks[g_stacks_used++];
    r.hash = hash;
    r.count = 1;
    r.thread = GetCurrentThreadId();
    r.n = n;
    r.args[0] = arg;
    r.args[1] = arg2;
    r.args[2] = arg3;
    r.args[3] = arg4;
    memcpy(r.frames, f, n * sizeof(void*));
  } else {
    InterlockedIncrement(&g_stack_dropped);
  }
  ReleaseSRWLockExclusive(&g_stack_lock);
}

std::string Stacks(uintptr_t target, const std::string& entry_hex, unsigned ms, uintptr_t filter) {
  std::vector<uint8_t> entry;
  for (size_t i = 0; i < entry_hex.size(); i += 2) {
    const std::string b = entry_hex.substr(i, 2);
    char* end = nullptr;
    const unsigned long v = strtoul(b.c_str(), &end, 16);
    if (b.size() != 2 || !end || *end) return "err entry bytes: pairs of hex digits, no spaces\n";
    entry.push_back((uint8_t)v);
  }
  if (entry.size() < 5 || entry.size() > 24 || ms == 0 || ms > 30000)
    return "err usage: stacks <addr> <5..24 entry bytes, hex, whole instructions, not RIP-relative> <1..30000 ms> "
           "[first-argument qword hex]\n";
  if (target < Base() || target >= Base() + ImageSize()) return "err the address is not in the game's image\n";
  std::vector<uint8_t> have(entry.size());
  if (!SafeRead(have.data(), target, have.size()) || have != entry)
    return "err the entry bytes differ from the ones given; refused\n";
  const uintptr_t aligned = target & ~uintptr_t(7);
  const size_t o = target - aligned;
  if (o > 3) return "err the first 5 bytes cross an 8-byte boundary; cannot patch them in one write\n";
  // 16 slots of 256 bytes in a page reserved at start-up near the image (later the game's
  // heap fills that range), used in turn, each registered for unwinding once. The page stays
  // writable and executable: a call may still be inside an earlier slot.
  static LONG next_slot = 0;
  static bool registered[16] = {};
  uint8_t* pool = SimNearPage(0);
  if (!pool) return "err no code page near the image (reserved at start-up)\n";
  const LONG slot = next_slot++ % 16;
  uint8_t* page = pool + 256 * slot;
  const int64_t rel = reinterpret_cast<intptr_t>(page) - (intptr_t)(target + 5);
  if (rel < INT32_MIN || rel > INT32_MAX) return "err stub page out of jmp range\n";

  std::vector<uint8_t> s = {0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52,
                            0x41, 0x53, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x7F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x48, 0xB8});
  void* hit = reinterpret_cast<void*>(&StackHit);
  const auto* h = reinterpret_cast<const uint8_t*>(&hit);
  s.insert(s.end(), h, h + 8);
  s.insert(s.end(), {0xFF, 0xD0});
  for (uint8_t i = 0; i < 6; ++i) s.insert(s.end(), {0xF3, 0x0F, 0x6F, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + 16 * i)});
  s.insert(s.end(), {0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
                     0x5A, 0x59, 0x58, 0x9D});
  if (s.size() != kStackStubHead) return "err stub size mismatch\n";
  s.insert(s.end(), entry.begin(), entry.end());
  s.insert(s.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  const uint64_t back = target + entry.size();
  const auto* bb = reinterpret_cast<const uint8_t*>(&back);
  s.insert(s.end(), bb, bb + 8);
  DWORD old = 0;
  VirtualProtect(pool, 4096, PAGE_EXECUTE_READWRITE, &old);
  memcpy(page, s.data(), s.size());  // at most 160 bytes
  memcpy(page + 160, kStackStubUnwind, sizeof kStackStubUnwind);
  auto* fn = reinterpret_cast<RUNTIME_FUNCTION*>(page + 192);
  fn->BeginAddress = 0;
  fn->EndAddress = 160;
  fn->UnwindData = 160;
  FlushInstructionCache(GetCurrentProcess(), page, 256);
  if (!registered[slot]) registered[slot] = RtlAddFunctionTable(fn, 1, reinterpret_cast<DWORD64>(page)) != FALSE;

  AcquireSRWLockExclusive(&g_stack_lock);
  g_stacks_used = 0;
  ReleaseSRWLockExclusive(&g_stack_lock);
  InterlockedExchange(&g_stack_calls, 0);
  InterlockedExchange(&g_stack_matched, 0);
  InterlockedExchange(&g_stack_dropped, 0);
  g_stack_filter = filter;

  // The 5-byte jmp goes in with one aligned 8-byte write; the bytes after it are left as
  // they were (no new call reaches them; the stub runs them instead).
  uint64_t old_q = 0;
  if (!CopyGuarded(&old_q, aligned, 8)) return "err cannot read the entry\n";
  uint64_t new_q = old_q;
  auto* nb = reinterpret_cast<uint8_t*>(&new_q);
  nb[o] = 0xE9;
  const int32_t rel32 = (int32_t)rel;
  memcpy(nb + o + 1, &rel32, 4);
  DWORD prot = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(aligned), 8, PAGE_EXECUTE_READWRITE, &prot))
    return "err cannot make the entry writable\n";
#ifndef SOWGUARD_MUTANT  // negative control: the entry is never patched
  InterlockedExchange64(reinterpret_cast<volatile LONG64*>(aligned), (LONG64)new_q);
#endif
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(aligned), 8);
  SowLog(L"probe: recording call stacks at %S for %u ms", Where(target).c_str(), ms);
  Sleep(ms);
  InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(aligned), (LONG64)old_q, (LONG64)new_q);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(aligned), 8);
  VirtualProtect(reinterpret_cast<void*>(aligned), 8, prot, &old);
  Sleep(20);  // a call still inside StackHit finishes its record

  std::vector<StackRec> got;
  AcquireSRWLockExclusive(&g_stack_lock);
  got.assign(g_stacks, g_stacks + g_stacks_used);
  ReleaseSRWLockExclusive(&g_stack_lock);
  std::sort(got.begin(), got.end(), [](const StackRec& a, const StackRec& b) { return a.count > b.count; });
  const DWORD sim = InputGameThread();
  std::ostringstream out;
  out << "ok calls=" << g_stack_calls << " matched=" << g_stack_matched << " stacks=" << got.size()
      << " dropped=" << g_stack_dropped << "\n";
  for (const StackRec& r : got) {
    out << "stack count=" << r.count << " thread=" << r.thread << (sim && r.thread == sim ? " (simulation)" : "")
        << " args=" << Where(r.args[0]) << "," << Where(r.args[1]) << "," << Where(r.args[2]) << "," << Where(r.args[3])
        << "\n ";
    for (USHORT i = 0; i < r.n; ++i) out << " " << Where(reinterpret_cast<uintptr_t>(r.frames[i]));
    out << "\n";
  }
  return out.str();
}

const char* DpStateName() {
  switch (g_dp_state) {
    case kDpHooked: return "hooked";
    case kDpRefused: return "refused";
    default: return "off";
  }
}

size_t ScanGuarded(const uint8_t* p, size_t n, const int* pat, size_t m, uintptr_t* hits, size_t room) {
  size_t found = 0;
  __try {
    for (size_t i = 0; i + m <= n && found < room; ++i) {
      size_t j = 0;
      while (j < m && (pat[j] < 0 || p[i + j] == pat[j])) ++j;
      if (j == m) hits[found++] = reinterpret_cast<uintptr_t>(p + i);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return found;
}

bool ParsePattern(const std::string& text, std::vector<int>* out) {
  std::istringstream in(text);
  std::string tok;
  out->clear();
  while (in >> tok) {
    if (tok == "?" || tok == "??") out->push_back(-1);
    else if (tok.size() == 2 && isxdigit((unsigned char)tok[0]) && isxdigit((unsigned char)tok[1]))
      out->push_back((int)strtoul(tok.c_str(), nullptr, 16));
    else return false;
  }
  return !out->empty() && (*out)[0] >= 0;
}

// Hits of a pattern in the exe's readable memory, as absolute addresses.
std::vector<uintptr_t> Scan(const std::vector<int>& pat, size_t max_hits) {
  std::vector<uintptr_t> hits;
  const uintptr_t base = Base(), end = base + ImageSize();
  uintptr_t a = base;
  std::vector<uintptr_t> chunk(max_hits);
  while (a < end && hits.size() < max_hits) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
    uintptr_t r_end = std::min<uintptr_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, end);
    if (mbi.State == MEM_COMMIT && ReadableProtect(mbi.Protect)) {
      size_t n = ScanGuarded(reinterpret_cast<const uint8_t*>(a), r_end - a, pat.data(), pat.size(), chunk.data(),
                             max_hits - hits.size());
      hits.insert(hits.end(), chunk.begin(), chunk.begin() + n);
    }
    a = r_end;
  }
  return hits;
}

bool ParseAddr(const std::string& s, uintptr_t* out) {
  char* end = nullptr;
  if (s.rfind("rva:", 0) == 0) {
    *out = Base() + strtoull(s.c_str() + 4, &end, 16);
  } else if (s.rfind("abs:", 0) == 0) {
    *out = strtoull(s.c_str() + 4, &end, 16);
  } else {
    return false;
  }
  return end && *end == '\0';
}

std::string Hex(uintptr_t v) {
  char b[24];
  snprintf(b, sizeof b, "%llX", (unsigned long long)v);
  return b;
}

std::string Where(uintptr_t a) {
  const uintptr_t base = Base();
  if (a >= base && a < base + ImageSize()) return "rva:" + Hex(a - base);
  return "abs:" + Hex(a);
}

// "graphwatch"/"ctrlwatch" after the time: [player] [then <probe command>], the command run once
// recording has begun.
void WatchArgs(std::istringstream& in, bool* player, std::string* then) {
  std::string w;
  while (in >> w) {
    if (w == "player") {
      *player = true;
    } else if (w == "then") {
      std::getline(in, *then);
      const size_t b = then->find_first_not_of(' ');
      *then = b == std::string::npos ? std::string() : then->substr(b);
      return;
    }
  }
}

std::string Handle(const std::string& line) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  std::ostringstream out;
  if (cmd == "ping") {
    out << "ok pid=" << GetCurrentProcessId() << " base=" << Hex(Base()) << " ready=" << g_ready
        << " ready_ms=" << g_ready_ms << " write=" << (g_cfg.write ? 1 : 0) << " debug_print=" << DpStateName()
        << "\n";
  } else if (cmd == "module") {
    const uintptr_t base = Base();
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    out << "ok base=" << Hex(base) << " size=" << Hex(nt->OptionalHeader.SizeOfImage) << "\n";
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
      char name[9] = {};
      memcpy(name, sec->Name, 8);
      for (char& c : name)
        if (c && (c < 0x20 || c > 0x7E)) c = '?';
      MEMORY_BASIC_INFORMATION mbi{};
      VirtualQuery(reinterpret_cast<void*>(base + sec->VirtualAddress), &mbi, sizeof mbi);
      out << "section " << name << " rva=" << Hex(sec->VirtualAddress) << " vsize=" << Hex(sec->Misc.VirtualSize)
          << " protect=" << Hex(mbi.Protect) << "\n";
    }
  } else if (cmd == "scan") {
    std::string rest;
    std::getline(in, rest);
    size_t max_hits = 64;
    size_t pos = rest.find(" max=");
    if (pos != std::string::npos) {
      max_hits = std::min<size_t>(strtoul(rest.c_str() + pos + 5, nullptr, 10), 4096);
      rest.resize(pos);
    }
    std::vector<int> pat;
    if (!ParsePattern(rest, &pat)) return "err bad pattern (hex bytes and ?, first byte not ?)\n";
    ULONGLONG t0 = GetTickCount64();
    auto hits = Scan(pat, max_hits);
    out << "ok hits=" << hits.size() << " ms=" << GetTickCount64() - t0 << "\n";
    for (uintptr_t h : hits) out << Where(h) << "\n";
  } else if (cmd == "read") {
    std::string a, l;
    uintptr_t addr = 0;
    in >> a >> l;
    size_t len = strtoul(l.c_str(), nullptr, 0);
    if (!ParseAddr(a, &addr) || len == 0 || len > 4096) return "err usage: read <rva:hex|abs:hex> <len 1..4096>\n";
    std::vector<uint8_t> buf(len);
    if (!SafeRead(buf.data(), addr, len)) return "err unreadable " + Where(addr) + "\n";
    out << "ok " << Where(addr) << "\n";
    char b[4];
    for (size_t i = 0; i < len; ++i) {
      snprintf(b, sizeof b, "%02x", buf[i]);
      out << b << ((i % 32 == 31 || i + 1 == len) ? "\n" : " ");
    }
  } else if (cmd == "rip") {
    std::string a;
    int dispoff = 0, len = 0;
    uintptr_t addr = 0;
    in >> a >> dispoff >> len;
    int32_t disp = 0;
    if (!ParseAddr(a, &addr) || dispoff <= 0 || len <= dispoff) return "err usage: rip <addr> <dispoff> <len>\n";
    if (!SafeRead(&disp, addr + dispoff, 4)) return "err unreadable " + Where(addr) + "\n";
    out << "ok " << Where(addr + len + disp) << "\n";
  } else if (cmd == "deref") {
    std::string a;
    uintptr_t addr = 0, v = 0;
    in >> a;
    if (!ParseAddr(a, &addr)) return "err usage: deref <addr> [off ...]\n";
    if (!SafeRead(&v, addr, 8)) return "err unreadable " + Where(addr) + "\n";
    out << "ok\n" << Where(addr) << " -> " << Hex(v) << "\n";
    std::string off;
    while (in >> off) {
      uintptr_t at = v + strtoull(off.c_str(), nullptr, 0);
      if (!SafeRead(&v, at, 8)) {
        out << Where(at) << " -> unreadable\n";
        break;
      }
      out << Where(at) << " -> " << Hex(v) << "\n";
    }
  } else if (cmd == "write") {
    if (!g_cfg.write) return "err write disabled (probe_write=0)\n";
    std::string a, rest;
    uintptr_t addr = 0;
    in >> a;
    std::getline(in, rest);
    std::vector<int> bytes;
    if (!ParseAddr(a, &addr) || !ParsePattern(rest, &bytes) || bytes.size() > 256)
      return "err usage: write <addr> <hex bytes, max 256, no ?>\n";
    std::vector<uint8_t> data;
    for (int b : bytes) {
      if (b < 0) return "err usage: write <addr> <hex bytes, max 256, no ?>\n";
      data.push_back((uint8_t)b);
    }
    if (!SafeWrite(addr, data.data(), data.size())) return "err not writable " + Where(addr) + "\n";
    SowLog(L"probe: wrote %zu byte(s) at %S", data.size(), Where(addr).c_str());
    out << "ok wrote " << data.size() << " at " << Where(addr) << "\n";
  } else if (cmd == "dbg" || cmd == "dbgstats") {
    if (g_dp_state != kDpHooked) return std::string("err debug print ") + DpStateName() + "\n";
    std::string count;
    in >> count;
    size_t n = count.empty() ? 20 : strtoul(count.c_str(), nullptr, 10);
    EnterCriticalSection(&g_dp_lock);
    out << "ok lines=" << g_dp_lines << " bytes=" << g_dp_bytes << " dropped=" << g_dp_dropped << "\n";
    if (cmd == "dbg") {
      n = std::min<size_t>(n, g_dp_ring.size());
      for (size_t i = g_dp_ring.size() - n; i < g_dp_ring.size(); ++i) out << g_dp_ring[i] << "\n";
    } else {
      for (const auto& kv : g_dp_counts) out << kv.second << "\t" << kv.first << "\n";
    }
    LeaveCriticalSection(&g_dp_lock);
  } else if (cmd == "findq") {
    // Read-only: every 8-byte-aligned copy of a value in private read-write memory (the
    // heap), e.g. all objects of one class by their vtable pointer.
    std::string v, m;
    in >> v >> m;
    if (v.empty()) return "err usage: findq <hex value> [max hits]\n";
    const uint64_t value = strtoull(v.c_str(), nullptr, 16);
    const size_t max_hits = m.empty() ? 64 : std::min<size_t>(strtoul(m.c_str(), nullptr, 10), 4096);
    const ULONGLONG t0 = GetTickCount64();
    std::vector<uintptr_t> hits(max_hits);
    size_t scanned = 0;
    hits.resize(SimScanHeap(value, hits.data(), hits.size(), &scanned));
    out << "ok hits=" << hits.size() << " resident_mb=" << scanned / (1 << 20) << " ms=" << GetTickCount64() - t0
        << "\n";
    for (uintptr_t h : hits) out << Where(h) << "\n";
  } else if (cmd == "player") {
    // Steam build 3095001: [GameClient]+0x888 is the player (position +0xAC, rotation
    // quaternion +0xB8: what Teleport and Player.SetOrientation use); its +0x26A8 is the
    // character the damage code works on (max health +0x428, health +0x42C, fraction +0x430).
    uintptr_t gc = 0, player = 0, ch = 0;
    if (!SafeRead(&gc, Base() + g_cfg.gameclient_rva, 8) || !gc) return "err no GameClient\n";
    if (!SafeRead(&player, gc + 0x888, 8) || !player) return "err no player (not in the world?)\n";
    float pos[3], rot[4];
    if (!SafeRead(pos, player + 0xAC, sizeof pos) || !SafeRead(rot, player + 0xB8, sizeof rot))
      return "err unreadable player " + Where(player) + "\n";
    char b[320];
    snprintf(b, sizeof b, "ok player=%s pos=%.3f,%.3f,%.3f rot=%.4f,%.4f,%.4f,%.4f", Where(player).c_str(), pos[0],
             pos[1], pos[2], rot[0], rot[1], rot[2], rot[3]);
    out << b;
    float h[3];
    if (SafeRead(&ch, player + 0x26A8, 8) && ch && SafeRead(h, ch + 0x428, sizeof h)) {
      snprintf(b, sizeof b, " character=%s health=%.2f max=%.2f frac=%.3f", Where(ch).c_str(), h[1], h[0], h[2]);
      out << b;
    }
    out << "\n";
  } else if (cmd == "pad") {
    if (!g_cfg.write) return "err pad disabled (probe_write=0)\n";
    std::string buttons;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    unsigned ms = 0;
    if (!(in >> buttons >> lx >> ly >> rx >> ry >> ms))
      return "err usage: pad <buttons hex> <left x> <left y> <right x> <right y> <ms>\n";
    std::string err;
    if (!InputHold((unsigned)strtoul(buttons.c_str(), nullptr, 16), lx, ly, rx, ry, ms, &err)) return "err " + err + "\n";
    out << "ok holding for " << ms << " ms\n";
  } else if (cmd == "save") {
    if (!g_cfg.write) return "err save disabled (probe_write=0)\n";
    std::string err;
    if (!InputRequestSave(g_cfg.save_handler_rva, g_cfg.gameclient_rva, &err)) return "err " + err + "\n";
    out << "ok Player.Save queued for the next pad read\n";
  } else if (cmd == "lkp") {
    if (!g_cfg.write) return "err lkp disabled (probe_write=0)\n";
    float pos[3];
    if (!(in >> pos[0] >> pos[1] >> pos[2])) return "err usage: lkp <x> <y> <z>\n";
    std::string err;
    if (!SimRequestLkp(g_cfg.lkp_rva, g_cfg.gameclient_rva, pos, &err)) return "err " + err + "\n";
    out << "ok last-known-position effect queued for the simulation thread (shown so far: " << SimLkpShown() << ")\n";
  } else if (cmd == "teleport") {
    if (!g_cfg.write) return "err teleport disabled (probe_write=0)\n";
    float pos[3];
    if (!(in >> pos[0] >> pos[1] >> pos[2])) return "err usage: teleport <x> <y> <z>\n";
    std::string err;
    if (!SimRequestPlayerTeleport(g_cfg.teleport_rva, g_cfg.gameclient_rva, pos, &err)) return "err " + err + "\n";
    out << "ok player teleport queued for the simulation thread\n";
  } else if (cmd == "spawn") {
    if (!g_cfg.write) return "err spawn disabled (probe_write=0)\n";
    std::string record, look;
    float pos[3];
    in >> record;
    if (record.empty()) return "err usage: spawn <character type> [x y z [appearance]]\n";
    const bool at = static_cast<bool>(in >> pos[0] >> pos[1] >> pos[2]);
    if (at) in >> look;
    SimSpawnRvas r;
    r.gameclient_rva = g_cfg.gameclient_rva;
    std::string report;
    if (!SimSpawn(r, record.c_str(), at ? pos : nullptr, look.empty() ? nullptr : look.c_str(), &report))
      return "err " + report + "\n";
    out << "ok " << report << "\n";
  } else if (cmd == "animres") {
    if (!g_cfg.write) return "err animres disabled (probe_write=0)\n";
    std::string a, b, err;
    uintptr_t ch = 0, res = 0;
    in >> a >> b;
    if (!ParseAddr(a, &ch) || !ParseAddr(b, &res)) return "err usage: animres <character> <behavior resource>\n";
    if (!SimSetBehaviorResource(SimSpawnRvas().behavior_rva, ch, res, &err)) return "err " + err + "\n";
    out << "ok queued for the simulation thread\n";
  } else if (cmd == "watch") {
    return SimWatchReport();
  } else if (cmd == "look") {
    if (!g_cfg.write) return "err look disabled (probe_write=0)\n";
    std::string base, donor, tok;
    std::vector<unsigned> offsets;
    in >> base >> donor;
    while (in >> tok) offsets.push_back((unsigned)strtoul(tok.c_str(), nullptr, 16));
    if (base.empty() || donor.empty() || offsets.empty())
      return "err usage: look <base appearance> <donor appearance> <field offset hex> ...\n";
    std::string report;
    if (!SimMakeLook(SimSpawnRvas().appearance_lookup_rva, base.c_str(), offsets, donor.c_str(), &report))
      return "err " + report + "\n";
    out << "ok " << report << "\n";
  } else if (cmd == "count") {
    if (!g_cfg.write) return "err count disabled (probe_write=0)\n";
    std::string vt;
    unsigned first = 0, n = 0, ms = 0;
    if (!(in >> vt >> first >> n >> ms)) return "err usage: count <vtable rva hex> <first slot> <slots> <ms>\n";
    return SimCountSlots(strtoull(vt.c_str(), nullptr, 16), first, n, ms);
  } else if (cmd == "puppet") {
    std::string a, r;
    in >> a;
    if (a.empty()) {
      out << "ok " << SimPuppetStatus() << " " << SimPuppetDetail() << "\n";
    } else if (!g_cfg.write) {
      return "err puppet disabled (probe_write=0)\n";
    } else if (a == "ai") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet ai on|off\n";
      if (v == "off" && !SimRequestBrainHook(g_cfg.brain_site_rva, g_cfg.tree_rva, &r)) return "err " + r + "\n";
      SimSetPuppetAi(v == "on");
      out << "ok puppet ai " << v << "\n";
    } else if (a == "tp") {
      float at[3];
      if (!(in >> at[0] >> at[1] >> at[2])) return "err usage: puppet tp <x> <y> <z> [yaw degrees]\n";
      float yaw = 0;
      const bool has_yaw = static_cast<bool>(in >> yaw);
      float cur[3], rot[4];
      if (!SimPuppetXform(cur, rot)) return "err no puppet\n";
      if (has_yaw) {
        const float h = yaw * 3.14159265f / 360.0f;  // half the angle, in radians
        rot[0] = 0;
        rot[1] = sinf(h);
        rot[2] = 0;
        rot[3] = cosf(h);
      }
      if (!SimRequestPuppetMove(g_cfg.teleport_rva, at, rot, &r)) return "err " + r + "\n";
      out << "ok queued\n";
    } else if (a == "walk" || a == "idle") {
      float dest[3] = {}, scale = 1.0f;
      int type = 0;
      if (a == "walk" && !(in >> dest[0] >> dest[1] >> dest[2] >> type))
        return "err usage: puppet walk <x> <y> <z> <1 walk | 2 run> [speed factor]\n";
      in >> scale;
      if (!SimPuppetWalk(g_cfg.move_request_rva, dest, a == "walk" ? type : 0, scale, &r)) return "err " + r + "\n";
      out << "ok queued for the simulation thread\n";
    } else if (a == "snap") {
      float units = -1;
      if (!(in >> units) || units < 1) return "err usage: puppet snap <units, at least 1>\n";
      CoopSetAnimSnap(units);
      out << "ok puppet snap " << units << "\n";
    } else if (a == "inputs") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet inputs on|off\n";
      CoopSetInputs(v == "on");
      out << "ok puppet inputs " << v << "\n";
    } else if (a == "hold") {
      unsigned ms = 0;
      int index[16], n = 0;
      float values[16];
      std::string tok;
      in >> ms;
      while (n < 16 && in >> tok) {
        const size_t eq = tok.find('=');
        index[n] = atoi(tok.substr(0, eq).c_str());
        values[n] = eq == std::string::npos ? 1.0f : (float)atof(tok.substr(eq + 1).c_str());
        ++n;
      }
      return SimPuppetHold(ms, index, values, n);
    } else if (a == "actions") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet actions on|off\n";
      CoopSetActions(v == "on");
      out << "ok puppet actions " << v << "\n";
    } else if (a == "follower") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet follower on|off\n";
      CoopSetFollower(v == "on");
      out << "ok puppet follower " << v << "\n";
    } else if (a == "targetable") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet targetable on|off\n";
      CoopSetTargetable(v == "on");
      out << "ok puppet targetable " << v << "\n";
    } else if (a == "play") {
      std::string name;
      int flag = 0;
      in >> name >> flag;
      if (name.empty()) return "err usage: puppet play <action name> [flag 0|1]\n";
      return SimPuppetPlay(0x664CA4, name.c_str(), flag);
    } else if (a == "move") {
      std::string v, w1, w2;
      in >> v >> w1 >> w2;
      char* e = nullptr;
      const unsigned long id = strtoul(v.c_str(), &e, 16);
      float speed = 1.0f;
      bool player = false;
      for (const std::string& w : {w1, w2}) {
        if (w == "player") player = true;
        else if (!w.empty()) speed = strtof(w.c_str(), nullptr);
      }
      if (v.empty() || *e || speed <= 0) return "err usage: puppet move <attack id, hex> [speed] [player]\n";
      return SimPuppetMove(g_cfg.attack_table_rva, (uint32_t)id, speed, player ? g_cfg.gameclient_rva : 0);
    } else if (a == "vis") {
      std::string v;
      in >> v;
      SimSetVisMask((unsigned)strtoul(v.c_str(), nullptr, 16));
      out << "ok puppet vis " << v << std::string(1, 10);
    } else if (a == "assist") {
      std::string v;
      in >> v;
      if (v.empty() || v.size() > 60) return "err usage: puppet assist <action name>|off" + std::string(1, 10);
      SimSetAssistJump(v.c_str());
      out << "ok puppet assist " << v << std::string(1, 10);
    } else if (a == "moves") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet moves on|off" + std::string(1, 10);
      SimSetAttackMoves(v == "on");
      out << "ok puppet moves " << v << " (" << SimAttackMovesPlayed() << " played)" << std::string(1, 10);
    } else if (a == "attacks") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet attacks on|off\n";
      CoopSetAttacks(v == "on");
      out << "ok puppet attacks " << v << "\n";
    } else if (a == "attack") {
      std::string v, who;
      in >> v >> who;
      char* e = nullptr;
      const unsigned long id = strtoul(v.c_str(), &e, 16);
      if (v.empty() || *e || (!who.empty() && who != "player" && who != "at"))
        return "err usage: puppet attack <attack id, hex> [player | at]\n";
      return SimPuppetAttack(g_cfg.attack_thunk_rva, g_cfg.attack_table_rva, (uint32_t)id,
                             who == "player" ? g_cfg.gameclient_rva : 0, who == "at");
    } else if (a == "reset") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet reset on|off\n";
      CoopSetPutBackResets(v == "on");
      out << "ok puppet reset " << v << "\n";
    } else if (a == "air") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet air on|off\n";
      CoopSetAirFollow(v == "on");
      out << "ok puppet air " << v << "\n";
    } else if (a == "speed") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet speed on|off\n";
      CoopSetSpeed(v == "on");
      out << "ok puppet speed " << v << "\n";
    } else if (a == "mirror") {
      std::string v;
      in >> v;
      if (v != "on" && v != "off") return "err usage: puppet mirror on|off\n";
      if (v == "on" && !SimAnimInputsInstall(g_cfg.anim_var_rva, g_cfg.gameclient_rva, &r)) return "err " + r + "\n";
      SimSetMirror(v == "on");
      out << "ok puppet mirror " << v << "\n";
    } else if (a == "follow") {
      std::string v;
      in >> v;
      const int f = v == "anim" ? 4 : v == "both" ? 3 : v == "walk" ? 2 : v == "teleport" ? 1 : v == "none" ? 0 : -1;
      if (f < 0) return "err usage: puppet follow anim|walk|teleport|both|none\n";
      CoopSetFollow(f);
      out << "ok puppet follow " << v << "\n";
    } else {
      if (!g_cfg.write) return "err puppet disabled (probe_write=0)\n";
      uintptr_t addr = 0;
      if (a == "nearest") {
        if (!SimPickNearestPuppet(g_cfg.gameclient_rva, &r)) return "err " + r + "\n";
        out << "ok " << r << "\n";
      } else if (a == "off") {
        SimSetPuppet(0, &r);
        out << "ok puppet none\n";
      } else if (ParseAddr(a, &addr)) {
        if (!SimSetPuppet(addr, &r)) return "err " + r + "\n";
        out << "ok " << SimPuppetStatus() << "\n";
      } else {
        return "err usage: puppet [nearest | off | abs:<character> | ai on|off | walk x y z type | idle | "
               "follow anim|walk|teleport|both|none | inputs on|off | actions on|off | follower on|off | targetable on|off | air on|off | "
               "speed on|off | snap <units> | mirror on|off | hold <ms> <index>[=value]...]\n";
      }
    }
  } else if (cmd == "stacks") {
    if (!g_cfg.write) return "err stacks disabled (probe_write=0)\n";
    std::string a, bytes, f;
    unsigned ms = 0;
    in >> a >> bytes >> ms >> f;
    uintptr_t addr = 0;
    if (!ParseAddr(a, &addr) || bytes.empty() || !ms)
      return "err usage: stacks <addr> <entry bytes hex> <ms> [first-argument qword hex]\n";
    return Stacks(addr, bytes, ms, f.empty() ? 0 : strtoull(f.c_str(), nullptr, 16));
  } else if (cmd == "remote") {
    out << CoopStatus();
  } else if (cmd == "inputs") {
    out << SimInputsReport();
  } else if (cmd == "enemies") {
    out << CoopEnemyStatus();
  } else if (cmd == "adopt") {
    // Experiment: make a character a proxy (its behavior tree skipped, a follower) / release it; "registry" starts the
    // enemy registry (needed for the skip).
    std::string a, b, r;
    in >> a >> b;
    if (a == "registry") {
      if (!SimEnemyRegistryStart(g_cfg.brain_site_rva, g_cfg.tree_rva, &r)) return "err " + r + "\n";
      return "ok registry on\n";
    }
    uintptr_t who = 0;
    if (!ParseAddr(a, &who) || !who) return "err usage: adopt registry | adopt abs:<character> [off]\n";
    if (b == "off") {
      SimProxyDrop(who);
      return "ok released\n";
    }
    return SimProxyAdopt(who) ? "ok adopted\n" : "err not a character with a health object pointing back at it\n";
  } else if (cmd == "retarget") {
    std::string a, b;
    uintptr_t enemy = 0, target = 0;
    in >> a >> b;
    if (a == "show") {
      ParseAddr(b, &enemy);
      char t[64];
      snprintf(t, sizeof t, "ok target abs:%llX retargeted=%ld\n", (unsigned long long)SimEnemyTarget(enemy), SimRetargeted());
      return t;
    }
    if (!ParseAddr(a, &enemy) || !enemy || (b != "off" && (!ParseAddr(b, &target) || !target)))
      return "err usage: retarget abs:<enemy> abs:<target character> | retarget abs:<enemy> off | retarget show abs:<enemy>\n";
    out << (SimRetarget(enemy, b == "off" ? 0 : target) ? "ok\n" : "err the table is full\n");
  } else if (cmd == "standinhit") {
    std::string a, id;
    uintptr_t who = 0;
    in >> a >> id;
    if (!ParseAddr(a, &who) || !who || id.empty()) return "err usage: standinhit abs:<enemy> <attack id, hex>\n";
    out << CoopTestStandinHit(who, (uint32_t)strtoul(id.c_str(), nullptr, 16));
  } else if (cmd == "damage") {
    std::string a, id;
    uintptr_t who = 0;
    in >> a >> id;
    if (!ParseAddr(a, &who) || !who || id.empty()) return "err usage: damage abs:<proxy character> <attack id, hex>\n";
    out << CoopTestDamage(who, (uint32_t)strtoul(id.c_str(), nullptr, 16));
  } else if (cmd == "despawn") {
    if (!g_cfg.write) return "err despawn disabled (probe_write=0)\n";
    std::string a, r;
    uintptr_t at = 0;
    in >> a;
    if (!ParseAddr(a, &at) || !at) return "err usage: despawn abs:<character>\n";
    if (!SimDespawnOffscreen(0x1A8BB74, at, &r)) return "err " + r + "\n";
    out << "ok queued for the simulation thread\n";
  } else if (cmd == "ambient") {
    if (!g_cfg.write) return "err ambient disabled (probe_write=0)\n";
    std::string v, r;
    in >> v;
    if (v == "on" || v == "off") {
      if (!SimAmbientHookInstall(0x166DF8, &r)) return "err " + r + "\n";
      SimAmbientBlock(v == "on");
    } else if (!v.empty()) {
      return "err usage: ambient [on|off]  (on: no new ambient characters)\n";
    }
    out << SimAmbientReport();
  } else if (cmd == "graphwatch") {
    if (!g_cfg.write) return "err graphwatch disabled (probe_write=0)\n";
    unsigned ms = 0;
    bool player = false;
    std::string then;
    in >> ms;
    WatchArgs(in, &player, &then);
    return SimGraphWatch(0x195988, player, g_cfg.gameclient_rva, ms, [&] {
      if (!then.empty()) Handle(then);
    });
  } else if (cmd == "ctrlwatch") {
    if (!g_cfg.write) return "err ctrlwatch disabled (probe_write=0)\n";
    unsigned ms = 0;
    bool player = false;
    std::string then;
    in >> ms;
    WatchArgs(in, &player, &then);
    return SimCtrlWatch(0x41A1E0, player, g_cfg.gameclient_rva, ms, [&] {
      if (!then.empty()) Handle(then);
    });
  } else if (cmd == "trace") {
    if (!g_cfg.write) return "err trace disabled (probe_write=0)\n";
    std::string a, f, n;
    unsigned ms = 0;
    uintptr_t addr = 0, rcx = 0;
    in >> a >> ms >> f >> n;
    if (f == "names" || f == "args" || f == "r8") std::swap(f, n);
    if (!ParseAddr(a, &addr) || !ms || ms > 10000 || (!f.empty() && !ParseAddr(f, &rcx)) ||
        (!n.empty() && n != "names" && n != "args" && n != "r8"))
      return "err usage: trace <addr> <1..10000 ms> [rcx abs:<hex>] [names|args|r8]\n";
    return SimTraceCalls(addr, rcx, ms, 0, n == "names", false, n == "args" || n == "r8", n == "r8");
  } else if (cmd == "wtrace") {
    if (!g_cfg.write) return "err wtrace disabled (probe_write=0)\n";
    std::string a, l, rw;
    unsigned ms = 0, len = 4;
    uintptr_t addr = 0;
    in >> a >> ms >> l >> rw;
    if (!l.empty()) len = (unsigned)strtoul(l.c_str(), nullptr, 10);
    if (!ParseAddr(a, &addr) || !ms || ms > 10000 || (len != 1 && len != 2 && len != 4 && len != 8) ||
        (!rw.empty() && rw != "rw"))
      return "err usage: wtrace <addr> <1..10000 ms> [1|2|4|8 bytes, default 4] [rw: reads too]\n";
    return SimTraceCalls(addr, 0, ms, len, false, rw == "rw");
  } else {
    return "err unknown command (ping, module, scan, read, rip, deref, dbg, dbgstats, write, player, pad, save, lkp, "
           "count, findq, inputs, look, puppet, remote, spawn, stacks, teleport, trace, wtrace)\n";
  }
  return out.str();
}

void Serve(HANDLE pipe) {
  std::string pending;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got) {
    pending.append(buf, got);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      std::string reply = Handle(line) + "END\n";
      DWORD w = 0;
      if (!WriteFile(pipe, reply.data(), (DWORD)reply.size(), &w, nullptr)) return;
    }
  }
}

DWORD WINAPI PipeThread(void*) {
  wchar_t name[64];
  swprintf_s(name, L"\\\\.\\pipe\\sowprobe-%lu", GetCurrentProcessId());
  HANDLE f = CreateFileW((g_cfg.profile + L"\\sowprobe.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    char line[96];
    int n = snprintf(line, sizeof line, "%ls\r\n", name);
    DWORD w = 0;
    WriteFile(f, line, (DWORD)n, &w, nullptr);
    CloseHandle(f);
  }
  SowLog(L"probe: serving %s", name);
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                   1 << 16, 1 << 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      SowLog(L"probe: CreateNamedPipe failed (%lu), probe stopped", GetLastError());
      return 0;
    }
    if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) Serve(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

DWORD WINAPI ReadyThread(void*) {
  std::vector<int> pat;
  if (!ParsePattern(g_cfg.ready_pattern, &pat)) {
    SowLog(L"probe: bad probe_ready_pattern, not waiting");
    return 0;
  }
  const ULONGLONG t0 = GetTickCount64();
  while (GetTickCount64() - t0 < g_cfg.wait_sec * 1000ull) {
    auto hits = Scan(pat, 4);
    if (!hits.empty()) {
      g_ready_ms = (LONG64)(GetTickCount64() - t0);
      InterlockedExchange(&g_ready, 1);
      SowLog(L"probe: ready after %lld ms (%zu hit(s) of the ready pattern, first %S)", g_ready_ms, hits.size(),
             Where(hits[0]).c_str());
      if (g_cfg.hook_debug_print) {
        if (g_cfg.write || g_cfg.coop) InstallDebugPrintHook();
        else SowLog(L"probe: hook_debug_print=1 needs probe_write=1 or a coop_role, not hooked");
      }
      return 0;
    }
    Sleep(500);
  }
  SowLog(L"probe: ready pattern not seen within %u s", g_cfg.wait_sec);
  return 0;
}

}  // namespace

void StartProbe(const ProbeConfig& cfg) {
  g_cfg = cfg;
  // Threads start running once the loader lock is released, after DllMain returns.
  HANDLE a = CreateThread(nullptr, 0, ReadyThread, nullptr, 0, nullptr);
  HANDLE b = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
  if (a) CloseHandle(a);
  if (b) CloseHandle(b);
  SowLog(L"probe: started (ready pattern \"%S\", wait %u s)", cfg.ready_pattern.c_str(), cfg.wait_sec);
}
