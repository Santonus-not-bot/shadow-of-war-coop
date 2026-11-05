// input: a virtual controller for unattended test runs (autoload=1).
//
// The game's own XInputGetState calls see a connected pad on user 0 whose A button this
// thread presses on a schedule, so a test copy gets from the title screen into the story
// with no one at the keyboard (title: "press any key"; then the story in the campaign
// list; then one more screen). Pressing stops as soon as the game's debug print says the
// world is starting ("[QUESTMGR]: GameModeSet - ...", needs hook_debug_print=1 with the
// QUESTMGR channel on), or after max_presses. "[GAMEMODE|SESSION]: Load:" comes earlier,
// when the campaign's save has been read, and is only logged.
// A real pad on user 0 keeps working: its state is passed on with A added.
//
// The game imports XInputGetState from XINPUT9_1_0.dll, and SteamStub writes the real
// address into the game's rebuilt import table (the first pages of .xtext) at start-up.
// This thread waits for that slot and points it at PadGetState; nothing else in the game
// changes. The game reads pads even when its window is not focused (seen 2026-10-21: one
// real pad drove both test copies), but it pauses itself when the window loses focus; see
// background_run below. sim_hook is installed from this thread too (see simhook.h).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "input.h"
#include "probe.h"
#include "simhook.h"

#pragma comment(lib, "user32.lib")  // FindWindowExW (the game loads user32 anyway)

namespace {

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

InputConfig g_cfg;
XInputGetState_t g_real = nullptr;
volatile LONG g_installed = 0;  // the import slot points at PadGetState
volatile LONG g_buttons = 0;    // virtual buttons currently held
volatile LONG g_packet = 0;     // bumped on every virtual change, added to the packet number
HANDLE g_loaded = nullptr;      // set when the game reports the world starting

// Probe "pad": a timed hold of buttons and sticks.
struct Hold {
  WORD buttons = 0;
  BYTE lt = 0, rt = 0;  // triggers: bits 16-23 and 24-31 of the probe's buttons value
  SHORT lx = 0, ly = 0, rx = 0, ry = 0;
  ULONGLONG until = 0;
  bool ended = true;
};
Hold g_hold;
CRITICAL_SECTION g_hold_lock;

// Probe "save": runs from the game's own pad read, once the world has started. The game
// reads the pad on its input thread, not on the simulation thread that logs the world
// start (live: 20172 vs 27508). That is fine for Player.Save: it only arms a 0.5 s timer
// on the player (42D9A8 -> FF0B4, four plain stores) that the simulation thread checks.
using Handler_t = void(__cdecl*)(int, const char**);
volatile DWORD g_game_thread = 0;  // thread that logged the world start; 0 until then
volatile LONG g_save_pending = 0;
Handler_t g_save_handler = nullptr;
volatile LONG g_pad_thread_logged = 0;

void RunPadThreadWork() {
  const DWORD me = GetCurrentThreadId();
  if (InterlockedCompareExchange(&g_pad_thread_logged, 1, 0) == 0)
    SowLog(L"input: the game reads the pad on thread %lu", me);
  if (!g_game_thread) return;
  if (g_save_pending && InterlockedExchange(&g_save_pending, 0) != 0) {
    SowLog(L"input: running Player.Save on the pad thread %lu (simulation thread %lu)", me, g_game_thread);
    g_save_handler(0, nullptr);
    SowLog(L"input: Player.Save returned");
  }
}

DWORD WINAPI PadGetState(DWORD user, XINPUT_STATE* state) {
  DWORD r = g_real(user, state);
  if (user != 0 || !state) return r;
  RunPadThreadWork();
  if (r != ERROR_SUCCESS) {  // no real pad on user 0: the virtual one is connected
    memset(state, 0, sizeof *state);
    r = ERROR_SUCCESS;
  }
  state->Gamepad.wButtons |= (WORD)g_buttons;
  EnterCriticalSection(&g_hold_lock);
  if (GetTickCount64() < g_hold.until) {
    state->Gamepad.wButtons |= g_hold.buttons;
    if (g_hold.lt) state->Gamepad.bLeftTrigger = g_hold.lt;
    if (g_hold.rt) state->Gamepad.bRightTrigger = g_hold.rt;
    if (g_hold.lx || g_hold.ly) {
      state->Gamepad.sThumbLX = g_hold.lx;
      state->Gamepad.sThumbLY = g_hold.ly;
    }
    if (g_hold.rx || g_hold.ry) {
      state->Gamepad.sThumbRX = g_hold.rx;
      state->Gamepad.sThumbRY = g_hold.ry;
    }
  } else if (!g_hold.ended) {
    g_hold.ended = true;
    InterlockedIncrement(&g_packet);
  }
  LeaveCriticalSection(&g_hold_lock);
  state->dwPacketNumber += (DWORD)g_packet;
  return r;
}

struct Section {
  uint8_t* start = nullptr;
  size_t size = 0;
};

Section FindSection(const char* name) {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  auto* sec = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    char n[9] = {};
    memcpy(n, sec->Name, 8);
    if (strcmp(n, name) == 0) return {base + sec->VirtualAddress, sec->Misc.VirtualSize};
  }
  return {};
}

// Every 8-byte-aligned slot in the first `limit` bytes of the section that holds `value`.
std::vector<void**> FindSlots(const Section& s, void* value, size_t limit) {
  std::vector<void**> out;
  size_t n = std::min(s.size, limit) / 8;
  auto** p = reinterpret_cast<void**>(s.start);
  for (size_t i = 0; i < n; ++i)
    if (p[i] == value) out.push_back(p + i);
  return out;
}

bool Patch(void** slot, void* value) {
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  InterlockedExchangePointer(slot, value);
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}

HWND FindGameWindow() {
  const DWORD pid = GetCurrentProcessId();
  HWND w = nullptr;
  while ((w = FindWindowExW(nullptr, w, g_cfg.window_class.c_str(), nullptr)) != nullptr) {
    DWORD owner = 0;
    GetWindowThreadProcessId(w, &owner);
    if (owner == pid) return w;
  }
  return nullptr;
}

// background_run=1: an unfocused copy keeps running. The game pauses itself when its
// window loses focus (seen 2026-10-21: the player's position and rotation read 0 while
// paused, and its log stops), so the window's focus-loss messages are swallowed. In the
// world it also ignores the pad unless its window is in front, so its GetForegroundWindow
// and GetActiveWindow return its own window. A copy that believes it is active must not
// grab the mouse, so its ClipCursor and SetCursorPos do nothing unless its window really
// is in front (checked with this module's own, unpatched GetForegroundWindow).
WNDPROC g_orig_proc = nullptr;
HWND g_window = nullptr;
volatile LONG g_swallowed = 0, g_cursor_blocked = 0;
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
ClipCursor_t g_real_clip = nullptr;
SetCursorPos_t g_real_setpos = nullptr;

LRESULT CALLBACK BackgroundProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  if ((m == WM_ACTIVATEAPP && !wp) || (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || m == WM_KILLFOCUS) {
    if (InterlockedIncrement(&g_swallowed) <= 20) SowLog(L"input: kept running: focus-loss message 0x%X swallowed", m);
    return 0;
  }
  return CallWindowProcW(g_orig_proc, w, m, wp, lp);
}

BOOL WINAPI GuardClipCursor(const RECT* r) {
  if (r && GetForegroundWindow() != g_window) {
    if (InterlockedIncrement(&g_cursor_blocked) == 1) SowLog(L"input: ClipCursor from a window in the background ignored");
    return TRUE;
  }
  return g_real_clip(r);
}

HWND WINAPI OwnWindow() { return g_window; }

BOOL WINAPI GuardSetCursorPos(int x, int y) {
  if (GetForegroundWindow() != g_window) {
    if (InterlockedIncrement(&g_cursor_blocked) == 1) SowLog(L"input: SetCursorPos from a window in the background ignored");
    return TRUE;
  }
  return g_real_setpos(x, y);
}

void InstallBackgroundRun(HWND w, const Section& sec) {
#ifndef SOWGUARD_MUTANT  // negative control: focus loss reaches the game
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  g_window = w;
  g_real_clip = reinterpret_cast<ClipCursor_t>(GetProcAddress(user32, "ClipCursor"));
  g_real_setpos = reinterpret_cast<SetCursorPos_t>(GetProcAddress(user32, "SetCursorPos"));
  size_t clip = 0, setpos = 0;
  for (void** s : FindSlots(sec, reinterpret_cast<void*>(g_real_clip), g_cfg.iat_scan_bytes))
    clip += Patch(s, reinterpret_cast<void*>(&GuardClipCursor));
  for (void** s : FindSlots(sec, reinterpret_cast<void*>(g_real_setpos), g_cfg.iat_scan_bytes))
    setpos += Patch(s, reinterpret_cast<void*>(&GuardSetCursorPos));
  size_t focus = 0;
  for (const char* name : {"GetForegroundWindow", "GetActiveWindow"})
    for (void** s : FindSlots(sec, reinterpret_cast<void*>(GetProcAddress(user32, name)), g_cfg.iat_scan_bytes))
      focus += Patch(s, reinterpret_cast<void*>(&OwnWindow));
  g_orig_proc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&BackgroundProc)));
  SowLog(L"input: background run on (window procedure %s; ClipCursor slots %zu, SetCursorPos slots %zu, "
         L"foreground/active window slots %zu)",
         g_orig_proc ? L"wrapped" : L"NOT wrapped", clip, setpos, focus);
#else
  (void)w;
  (void)sec;
#endif
}

void SetButtons(LONG buttons) {
  InterlockedExchange(&g_buttons, buttons);
  InterlockedIncrement(&g_packet);
}

DWORD WINAPI InputThread(void*) {
  const ULONGLONG t0 = GetTickCount64();
  HMODULE xi = GetModuleHandleW(L"XINPUT9_1_0.dll");
  while (!xi && GetTickCount64() - t0 < 120000) {
    Sleep(50);
    xi = GetModuleHandleW(L"XINPUT9_1_0.dll");
  }
  if (!xi) {
    SowLog(L"input: XINPUT9_1_0.dll never loaded; virtual pad off");
    return 0;
  }
  g_real = reinterpret_cast<XInputGetState_t>(GetProcAddress(xi, "XInputGetState"));
  Section sec = FindSection(g_cfg.iat_section.c_str());
  if (!g_real || !sec.start) {
    SowLog(L"input: no XInputGetState or no %S section; virtual pad off", g_cfg.iat_section.c_str());
    return 0;
  }
  // SteamStub fills the import table after loading the DLLs; wait for the slot.
  std::vector<void**> slots = FindSlots(sec, reinterpret_cast<void*>(g_real), g_cfg.iat_scan_bytes);
  while (slots.empty() && GetTickCount64() - t0 < 120000) {
    Sleep(20);
    slots = FindSlots(sec, reinterpret_cast<void*>(g_real), g_cfg.iat_scan_bytes);
  }
  if (slots.empty()) {
    SowLog(L"input: XInputGetState not found in the first %u bytes of %S; virtual pad off", g_cfg.iat_scan_bytes,
           g_cfg.iat_section.c_str());
    return 0;
  }
#ifndef SOWGUARD_MUTANT  // negative control: the pad is never wired in
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  for (void** s : slots) {
    if (!Patch(s, reinterpret_cast<void*>(&PadGetState))) {
      SowLog(L"input: cannot patch the XInputGetState slot at rva:%llX (%lu); virtual pad off",
             (unsigned long long)(reinterpret_cast<uintptr_t>(s) - base), GetLastError());
      return 0;
    }
    SowLog(L"input: XInputGetState slot rva:%llX now reads the virtual pad (%llu ms after start)",
           (unsigned long long)(reinterpret_cast<uintptr_t>(s) - base), GetTickCount64() - t0);
  }
  InterlockedExchange(&g_installed, 1);
#endif

  // SteamStub is done with the image by now (it filled the import table), so the game's
  // vtables can be patched.
  if (g_cfg.sim_vtable_rva) {
    std::string err;
    if (!SimHookInstall(g_cfg.sim_vtable_rva, g_cfg.sim_slot, &err)) SowLog(L"input: sim_hook not installed: %S", err.c_str());
  }

  HWND w = FindGameWindow();
  while (!w && GetTickCount64() - t0 < 300000) {
    Sleep(100);
    w = FindGameWindow();
  }
  if (!w) {
    SowLog(L"input: no %s window after 300 s; no presses", g_cfg.window_class.c_str());
    return 0;
  }
  if (g_cfg.background_run) InstallBackgroundRun(w, sec);
  SowLog(L"input: game window up at %llu ms; first A press in %u ms, then every %u ms, at most %u",
         GetTickCount64() - t0, g_cfg.first_ms, g_cfg.every_ms, g_cfg.max_presses);
  DWORD wait = g_cfg.first_ms;
  for (unsigned n = 1; n <= g_cfg.max_presses; ++n) {
    if (WaitForSingleObject(g_loaded, wait) == WAIT_OBJECT_0) break;
    SetButtons(XINPUT_GAMEPAD_A);
    Sleep(g_cfg.hold_ms);
    SetButtons(0);
    SowLog(L"input: pressed A (%u of %u) at %llu ms", n, g_cfg.max_presses, GetTickCount64() - t0);
    wait = g_cfg.every_ms;
  }
  if (WaitForSingleObject(g_loaded, g_cfg.every_ms) == WAIT_OBJECT_0)
    SowLog(L"input: the game reports the world starting at %llu ms; pressing stopped", GetTickCount64() - t0);
  else
    SowLog(L"input: no world start reported after %u presses; gave up", g_cfg.max_presses);
  return 0;
}

ULONGLONG g_t0 = 0;

}  // namespace

void StartInput(const InputConfig& cfg) {
  g_cfg = cfg;
  g_t0 = GetTickCount64();
  InitializeCriticalSection(&g_hold_lock);
  g_loaded = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE t = CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  else SowLog(L"input: cannot start the virtual-pad thread (%lu)", GetLastError());
}

void InputOnDebugLine(const char* category, const char* msg) {
  if (!g_loaded || WaitForSingleObject(g_loaded, 0) == WAIT_OBJECT_0) return;
  if (strncmp(category, "[GAMEMODE|SESSION]", 18) == 0 && strncmp(msg, "Load:", 5) == 0)
    SowLog(L"input: the game read the campaign's save at %llu ms (still pressing)", GetTickCount64() - g_t0);
  if (strncmp(category, "[QUESTMGR]", 10) == 0 && strncmp(msg, "GameModeSet - ", 14) == 0) {
    g_game_thread = GetCurrentThreadId();
    SowLog(L"input: world start logged on thread %lu (the simulation thread)", g_game_thread);
    SetEvent(g_loaded);
  }
}

bool InputHold(unsigned buttons, int lx, int ly, int rx, int ry, unsigned ms, std::string* err) {
  if (!g_installed) {
    *err = "virtual pad not installed (autoload=1)";
    return false;
  }
  auto stick = [](int v) { return (SHORT)std::max(-32768, std::min(32767, v)); };
  EnterCriticalSection(&g_hold_lock);
  g_hold.buttons = (WORD)buttons;
  g_hold.lt = (BYTE)((buttons >> 16) & 0xFF);
  g_hold.rt = (BYTE)((buttons >> 24) & 0xFF);
  g_hold.lx = stick(lx);
  g_hold.ly = stick(ly);
  g_hold.rx = stick(rx);
  g_hold.ry = stick(ry);
  g_hold.until = GetTickCount64() + std::min(ms, 60000u);
  g_hold.ended = false;
  LeaveCriticalSection(&g_hold_lock);
  InterlockedIncrement(&g_packet);
  SowLog(L"input: hold buttons %04X, left %d,%d, right %d,%d for %u ms", buttons, lx, ly, rx, ry, ms);
  return true;
}

// Player.Save in Steam build 3095001: if the player exists, ask it for a delayed save.
//   sub rsp,28h; mov rax,[rip+GameClient]; mov rcx,[rax+888h]; test rcx,rcx; je +5;
//   call <save>; add rsp,28h; ret
bool InputRequestSave(uintptr_t handler_rva, uintptr_t gameclient_rva, std::string* err) {
  if (!g_installed) {
    *err = "virtual pad not installed (autoload=1)";
    return false;
  }
  if (!g_game_thread) {
    *err = "world not started yet (no GameModeSet seen)";
    return false;
  }
  static const uint8_t kHead[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05};
  static const uint8_t kMid[] = {0x48, 0x8B, 0x88, 0x88, 0x08, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x05, 0xE8};
  static const uint8_t kTail[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};
  const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const uint8_t* f = base + handler_rva;
  int32_t disp = 0;
  memcpy(&disp, f + 7, 4);
  if (memcmp(f, kHead, sizeof kHead) || memcmp(f + 11, kMid, sizeof kMid) || memcmp(f + 28, kTail, sizeof kTail) ||
      f + 11 + disp != base + gameclient_rva) {
    *err = "Player.Save handler bytes differ from the known build; refused";
    return false;
  }
  g_save_handler = reinterpret_cast<Handler_t>(const_cast<uint8_t*>(f));
  InterlockedExchange(&g_save_pending, 1);
  SowLog(L"input: Player.Save queued for the next pad read");
  return true;
}

DWORD InputGameThread() { return g_game_thread; }
