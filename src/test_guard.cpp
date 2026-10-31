// Offline tests for sowguard (the bink2w64.dll proxy). No game runs.
//
// Never touches the owner's real folders: each case builds a fake owner tree (fake
// LocalAppData, Documents, Steam, game install) inside its own case directory, loads
// the proxy DLL into a child copy of this exe with test_mode=1, and checks what
// happened on disk and how the child exited.
//
//   test_guard.exe run    <test root> <proxy dll> <original bink2w64.dll> <exe to hash> [case]
//                         (with a case name, only that case runs)
//   test_guard.exe mutant <test root> <mutant proxy dll> <original bink2w64.dll>
//   test_guard.exe case   <name> <case dir> [arg]          (child, started by run)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <winhttp.h>
#include <winternl.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "xinput9_1_0.lib")  // like the game: XInputGetState from XINPUT9_1_0.dll
#pragma comment(lib, "user32.lib")

#ifndef FILE_OVERWRITE_IF
#define FILE_OVERWRITE_IF 0x00000005
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif

namespace {

constexpr DWORD kExitConfig = 0xC0DE0001;
constexpr DWORD kExitOutsideRoot = 0xC0DE0002;
constexpr DWORD kExitExeIdentity = 0xC0DE0003;
constexpr DWORD kExitSteamAppId = 0xC0DE0004;
constexpr DWORD kExitProtected = 0xC0DE0010;
constexpr DWORD kExitSteamGuard = 0xC0DE0006;
constexpr const char* kGameExeSha256 = "ed0d95cabb033cd66aaa99d699374881846c3cb03ec345f1123d82d45e0bdaef";

// ---------------------------------------------------------------- file helpers

bool WriteText(const std::wstring& path, const std::string& text) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    wprintf(L"    write %s failed (%lu)\n", path.c_str(), GetLastError());
    return false;
  }
  DWORD w = 0;
  BOOL ok = WriteFile(f, text.data(), (DWORD)text.size(), &w, nullptr);
  CloseHandle(f);
  return ok && w == text.size();
}

bool ReadText(const std::wstring& path, std::string* out) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  char buf[4096];
  DWORD got = 0;
  BOOL ok = ReadFile(f, buf, sizeof buf, &got, nullptr);
  CloseHandle(f);
  if (!ok) return false;
  out->assign(buf, got);
  return true;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool FileIs(const std::wstring& path, const std::string& want) {
  std::string got;
  return ReadText(path, &got) && got == want;
}

void MakeDirs(const std::wstring& p) {
  for (size_t i = 3; i <= p.size(); ++i)
    if (i == p.size() || p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
}

void RemoveTree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring n = fd.cFileName;
      if (n == L"." || n == L"..") continue;
      std::wstring p = dir + L"\\" + n;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
      else DeleteFileW(p.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(dir.c_str());
}

// relative path -> content ("<dir>" for directories)
void Snapshot(const std::wstring& dir, const std::wstring& rel, std::map<std::wstring, std::string>* out) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::wstring n = fd.cFileName;
    if (n == L"." || n == L"..") continue;
    std::wstring p = dir + L"\\" + n, r = rel + L"\\" + n;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      (*out)[r] = "<dir>";
      Snapshot(p, r, out);
    } else {
      std::string s;
      ReadText(p, &s);
      (*out)[r] = s;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

std::wstring Self() {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
  return std::wstring(buf, n);
}

// ---------------------------------------------------------------- child cases

struct Layout {
  std::wstring dir, sow, install, remote, local_wb;
  explicit Layout(const std::wstring& d)
      : dir(d),
        sow(d + L"\\owner\\Local\\WB Games\\Shadow of War"),
        install(d + L"\\owner\\Steam\\steamapps\\common\\ShadowOfWar\\x64"),
        remote(d + L"\\owner\\Steam\\userdata\\123\\356190\\remote"),
        local_wb(d + L"\\owner\\Local\\WB Games") {}
  std::wstring Profile(const std::wstring& rel) const { return dir + L"\\profile\\" + rel; }
  std::wstring PSow(const std::wstring& file) const { return Profile(L"LocalAppData\\WB Games\\Shadow of War\\" + file); }
};

bool Check(bool ok, const char* what) {
  printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
  return ok;
}

using NtCreateFile_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                         ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

bool NtWriteRelative(HANDLE dir, const std::wstring& name, const std::string& text) {
  auto nt_create = reinterpret_cast<NtCreateFile_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  UNICODE_STRING us;
  us.Buffer = const_cast<PWSTR>(name.c_str());
  us.Length = us.MaximumLength = (USHORT)(name.size() * sizeof(wchar_t));
  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, dir, nullptr);
  IO_STATUS_BLOCK io{};
  HANDLE f = nullptr;
  NTSTATUS st = nt_create(&f, FILE_GENERIC_WRITE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr, 0);
  if (st < 0) {
    printf("    NtCreateFile relative failed 0x%08lX\n", (unsigned long)st);
    return false;
  }
  DWORD w = 0;
  BOOL ok = WriteFile(f, text.data(), (DWORD)text.size(), &w, nullptr);
  CloseHandle(f);
  return ok != FALSE;
}

// ---------------------------------------------------------------- network helpers

// Addresses: loopback, and documentation ranges (192.0.2.0/24, 2001:db8::/32) that are
// never routed, so a probe never reaches a real host.
sockaddr_storage Addr(const char* ip, unsigned short port) {
  sockaddr_storage ss{};
  if (strchr(ip, ':')) {
    auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    inet_pton(AF_INET6, ip, &a->sin6_addr);
  } else {
    auto* a = reinterpret_cast<sockaddr_in*>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    inet_pton(AF_INET, ip, &a->sin_addr);
  }
  return ss;
}

int AddrLen(const sockaddr_storage& ss) { return ss.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in); }

// Non-blocking connect; returns the WSA error seen within 1.5 s (0 = connected,
// WSAETIMEDOUT = still pending).
int TryConnect(const char* ip, unsigned short port) {
  sockaddr_storage ss = Addr(ip, port);
  SOCKET s = socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return WSAGetLastError();
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
  int err = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&ss), AddrLen(ss)) != 0) {
    err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      fd_set w, e;
      FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
      timeval tv{1, 500000};
      int r = select(0, nullptr, &w, &e, &tv);
      if (r == 0) err = WSAETIMEDOUT;
      else {
        int so = 0, len = sizeof so;
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
        err = so;
      }
    }
  }
  closesocket(s);
  return err;
}

// ConnectEx (overlapped, as WinHTTP and many engines use); same result convention.
int TryConnectEx(const char* ip, unsigned short port) {
  sockaddr_storage ss = Addr(ip, port);
  SOCKET s = socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return WSAGetLastError();
  sockaddr_storage any{};
  any.ss_family = ss.ss_family;
  bind(s, reinterpret_cast<sockaddr*>(&any), AddrLen(ss));
  GUID guid = WSAID_CONNECTEX;
  LPFN_CONNECTEX connect_ex = nullptr;
  DWORD bytes = 0;
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof guid, &connect_ex, sizeof connect_ex, &bytes, nullptr, nullptr);
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  int err = 0;
  if (!connect_ex(s, reinterpret_cast<sockaddr*>(&ss), AddrLen(ss), nullptr, 0, nullptr, &ov)) {
    err = WSAGetLastError();
    if (err == ERROR_IO_PENDING) {
      if (WaitForSingleObject(ov.hEvent, 1500) == WAIT_TIMEOUT) {
        err = WSAETIMEDOUT;
        CancelIoEx(reinterpret_cast<HANDLE>(s), &ov);
      } else {
        DWORD flags = 0;
        err = WSAGetOverlappedResult(s, &ov, &bytes, FALSE, &flags) ? 0 : WSAGetLastError();
      }
    }
  }
  closesocket(s);
  CloseHandle(ov.hEvent);
  return err;
}

int TrySendTo(const char* ip, unsigned short port) {
  sockaddr_storage ss = Addr(ip, port);
  SOCKET s = socket(ss.ss_family, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return WSAGetLastError();
  int r = sendto(s, "x", 1, 0, reinterpret_cast<sockaddr*>(&ss), AddrLen(ss));
  int err = r == SOCKET_ERROR ? WSAGetLastError() : 0;
  closesocket(s);
  return err;
}

// connect() on a UDP socket, then send(): the address travels with the connect.
int TryUdpConnectSend(const char* ip, unsigned short port) {
  sockaddr_storage ss = Addr(ip, port);
  SOCKET s = socket(ss.ss_family, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return WSAGetLastError();
  int err = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&ss), AddrLen(ss)) != 0) err = WSAGetLastError();
  else if (send(s, "x", 1, 0) == SOCKET_ERROR) err = WSAGetLastError();
  closesocket(s);
  return err;
}

// A listening TCP socket on loopback; returns its port (0 on failure).
unsigned short Listen(const char* ip, SOCKET* out) {
  sockaddr_storage ss = Addr(ip, 0);
  SOCKET s = socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return 0;
  int len = AddrLen(ss);
  if (bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0 || listen(s, 4) != 0 ||
      getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    closesocket(s);
    return 0;
  }
  *out = s;
  return ntohs(ss.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                                        : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
}

// UDP datagram to ourselves over loopback; true when it arrives.
bool UdpLoopbackEcho() {
  sockaddr_storage ss = Addr("127.0.0.1", 0);
  SOCKET rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int len = AddrLen(ss);
  bool ok = bind(rx, reinterpret_cast<sockaddr*>(&ss), len) == 0 &&
            getsockname(rx, reinterpret_cast<sockaddr*>(&ss), &len) == 0 &&
            sendto(tx, "ping", 4, 0, reinterpret_cast<sockaddr*>(&ss), len) == 4;
  if (ok) {
    DWORD timeout = 1500;
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof timeout);
    char buf[8] = {};
    ok = recv(rx, buf, sizeof buf, 0) == 4 && memcmp(buf, "ping", 4) == 0;
  }
  closesocket(rx);
  closesocket(tx);
  return ok;
}

// WinHTTP request to an IP literal (no DNS); returns the Win32 error (0 = response).
DWORD WinHttpTry(const wchar_t* host, DWORD* ms) {
  ULONGLONG t0 = GetTickCount64();
  DWORD err = 0;
  HINTERNET s = WinHttpOpen(L"sowguard-test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
  HINTERNET c = s ? WinHttpConnect(s, host, 80, 0) : nullptr;
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/", nullptr, nullptr, nullptr, 0) : nullptr;
  if (r) WinHttpSetTimeouts(r, 3000, 3000, 3000, 3000);
  if (!r || !WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(r, nullptr)) err = GetLastError();
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  *ms = (DWORD)(GetTickCount64() - t0);
  return err;
}

// Probe fixtures: a marker to scan for, a rip-relative "lea rax, [g_rip_target]" built at
// runtime, and a pointer to a known value. The child reports their RVAs.
const char kProbeMarker[] = "SOWPROBE-MARK-7f3a";
volatile uint64_t g_rip_target = 0x1122334455667788ull;
uint8_t g_rip_insn[8];
volatile uint64_t* volatile g_ptr = &g_rip_target;

std::string MarkerPattern() {
  std::string p;
  char b[4];
  for (size_t i = 0; i + 1 < sizeof kProbeMarker; ++i) {
    snprintf(b, sizeof b, "%s%02X", i ? " " : "", (unsigned char)kProbeMarker[i]);
    p += b;
  }
  return p;
}

int ProbeChild(const Layout& L) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  auto insn = reinterpret_cast<uintptr_t>(g_rip_insn), target = reinterpret_cast<uintptr_t>(&g_rip_target);
  int32_t disp = (int32_t)(target - (insn + 7));
  g_rip_insn[0] = 0x48;
  g_rip_insn[1] = 0x8D;
  g_rip_insn[2] = 0x05;
  memcpy(g_rip_insn + 3, &disp, 4);
  // A heap block holding a unique value twice, for "findq".
  auto* heap = static_cast<uint64_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  heap[32] = heap[100] = 0x5A5A1234ABCD0001ull;
  char info[256];
  snprintf(info, sizeof info, "marker=%llX insn=%llX target=%llX ptr=%llX heap=%llX\n",
           (unsigned long long)(reinterpret_cast<uintptr_t>(kProbeMarker) - base), (unsigned long long)(insn - base),
           (unsigned long long)(target - base),
           (unsigned long long)(reinterpret_cast<uintptr_t>(&g_ptr) - base),
           (unsigned long long)reinterpret_cast<uintptr_t>(heap));
  WriteText(L.dir + L"\\probe_info.txt", info);
  // Stay alive until the parent is done with the pipe.
  for (int i = 0; i < 600 && !Exists(L.dir + L"\\stop"); ++i) Sleep(50);
  return g_rip_target == 0x1122334455667788ull ? 0 : 1;
}

// Debug-print fixtures: the game's 12-byte stub, a "next function" sentinel that must
// survive the patch, and a call site (E8 to the stub, then unique bytes) to find it by.
alignas(16) uint8_t g_dp_area[64] = {
    0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0xC3, 0xCC,  // stub
    0x90, 0x90, 0xC3, 0xCC,                                                  // next function
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0xE8, 0xDB, 0xFF, 0xFF, 0xFF, 0xD3, 0xB6, 0x57, 0x0B, 0x5E, 0x01, 0x7A,  // call -37 (to 0)
};
const char* kDpCallSitePattern = "E8 ? ? ? ? D3 B6 57 0B 5E 01 7A";
volatile uint64_t g_write_target = 0;
volatile uint8_t g_channels[4] = {0, 0, 0, 0};  // debug_channels lists [0] and [2]

bool WaitFile(const std::wstring& path, int ms);

// Calls `target` (the debug-print stub, hooked or not) from generated code with known
// values in rax, rcx (category), rdx (format), r8-r11 and xmm0-5, and sentinels in the
// caller's first two home slots, then records them all after the call. The original
// stub changes none of them; neither may the hook (game callers rely on it).
//   thunk(in, out, target): in/out = rax rcx rdx r8 r9 r10 r11 xmm0..5 (13 qwords),
//   out[13], out[14] = the two home-slot sentinels after the call.
std::string CheckRegisters(void* target) {
  static const uint8_t kThunk[] = {
      0x53, 0x56, 0x57, 0x48, 0x89, 0xD3, 0x4C, 0x89, 0xC6, 0x48, 0x89, 0xCF, 0x48, 0x83, 0xEC, 0x20,
      0x48, 0xC7, 0x04, 0x24, 0x5A, 0x5A, 0x5A, 0x5A,              // mov qword [rsp], 5A5A5A5Ah
      0x48, 0xC7, 0x44, 0x24, 0x08, 0xA5, 0xA5, 0xA5, 0x25,        // mov qword [rsp+8], 25A5A5A5h
      0x48, 0x8B, 0x07, 0x48, 0x8B, 0x4F, 0x08, 0x48, 0x8B, 0x57, 0x10, 0x4C, 0x8B, 0x47, 0x18,
      0x4C, 0x8B, 0x4F, 0x20, 0x4C, 0x8B, 0x57, 0x28, 0x4C, 0x8B, 0x5F, 0x30,
      0xF3, 0x0F, 0x7E, 0x47, 0x38, 0xF3, 0x0F, 0x7E, 0x4F, 0x40, 0xF3, 0x0F, 0x7E, 0x57, 0x48,
      0xF3, 0x0F, 0x7E, 0x5F, 0x50, 0xF3, 0x0F, 0x7E, 0x67, 0x58, 0xF3, 0x0F, 0x7E, 0x6F, 0x60,
      0xFF, 0xD6,                                                  // call rsi (target)
      0x48, 0x89, 0x03, 0x48, 0x89, 0x4B, 0x08, 0x48, 0x89, 0x53, 0x10, 0x4C, 0x89, 0x43, 0x18,
      0x4C, 0x89, 0x4B, 0x20, 0x4C, 0x89, 0x53, 0x28, 0x4C, 0x89, 0x5B, 0x30,
      0x66, 0x0F, 0xD6, 0x43, 0x38, 0x66, 0x0F, 0xD6, 0x4B, 0x40, 0x66, 0x0F, 0xD6, 0x53, 0x48,
      0x66, 0x0F, 0xD6, 0x5B, 0x50, 0x66, 0x0F, 0xD6, 0x63, 0x58, 0x66, 0x0F, 0xD6, 0x6B, 0x60,
      0x48, 0x8B, 0x04, 0x24, 0x48, 0x89, 0x43, 0x68, 0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x89, 0x43, 0x70,
      0x48, 0x83, 0xC4, 0x20, 0x5F, 0x5E, 0x5B, 0xC3};
  auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  memcpy(code, kThunk, sizeof kThunk);
  FlushInstructionCache(GetCurrentProcess(), code, sizeof kThunk);
  static const char* kCategory = "[TEST]: ";
  static const char* kFormat = "regs r8=%llx r9=%llx";
  const char* names[] = {"rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5"};
  uint64_t in[13] = {0x1111111111111111ull, (uint64_t)(uintptr_t)kCategory, (uint64_t)(uintptr_t)kFormat,
                     0x8888888888888888ull, 0x9999999999999999ull, 0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull};
  for (int i = 0; i < 6; ++i) in[7 + i] = 0xC0C0C0C0C0C0C000ull + i;
  uint64_t out[15] = {};
  using Thunk = void (*)(const uint64_t*, uint64_t*, void*);
  reinterpret_cast<Thunk>(static_cast<void*>(code))(in, out, target);
  VirtualFree(code, 0, MEM_RELEASE);
  std::string bad;
  for (int i = 0; i < 13; ++i)
    if (out[i] != in[i]) bad += std::string(" ") + names[i];
  if (out[13] != 0x5A5A5A5Aull) bad += " home-slot-1";
  if (out[14] != 0x25A5A5A5ull) bad += " home-slot-2";
  return bad.empty() ? "regs ok" : "changed:" + bad;
}

int HookChild(const Layout& L, const std::wstring& name) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  char info[128];
  snprintf(info, sizeof info, "area=%llX write=%llX\n",
           (unsigned long long)(reinterpret_cast<uintptr_t>(g_dp_area) - base),
           (unsigned long long)(reinterpret_cast<uintptr_t>(&g_write_target) - base));
  WriteText(L.dir + L"\\probe_info.txt", info);
  if (!WaitFile(L.dir + L"\\go", 15000)) return 3;
  using DebugPrint = void (*)(const char*, const char*, ...);
  auto dp = reinterpret_cast<DebugPrint>(static_cast<void*>(g_dp_area));
  int count = 0;
  dp("[TEST]: ", "hello %d %s", 42, "world");
  dp("[TEST]: ", "float %.2f and %d", 1.5, 7);
  dp("[TEST]: ", "bad pointer %s", reinterpret_cast<const char*>(0x10));
  dp("[TEST]: ", "count %n", &count);
  dp("[TEST]: ", "many %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6);
  std::vector<HANDLE> threads;
  for (int t = 0; t < 4; ++t)
    threads.push_back(CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
          auto f = reinterpret_cast<DebugPrint>(static_cast<void*>(g_dp_area));
          for (int i = 0; i < 250; ++i) f("[LOOP]: ", "thread %d i %d", (int)(uintptr_t)p, i);
          return 0;
        },
        reinterpret_cast<void*>((uintptr_t)t), 0, nullptr));
  WaitForMultipleObjects((DWORD)threads.size(), threads.data(), TRUE, 30000);
  for (HANDLE h : threads) CloseHandle(h);
  WriteText(L.dir + L"\\regs.txt", CheckRegisters(g_dp_area));
  // Channels are set only once the hook is in.
  const uint8_t want_on = name == L"probe_hook" ? 1 : 0;
  const bool chan_ok = g_channels[0] == want_on && g_channels[1] == 0 && g_channels[2] == want_on && g_channels[3] == 0;
  WriteText(L.dir + L"\\chan.txt", chan_ok ? "chan ok" : "chan wrong");
  WriteText(L.dir + L"\\called", "");
  for (int i = 0; i < 600 && !Exists(L.dir + L"\\stop"); ++i) Sleep(50);
  if (name == L"probe_hook") return g_write_target == 0x4847464544434241ull ? 0 : 4;
  return 0;
}

// Steam guard (remote storage and user stats), against the fake steam_api64.dll (loaded
// before the guard).
int SteamChild(const Layout& L, HMODULE fake) {
  using SetReady = void (*)(int);
  using Count = long (*)(int);
  using Rs = void* (*)();
  auto set_ready = reinterpret_cast<SetReady>(GetProcAddress(fake, "FakeSteam_SetReady"));
  auto count = reinterpret_cast<Count>(GetProcAddress(fake, "FakeSteam_Count"));
  auto rs_fn = reinterpret_cast<Rs>(GetProcAddress(fake, "FakeSteam_RemoteStorage"));
  auto us_count = reinterpret_cast<Count>(GetProcAddress(fake, "FakeSteam_UserStatsCount"));
  auto us_fn = reinterpret_cast<Rs>(GetProcAddress(fake, "FakeSteam_UserStats"));
  Sleep(300);  // the guard has to wait for Steam to come up
  set_ready(1);
  std::string log;
  for (int i = 0; i < 100 && log.find("user-stats writes blocked") == std::string::npos; ++i) {
    Sleep(50);
    ReadText(L.Profile(L"sowguard.log"), &log);
  }
  using Fn = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
  auto call_on = [](void* obj, int slot, uint64_t a, uint64_t b, uint64_t c) {
    return reinterpret_cast<Fn>((*static_cast<void* const* const*>(obj))[slot])(obj, a, b, c);
  };
  void* rs = rs_fn();
  auto call = [&](int slot, uint64_t a, uint64_t b, uint64_t c) { return call_on(rs, slot, a, b, c); };
  const char* save = "save.sav";
  const uint64_t name = (uint64_t)(uintptr_t)save, data = (uint64_t)(uintptr_t)"hello";
  struct Want {
    int slot;
    const char* what;
    uint64_t a, b, c;
    int kind;  // 0: bool true, 1: bool false, 2: u64 zero, 3: u64 ~0, 4: void
  } wants[] = {
      {0, "FileWrite", name, data, 5, 0},         {2, "FileWriteAsync", name, data, 5, 2},
      {5, "FileForget", name, 0, 0, 0},           {6, "FileDelete", name, 0, 0, 0},
      {7, "FileShare", name, 0, 0, 2},            {8, "SetSyncPlatforms", name, 1, 0, 0},
      {9, "FileWriteStreamOpen", name, 0, 0, 3},  {10, "FileWriteStreamWriteChunk", 42, data, 5, 1},
      {11, "FileWriteStreamClose", 42, 0, 0, 1},  {12, "FileWriteStreamCancel", 42, 0, 0, 1},
      {23, "SetCloudEnabledForApp", 0, 0, 0, 4},  {54, "UGCDownloadToLocation", 7, name, 0, 2},
  };
  std::string report;
  for (const Want& w : wants) {
    long before = count(w.slot);
    uint64_t r = call(w.slot, w.a, w.b, w.c);
    bool reached = count(w.slot) != before;
    bool value_ok = w.kind == 0 ? (uint8_t)r == 1 : w.kind == 1 ? (uint8_t)r == 0 : w.kind == 2 ? r == 0
                  : w.kind == 3 ? r == ~0ull : true;
    if (reached || !value_ok) report += std::string(" ") + w.what + (reached ? "(reached Steam)" : "(wrong value)");
  }
  long before = count(1);
  uint64_t read = call(1, name, data, 5);
  if (read != 7 || count(1) != before + 1) report += " FileRead(not passed through)";

  void* us = us_fn();
  const uint64_t ach = (uint64_t)(uintptr_t)"ACH_TEST";
  Want stat_wants[] = {
      {3, "SetStat(float)", ach, 0, 0, 0},       {4, "SetStat(int)", ach, 5, 0, 0},
      {5, "UpdateAvgRateStat", ach, 0, 0, 0},    {7, "SetAchievement", ach, 0, 0, 0},
      {8, "ClearAchievement", ach, 0, 0, 0},     {10, "StoreStats", 0, 0, 0, 0},
      {13, "IndicateAchievementProgress", ach, 1, 10, 0}, {21, "ResetAllStats", 1, 0, 0, 0},
      {31, "UploadLeaderboardScore", 7, 1, 100, 2}, {32, "AttachLeaderboardUGC", 7, 9, 0, 2},
  };
  for (const Want& w : stat_wants) {
    long before_us = us_count(w.slot);
    uint64_t r = call_on(us, w.slot, w.a, w.b, w.c);
    bool reached = us_count(w.slot) != before_us;
    bool value_ok = w.kind == 0 ? (uint8_t)r == 1 : w.kind == 2 ? r == 0 : true;
    if (reached || !value_ok) report += std::string(" ") + w.what + (reached ? "(reached Steam)" : "(wrong value)");
  }
  before = us_count(6);
  if (call_on(us, 6, ach, 0, 0) != 9 || us_count(6) != before + 1) report += " GetAchievement(not passed through)";
  // Like a game load: hundreds of SetStat calls, then an achievement that must still be logged.
  for (int i = 0; i < 400; ++i) call_on(us, 4, (uint64_t)(uintptr_t)"STAT_FLOOD", i, 0);
  call_on(us, 7, (uint64_t)(uintptr_t)"ACH_LATE", 0, 0);
  WriteText(L.dir + L"\\steam.txt", report.empty() ? "steam ok" : "steam wrong:" + report);
  return 0;
}

// steam_cloud=sandbox, against the fake steam_api64.dll: the game's cloud calls go to
// <profile>\steam-userdata\356190\remote and never to Steam.
int SteamCloudChild(const Layout& L, HMODULE fake) {
  using SetReady = void (*)(int);
  using Count = long (*)(int);
  using Rs = void* (*)();
  auto set_ready = reinterpret_cast<SetReady>(GetProcAddress(fake, "FakeSteam_SetReady"));
  auto count = reinterpret_cast<Count>(GetProcAddress(fake, "FakeSteam_Count"));
  void* rs = reinterpret_cast<Rs>(GetProcAddress(fake, "FakeSteam_RemoteStorage"))();
  Sleep(300);
  set_ready(1);
  std::string log;
  for (int i = 0; i < 100 && log.find("served from the sandbox cloud") == std::string::npos; ++i) {
    Sleep(50);
    ReadText(L.Profile(L"sowguard.log"), &log);
  }
  using Fn = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
  long reached = 0;
  auto call = [&](int slot, uint64_t a, uint64_t b, uint64_t c) {
    long before = count(slot);
    uint64_t r = reinterpret_cast<Fn>((*static_cast<void* const* const*>(rs))[slot])(rs, a, b, c);
    reached += count(slot) - before;
    return r;
  };
  auto s = [](const char* p) { return (uint64_t)(uintptr_t)p; };
  std::string bad;
  char buf[16] = {};
  if ((uint8_t)call(0, s("\\save.sav"), s("hello"), 5) != 1) bad += " FileWrite";
  if ((uint8_t)call(13, s("\\save.sav"), 0, 0) != 1) bad += " FileExists";
  if ((int32_t)call(15, s("\\save.sav"), 0, 0) != 5) bad += " GetFileSize";
  if ((int32_t)call(1, s("\\save.sav"), s(buf), sizeof buf) != 5 || memcmp(buf, "hello", 5) != 0) bad += " FileRead";
  if ((int64_t)call(16, s("\\save.sav"), 0, 0) < 1700000000) bad += " GetFileTimestamp";
  int32_t size = 0;
  if ((int32_t)call(18, 0, 0, 0) != 1) bad += " GetFileCount";
  const char* name = reinterpret_cast<const char*>(call(19, 0, s(reinterpret_cast<const char*>(&size)), 0));
  if (!name || strcmp(name, "save.sav") != 0 || size != 5) bad += " GetFileNameAndSize";
  if ((uint8_t)call(13, s("\\missing.sav"), 0, 0) != 0) bad += " FileExists(missing)";
  if ((int32_t)call(1, s("..\\escape.sav"), s(buf), sizeof buf) != 0) bad += " FileRead(..)";
  if ((uint8_t)call(0, s("..\\escape.sav"), s("x"), 1) != 0) bad += " FileWrite(..)";
  if ((uint8_t)call(0, s("C:\\escape.sav"), s("x"), 1) != 0) bad += " FileWrite(C:)";
  if (call(3, s("\\save.sav"), 0, 5) != 0) bad += " FileReadAsync";
  if ((uint8_t)call(6, s("\\save.sav"), 0, 0) != 1 || (uint8_t)call(13, s("\\save.sav"), 0, 0) != 0) bad += " FileDelete";
  if (reached) bad += " (" + std::to_string(reached) + " calls reached Steam)";
  WriteText(L.dir + L"\\cloud.txt", bad.empty() ? "cloud ok" : "cloud wrong:" + bad);
  return 0;
}

// "player", "pad" and "save" fixtures: a GameClient pointer, player and character laid out
// like the game's, and a Player.Save handler with the game's exact 33-byte shape whose
// call goes to SaveCounter(player).
alignas(16) uint8_t g_fake_player[0x2700];
alignas(16) uint8_t g_fake_character[0x500];
alignas(16) uint8_t g_fake_gameclient[0x900];
void* g_fake_gameclient_ptr = g_fake_gameclient;
volatile LONG g_saves = 0;
void* volatile g_saved_player = nullptr;
alignas(16) uint8_t g_save_handler[48];

__declspec(noinline) void SaveCounter(void* player) {
  g_saved_player = player;
  InterlockedIncrement(&g_saves);
}

void BuildPlayerFixtures() {
  const float pos[3] = {1.5f, -2.25f, 300.0f}, rot[4] = {0.0f, 0.7071f, 0.0f, 0.7071f}, health[3] = {500.0f, 250.0f, 0.5f};
  memcpy(g_fake_player + 0xAC, pos, sizeof pos);
  memcpy(g_fake_player + 0xB8, rot, sizeof rot);
  void* ch = g_fake_character;
  memcpy(g_fake_player + 0x26A8, &ch, 8);
  memcpy(g_fake_character + 0x428, health, sizeof health);
  void* p = g_fake_player;
  memcpy(g_fake_gameclient + 0x888, &p, 8);
  static const uint8_t code[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0x88, 0x88, 0x08, 0x00,
                                 0x00, 0x48, 0x85, 0xC9, 0x74, 0x05, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
  DWORD old = 0;
  VirtualProtect(g_save_handler, sizeof g_save_handler, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_save_handler, code, sizeof code);
  const uintptr_t f = reinterpret_cast<uintptr_t>(g_save_handler);
  const int32_t to_global = (int32_t)(reinterpret_cast<uintptr_t>(&g_fake_gameclient_ptr) - (f + 11));
  const int32_t to_counter = (int32_t)(reinterpret_cast<uintptr_t>(&SaveCounter) - (f + 28));
  memcpy(g_save_handler + 7, &to_global, 4);
  memcpy(g_save_handler + 24, &to_counter, 4);
  FlushInstructionCache(GetCurrentProcess(), g_save_handler, sizeof g_save_handler);
}

// The last-known-position effect stand-in: the game function's first 22 bytes (the
// rip-relative load reads GameClient+0x18), then a call to LkpRecorder(player, pos).
alignas(16) uint8_t g_lkp_fixture[64];
float g_lkp_seen[64][3];
volatile LONG g_lkp_calls = 0;
void* volatile g_lkp_player = nullptr;
volatile DWORD g_lkp_thread = 0;

__declspec(noinline) void LkpRecorder(void* player, const float* pos) {
  LONG i = InterlockedIncrement(&g_lkp_calls) - 1;
  if (i < 64) memcpy(g_lkp_seen[i], pos, sizeof g_lkp_seen[i]);
  g_lkp_player = player;
  g_lkp_thread = GetCurrentThreadId();
}

// coop_puppet_walk: stand-ins for the move request (3F4BD8's first 16 bytes, then a call
// to MoveRecorder), the AI tick's behavior-tree call (`mov rcx,[r8]; xor edx,edx; call
// <tree>`) and the tree (1919C8's first 10 bytes, then a jump to TreeRecorder). The
// stand-in AI gets a movement controller ([+0x2C78], whose +0 is the AI) and a tree
// ([+0x2D38], whose +8 is the AI); a second character's tree must keep running.
uint8_t* g_fake_ai = nullptr;  // the stand-in AI character (BuildPuppetFixtures)
alignas(16) uint8_t g_move_request_fixture[64];
alignas(16) uint8_t g_tick_fixture[32];
alignas(16) uint8_t g_tree_fixture[32];
alignas(16) uint8_t g_fake_controller[0x140];
alignas(16) uint8_t g_fake_trees[2][0x20];
alignas(16) uint8_t g_other_ai[0x100];
volatile LONG g_tree_calls[2] = {};
volatile LONG g_requests = 0, g_types_seen = 0, g_bad_requests = 0;
volatile LONG g_controller_ok = 1;
float g_max_lead = -1e9f;  // largest (request x - stand-in player x)
bool g_walk_case = false;

__declspec(noinline) void TreeRecorder(void* tree) {
  InterlockedIncrement(&g_tree_calls[tree == g_fake_trees[0] ? 0 : 1]);
}

__declspec(noinline) void MoveRecorder(void* controller, const uint8_t* req) {
  float dest[3], scale = 0;
  int32_t type = 0;
  memcpy(dest, req + 4, sizeof dest);
  memcpy(&type, req + 0x18, 4);
  memcpy(&scale, req + 0x20, 4);
  if (controller != g_fake_controller) g_controller_ok = 0;
  if (req[0] != 1 || req[0x24] != 0 || scale != 1.0f || type < 1 || type > 2) InterlockedIncrement(&g_bad_requests);
  if (type == 1 || type == 2) InterlockedOr(&g_types_seen, type);
  float x = 0;
  memcpy(&x, g_fake_player + 0xAC, sizeof x);
  g_max_lead = std::max(g_max_lead, dest[0] - x);
  InterlockedIncrement(&g_requests);
}

// coop_puppet_walk, animation inputs: a stand-in for the float animation-variable setter
// (176D78's first 20 bytes, then the frame undone and a jump to VarRecorder). The stand-in
// player sets three inputs and one other variable on its animation object (+0x120) every
// frame; the puppet (the stand-in AI) must get exactly the three, with the player's values.
alignas(16) uint8_t g_setter_fixture[64];
volatile LONG g_puppet_vars = 0, g_stray_vars = 0;
float g_seen_movemag = 0, g_seen_velocity = 0, g_seen_axis = 0;

__declspec(noinline) void __fastcall VarRecorder(void* anim, const char* name, float value) {
  if (anim != g_fake_ai + 0x120) return;
  InterlockedIncrement(&g_puppet_vars);
  if (strcmp(name, "RawMoveMag") == 0) g_seen_movemag = value;
  else if (strcmp(name, "PlayerVelocity") == 0) g_seen_velocity = value;
  else if (strcmp(name, "Game_MovementAxisX") == 0) g_seen_axis = value;
  else InterlockedIncrement(&g_stray_vars);
}

// coop_puppet_recv, root-motion speed: a stand-in for the AI movement component's per-frame
// step (191868's first 23 bytes, then the frame undone and a jump to StepRecorder), in the
// slot 0x80 of a movement component at the stand-in AI's +0x24A0 (whose +0x520 is the AI).
// Each frame the AI's own root motion is 200 units/s ([controller+0x9C] over [AI+0x1118]);
// StepRecorder sees what the step would apply.
alignas(16) uint8_t g_mover_step_fixture[64];
// The game's player-follower getter (1156AF0), only read for its bytes: the guard writes the
// byte it reads (+0x23B8) on the puppet.
alignas(16) const uint8_t g_follower_fixture[16] = {0x33, 0xC0, 0x38, 0x81, 0xB8, 0x23, 0x00, 0x00,
                                                    0x0F, 0x95, 0xC0, 0xC3};
// The game's attack target check (18626C): its prologue up to mov r14,[r8+20h], then an
// epilogue that accepts every candidate (al = 1). 4 bytes into an aligned qword, as in the game.
alignas(16) uint8_t g_target_check_buf[96];
uint8_t* const g_target_check_fixture = g_target_check_buf + 4;
// The function that applies every hit (2C70C0): its prologue, then an epilogue that returns 1
// (applied). On an aligned qword, as in the game.
alignas(16) uint8_t g_hit_buf[96];
uint8_t* const g_hit_fixture = g_hit_buf;
// The area-attack filter (2E5718): its first 18 bytes (two moves and the pushes), the pushes
// undone, then a jump to AoeRecorder. On an aligned qword, as in the game.
alignas(16) uint8_t g_aoe_fixture[64];
volatile LONG g_aoe_seen = 0;
const void* volatile g_aoe_last = nullptr;
// A full teleport's animation reset (FA7E10): its first 15 bytes (a move, a push, the frame, two
// more), undone, then a jump to ResetRecorder; rcx is a character's animation controller
// ([character+0x1970]). On an aligned qword, as in the game.
alignas(16) uint8_t g_reset_fixture[64];
volatile LONG g_reset_seen = 0;
alignas(16) uint8_t g_fake_anim_ctrl[2][0x40];  // the fake stand-in's ([g_fake_ai+0x1970]) and another
// The sender's attacks: the game's attack start (2D6B3C's 28 bytes, then a jmp to the start's
// head), the start (2E297C's 22 bytes, undone, then a jump to StartRecorder), the stop (2EA790's
// 23 bytes, undone, then a jump to StopRecorder), and a three-record attack table sorted by name
// hash, held by a global as the game's is ([27008F8]: count +0x28, array +0x38).
alignas(16) uint8_t g_attack_thunk_fixture[64];
alignas(16) uint8_t g_attack_start_fixture[64];
alignas(16) uint8_t g_attack_stop_fixture[64];
alignas(16) uint8_t g_fake_attacks[3 * 40];
alignas(16) uint8_t g_fake_attack_table[0x40];
void* g_fake_attack_table_ptr = g_fake_attack_table;
constexpr uint32_t kFakeAttackId = 0x221FBF97;  // the middle record's hash
volatile LONG g_attack_starts = 0, g_attack_stops = 0, g_attack_bad = 0;
alignas(16) uint8_t g_fake_mover[0x600];
void* g_fake_mover_vtable[1 + 81] = {};  // [0]: the type information slot before the table
// The movement component's frame update (slot 0x100, 2FFC1C's first 23 bytes, then the frame
// undone and a jump to UpdateRecorder), which puts the fake stand-in back on the ground (y
// -2.25) as the game's writes the transform from the movement's own state.
alignas(16) uint8_t g_mover_update_fixture[64];
volatile LONG g_step_calls = 0;
float g_step_speed = 0;

__declspec(noinline) void __fastcall StepRecorder(void* mover, void*) {
  if (mover != g_fake_mover) return;
  float d[3], dt = 0;
  memcpy(d, g_fake_controller + 0x9C, sizeof d);
  memcpy(&dt, g_fake_ai + 0x1118, sizeof dt);
  g_step_speed = std::sqrt(d[0] * d[0] + d[2] * d[2]) / dt;
  InterlockedIncrement(&g_step_calls);
}

// A start on the stand-in's health object with the middle record, no node, no target, scale 1;
// it keeps the record at +0xE0 as the game does. Anything else is counted as bad.
__declspec(noinline) void __fastcall StartRecorder(uint8_t* health, void* node, const uint8_t* record, void* target,
                                                   float scale, uint8_t, uint8_t, void*) {
  if (health != g_fake_ai + 0x4000 || node || target || scale != 1.0f || record != g_fake_attacks + 40) {
    InterlockedIncrement(&g_attack_bad);
    return;
  }
  memcpy(health + 0xE0, &record, 8);
  InterlockedIncrement(&g_attack_starts);
}

__declspec(noinline) void __fastcall StopRecorder(uint8_t* health, void* next) {
  if (health != g_fake_ai + 0x4000 || next) {
    InterlockedIncrement(&g_attack_bad);
    return;
  }
  memset(health + 0xE0, 0, 8);
  InterlockedIncrement(&g_attack_stops);
}

__declspec(noinline) void __fastcall AoeRecorder(void*, const void* character) {
  g_aoe_last = character;
  InterlockedIncrement(&g_aoe_seen);
}

__declspec(noinline) void __fastcall ResetRecorder(void*) { InterlockedIncrement(&g_reset_seen); }

__declspec(noinline) void __fastcall UpdateRecorder(void* mover, void*) {
  if (mover != g_fake_mover) return;
  const float ground = -2.25f;
  memcpy(g_fake_ai + 0xB0, &ground, sizeof ground);
}

void RunPuppetStep() {
  const float dt = 0.016f, d[3] = {0.0f, 0.0f, 200.0f * dt};
  memcpy(g_fake_ai + 0x1118, &dt, sizeof dt);
  memcpy(g_fake_controller + 0x9C, d, sizeof d);
  using Step = void(__fastcall*)(void*, void*);
  void** vt = nullptr;
  memcpy(&vt, g_fake_mover, sizeof vt);  // the guard may have given it a copy
  reinterpret_cast<Step>(vt[0x80 / 8])(g_fake_mover, g_fake_mover + 0xC8);
  reinterpret_cast<Step>(vt[0x100 / 8])(g_fake_mover, g_fake_mover + 0xC8);
}

void SetPlayerInputs() {
  using Set = void(__fastcall*)(void*, const char*, float);
  auto set = reinterpret_cast<Set>(reinterpret_cast<void*>(g_setter_fixture));
  void* anim = g_fake_player + 0x120;
  set(anim, "RawMoveMag", 0.75f);
  set(anim, "PlayerVelocity", 210.0f);
  set(anim, "Game_MovementAxisX", -0.5f);
  set(anim, "Player_Hand_L_X", 5.0f);  // not an input: must not reach the puppet
}

// A stand-in for the player's vtable: the "simulation thread" calls slots 1 and 2 every
// few ms; sim_hook goes on slot 1, the probe's "count" measures slot 2.
volatile LONG g_update_calls = 0;
__declspec(noinline) void __fastcall FakeUpdate(void*) { InterlockedIncrement(&g_update_calls); }
__declspec(noinline) void __fastcall FakeOther(void*) { InterlockedIncrement(&g_update_calls); }
void* g_fake_vtable[4] = {reinterpret_cast<void*>(&FakeOther), reinterpret_cast<void*>(&FakeUpdate),
                          reinterpret_cast<void*>(&FakeOther), reinterpret_cast<void*>(&FakeOther)};
volatile DWORD g_sim_thread = 0;
int g_walk_frames = 0;  // coop_puppet: frames the stand-in player has walked
bool g_walk_enabled = false;
volatile LONG g_sim_stop = 0;
const Layout* g_sim_layout = nullptr;
HMODULE g_sim_proxy = nullptr;

// coop_puppet_walk, once per stand-in frame: both characters' AI ticks run the tree call;
// after "move" the stand-in player walks at 100 units/s for 1.5 s, then 400 units/s for
// 1 s, then stops. Tree calls are counted while the puppet's AI is off (from "move" to the
// stop) and after "aion".
LONG g_puppet_tree_off = -1, g_other_tree_off = -1, g_puppet_tree_on = -1, g_teleports_at_move = -1;
LONG g_teleports_at_creep = -1;  // coop_puppet_recv: teleports when the sender starts to creep
LONG g_teleports_at_air = -1;    // coop_puppet_recv: teleports when the sender leaves the ground
// While the sender is in the air ("air" to "airend"): frames, movement steps at the start and
// end, and the highest the fake stand-in's own transform went.
LONG g_air_frames = 0, g_air_steps0 = -1, g_air_steps1 = -1;
float g_air_max_y = -1e9f;
int g_follower_at_free = -1;     // coop_puppet_recv: the puppet's follower byte before "free"/"dead"
// The target check's answers (1 accepted) for the puppet's health object and another one,
// every frame; for the puppet also just before "free"/"dead".
int g_target_standin = -1, g_target_other = -1, g_target_at_free = -1;
int g_hit_standin = -1, g_hit_other = -1, g_hit_at_free = -1;  // the same for a hit (1 applied)
int g_aoe_standin = -1, g_aoe_other = -1, g_aoe_at_free = -1;  // and for an area tick (1 evaluated)
int g_reset_standin = -1, g_reset_other = -1, g_reset_at_free = -1;  // and an animation reset (1 ran)
LONG TeleportCount();  // the stand-in teleport's calls (below)
LONG g_mark_move[2] = {}, g_mark_on = 0;
float g_walk_x0 = 0;
ULONGLONG g_walk_t0 = 0;
bool g_walk_stopped = false, g_aion_seen = false;

void WalkCaseFrame() {
  using Tick = void(__fastcall*)(void*, void*, void* const* tree_slot);
  auto tick = reinterpret_cast<Tick>(reinterpret_cast<void*>(g_tick_fixture));
  void* other_slot = g_fake_trees[1];
  tick(nullptr, nullptr, reinterpret_cast<void* const*>(g_fake_ai + 0x2D38));
  tick(nullptr, nullptr, &other_slot);
  if (!g_walk_t0 && Exists(g_sim_layout->dir + L"\\move")) {
    g_walk_t0 = GetTickCount64();
    memcpy(&g_walk_x0, g_fake_player + 0xAC, sizeof g_walk_x0);
    g_mark_move[0] = g_tree_calls[0];
    g_mark_move[1] = g_tree_calls[1];
    g_teleports_at_move = TeleportCount();
    // This copy's player starts an attack (its record at health+0xE0, the start time at +0x470).
    const uint8_t* rec = g_fake_attacks + 40;
    const uint32_t stamp = 5000;
    memcpy(g_fake_character + 0xE0, &rec, 8);
    memcpy(g_fake_character + 0x470, &stamp, 4);
  }
  if (g_teleports_at_creep < 0 && Exists(g_sim_layout->dir + L"\\creep")) g_teleports_at_creep = TeleportCount();
  if (g_teleports_at_air < 0 && Exists(g_sim_layout->dir + L"\\air")) {
    g_teleports_at_air = TeleportCount();
    g_air_steps0 = g_step_calls;
  }
  if (g_teleports_at_air >= 0 && g_air_steps1 < 0) {
    if (Exists(g_sim_layout->dir + L"\\airend")) {
      g_air_steps1 = g_step_calls;
    } else {
      ++g_air_frames;
    }
  }
  // "free": the game frees the puppet (its memory no longer holds the character class);
  // "dead": the puppet is killed (current health 0).
  const bool freed = Exists(g_sim_layout->dir + L"\\free"), dead = Exists(g_sim_layout->dir + L"\\dead");
  if (g_follower_at_free < 0 && (freed || dead)) {
    g_follower_at_free = g_fake_ai[0x23B8];
    g_target_at_free = g_target_standin;
    g_hit_at_free = g_hit_standin;
    g_aoe_at_free = g_aoe_standin;
    g_reset_at_free = g_reset_standin;
    if (freed) memset(g_fake_ai, 0, 8);
    if (dead) memset(g_fake_ai + 0x4000 + 0x42C, 0, 4);
  }
  if (g_walk_t0 && !g_walk_stopped) {
    const float t = (GetTickCount64() - g_walk_t0) / 1000.0f;
    const float d = t < 1.5f ? 100.0f * t : t < 2.5f ? 150.0f + 400.0f * (t - 1.5f) : 550.0f;
    const float x = g_walk_x0 + d;
    memcpy(g_fake_player + 0xAC, &x, sizeof x);
    if (t >= 2.5f) {
      g_walk_stopped = true;
      memset(g_fake_character + 0xE0, 0, 8);  // and its attack ends
      g_puppet_tree_off = g_tree_calls[0] - g_mark_move[0];
      g_other_tree_off = g_tree_calls[1] - g_mark_move[1];
    }
  }
  if (!g_aion_seen && Exists(g_sim_layout->dir + L"\\aion")) {
    g_aion_seen = true;
    g_mark_on = g_tree_calls[0];
  }
  if (g_aion_seen) g_puppet_tree_on = g_tree_calls[0] - g_mark_on;
  SetPlayerInputs();
  RunPuppetStep();
  // Where the movement's step and update leave the fake stand-in (the update puts it on the
  // ground unless skipped).
  if (g_teleports_at_air >= 0 && g_air_steps1 < 0) {
    float y = 0;
    memcpy(&y, g_fake_ai + 0xB0, sizeof y);
    g_air_max_y = std::max(g_air_max_y, y);
  }
  // An attack node checks two candidates: [candidate+0x20] is a health object.
  using TargetCheck = bool(__fastcall*)(void* node, void* attacker, void* candidate, uint8_t log);
  auto check = reinterpret_cast<TargetCheck>(static_cast<void*>(g_target_check_fixture));
  uint8_t standin[0x40] = {}, other[0x40] = {};
  const void* h = g_fake_ai + 0x4000;
  const void* o = g_fake_player;
  memcpy(standin + 0x20, &h, 8);
  memcpy(other + 0x20, &o, 8);
  g_target_standin = check(nullptr, nullptr, standin, 0) ? 1 : 0;
  g_target_other = check(nullptr, nullptr, other, 0) ? 1 : 0;
  // And a hit on each (rdx: a component of the target, [rdx+0x28] the target character).
  using Hit = int(__fastcall*)(void* attacker, const void* target, void* r8, void* r9);
  auto hit = reinterpret_cast<Hit>(static_cast<void*>(g_hit_fixture));
  uint8_t other_part[0x40] = {};
  memcpy(other_part + 0x28, &o, 8);
  g_hit_standin = hit(nullptr, h, nullptr, nullptr);  // the fake health object: +0x28 the fake AI
  g_hit_other = hit(nullptr, other_part, nullptr, nullptr);
  // And an area tick evaluating each (rdx: the character).
  using Aoe = void(__fastcall*)(void* tick, const void* character);
  auto aoe = reinterpret_cast<Aoe>(static_cast<void*>(g_aoe_fixture));
  uint8_t aoe_tick[0x40] = {};
  LONG seen = g_aoe_seen;
  aoe(aoe_tick, g_fake_ai);
  g_aoe_standin = g_aoe_seen != seen && g_aoe_last == g_fake_ai ? 1 : 0;
  seen = g_aoe_seen;
  aoe(aoe_tick, g_fake_player);
  g_aoe_other = g_aoe_seen != seen && g_aoe_last == g_fake_player ? 1 : 0;
  // And a full teleport's animation reset on each controller.
  using Reset = void(__fastcall*)(void* controller);
  auto reset = reinterpret_cast<Reset>(static_cast<void*>(g_reset_fixture));
  seen = g_reset_seen;
  reset(g_fake_anim_ctrl[0]);
  g_reset_standin = g_reset_seen != seen ? 1 : 0;
  seen = g_reset_seen;
  reset(g_fake_anim_ctrl[1]);
  g_reset_other = g_reset_seen != seen ? 1 : 0;
}

// The stand-in simulation thread: logs the world start after "go" (so it is the thread
// the guard calls the simulation thread), then calls the vtable slots like a frame loop.
DWORD WINAPI FakeSimThread(void*) {
  g_sim_thread = GetCurrentThreadId();
  using Line = void (*)(const char*, const char*);
  auto line = reinterpret_cast<Line>(GetProcAddress(g_sim_proxy, "SowGuardSelfTest_DebugLine"));
  bool sent = false;
  while (!g_sim_stop) {
    if (!sent && line && Exists(g_sim_layout->dir + L"\\go")) {
      line("[QUESTMGR]: ", "GameModeSet - SinglePlayer");
      sent = true;
    }
    if (g_walk_case) {
      WalkCaseFrame();
    } else if (g_walk_enabled && g_walk_frames < 200 && Exists(g_sim_layout->dir + L"\\move")) {
      float x = 0;
      memcpy(&x, g_fake_player + 0xAC, sizeof x);
      x += 0.5f;
      memcpy(g_fake_player + 0xAC, &x, sizeof x);
      ++g_walk_frames;
    }
    using Slot = void(__fastcall*)(void*);
    reinterpret_cast<Slot>(g_fake_vtable[1])(g_fake_player);
    reinterpret_cast<Slot>(g_fake_vtable[2])(g_fake_player);
    Sleep(5);
  }
  return 0;
}

void BuildLkpFixture() {
  static const uint8_t code[] = {
      0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40,  // prologue
      0x48, 0x8B, 0x05, 0, 0, 0, 0,                     // mov rax,[rip+GameClient+0x18]
      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,               // mov rax,LkpRecorder
      0xFF, 0xD0,                                       // call rax
      0x48, 0x83, 0xC4, 0x40, 0x5F,                     // add rsp,40h; pop rdi
      0x48, 0x8B, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x74, 0x24, 0x10, 0xC3};
  DWORD old = 0;
  VirtualProtect(g_lkp_fixture, sizeof g_lkp_fixture, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_lkp_fixture, code, sizeof code);
  const uintptr_t f = reinterpret_cast<uintptr_t>(g_lkp_fixture);
  const int32_t to_global = (int32_t)(reinterpret_cast<uintptr_t>(&g_fake_gameclient_ptr) + 0x18 - (f + 22));
  const uint64_t recorder = reinterpret_cast<uint64_t>(&LkpRecorder);
  memcpy(g_lkp_fixture + 18, &to_global, 4);
  memcpy(g_lkp_fixture + 24, &recorder, 8);
  FlushInstructionCache(GetCurrentProcess(), g_lkp_fixture, sizeof g_lkp_fixture);
}

// Puppet fixtures: a stand-in for 19B7AC4 (same 30-byte shape, calling TeleportRecorder),
// and a stand-in AI character in heap memory 10 units from the stand-in player, whose
// health object has the same class (vtable) as the player's.
alignas(16) uint8_t g_teleport_fixture[48];
void* g_fake_manager = nullptr;
const uint64_t g_fake_health_vtable[4] = {1, 2, 3, 4};
const uint64_t g_fake_ai_vtable[4] = {5, 6, 7, 8};
volatile LONG g_teleports = 0;
void* volatile g_teleported = nullptr;
float g_teleport_xform[7] = {};
volatile DWORD g_teleport_thread = 0;
volatile LONG g_player_tps = 0;
float g_player_tp_xform[7] = {};

float g_teleport_x[8192], g_teleport_y[8192];
__declspec(noinline) void TeleportRecorder(void*, void* object, const float* xform, bool) {
  if (object == static_cast<void*>(g_fake_player)) {  // probe "teleport"
    memcpy(g_player_tp_xform, xform, sizeof g_player_tp_xform);
    InterlockedIncrement(&g_player_tps);
    return;
  }
  const LONG i = g_teleports;
  if (i < 8192) {
    g_teleport_x[i] = xform[0];
    g_teleport_y[i] = xform[1];
  }
  if (object == static_cast<void*>(g_fake_ai)) {  // moved there, as the game's teleport does
    memcpy(g_fake_ai + 0xAC, xform, 3 * sizeof(float));
    memcpy(g_fake_ai + 0xB8, xform + 3, 4 * sizeof(float));
  }
  g_teleported = object;
  memcpy(g_teleport_xform, xform, sizeof g_teleport_xform);
  g_teleport_thread = GetCurrentThreadId();
  InterlockedIncrement(&g_teleports);
}

LONG TeleportCount() { return g_teleports; }

void BuildPuppetFixtures() {
  static const uint8_t code[] = {0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xC2, 0x41, 0xB1, 0x01, 0x48, 0x8B, 0xD1,
                                 0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x38, 0xC3};
  DWORD old = 0;
  VirtualProtect(g_teleport_fixture, sizeof g_teleport_fixture, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_teleport_fixture, code, sizeof code);
  const uintptr_t f = reinterpret_cast<uintptr_t>(g_teleport_fixture);
  const int32_t to_manager = (int32_t)(reinterpret_cast<uintptr_t>(&g_fake_manager) - (f + 20));
  const int32_t to_recorder = (int32_t)(reinterpret_cast<uintptr_t>(&TeleportRecorder) - (f + 25));
  memcpy(g_teleport_fixture + 16, &to_manager, 4);
  memcpy(g_teleport_fixture + 21, &to_recorder, 4);
  FlushInstructionCache(GetCurrentProcess(), g_teleport_fixture, sizeof g_teleport_fixture);
  // The player's health object gets a class; the AI and its health object live on the heap.
  const void* hv = g_fake_health_vtable;
  memcpy(g_fake_character, &hv, 8);
  g_fake_ai = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x8000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  uint8_t* health = g_fake_ai + 0x4000;
  const void* av = g_fake_ai_vtable;
  memcpy(g_fake_ai, &av, 8);
  memcpy(g_fake_ai + 0x26A8, &health, 8);
  const float pos[3] = {11.5f, -2.25f, 300.0f}, rot[4] = {0, 0, 0, 1}, hp[2] = {300.0f, 300.0f};
  memcpy(g_fake_ai + 0xAC, pos, sizeof pos);
  memcpy(g_fake_ai + 0xB8, rot, sizeof rot);
  memcpy(health, &hv, 8);
  memcpy(health + 0x28, &g_fake_ai, 8);
  memcpy(health + 0x428, hp, sizeof hp);
}

void BuildWalkFixtures(const std::wstring& dir) {
  static const uint8_t request[] = {0x48, 0x83, 0xEC, 0x28, 0xC7, 0x41, 0x68, 0x01, 0x00, 0x00, 0x00,
                                    0x8A, 0x02, 0x88, 0x41, 0x70,                  // the game's 16 bytes
                                    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0,  // mov rax,MoveRecorder; call rax
                                    0x48, 0x83, 0xC4, 0x28, 0xC3};
  static const uint8_t tick[] = {0x48, 0x83, 0xEC, 0x28, 0x49, 0x8B, 0x08, 0x33, 0xD2, 0xE8, 0, 0, 0, 0,
                                 0x48, 0x83, 0xC4, 0x28, 0xC3};
  static const uint8_t tree[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,  // the game's 10 bytes
                                 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};          // mov rax,TreeRecorder; jmp rax
  static const uint8_t setter[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x30,
                                   0x80, 0xB9, 0x88, 0x18, 0x00, 0x00, 0x00,  // the game's 20 bytes
                                   0x48, 0x83, 0xC4, 0x30, 0x5D,              // add rsp,30h; pop rbp
                                   0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax,VarRecorder; jmp rax
  static const uint8_t step[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
                                 0x89, 0x78, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70,  // the game's 23 bytes
                                 0x48, 0x83, 0xC4, 0x70, 0x5D,              // add rsp,70h; pop rbp
                                 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax,StepRecorder; jmp rax
  DWORD old = 0;
  for (uint8_t* f : {g_move_request_fixture, g_tick_fixture, g_tree_fixture, g_setter_fixture, g_mover_step_fixture,
                     g_mover_update_fixture})
    VirtualProtect(f, 64, PAGE_EXECUTE_READWRITE, &old);
  static const uint8_t update[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
                                   0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70,  // the game's 23 bytes
                                   0x48, 0x83, 0xC4, 0x70, 0x5D,              // add rsp,70h; pop rbp
                                   0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax,UpdateRecorder; jmp rax
  memcpy(g_mover_update_fixture, update, sizeof update);
  const uint64_t updaterec = reinterpret_cast<uint64_t>(&UpdateRecorder);
  memcpy(g_mover_update_fixture + 30, &updaterec, 8);
  static const uint8_t check[] = {
      0x48, 0x89, 0x5C, 0x24, 0x08, 0x44, 0x88, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10,  // the game's
      0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC,        // 37 bytes
      0x48, 0x83, 0xEC, 0x70, 0x4D, 0x8B, 0x70, 0x20,
      0x48, 0x83, 0xC4, 0x70, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D,  // epilogue
      0xB0, 0x01, 0xC3};                                                                      // mov al,1; ret
  VirtualProtect(g_target_check_buf, sizeof g_target_check_buf, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_target_check_fixture, check, sizeof check);
  FlushInstructionCache(GetCurrentProcess(), g_target_check_buf, sizeof g_target_check_buf);
  static const uint8_t hit[] = {
      0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,  // the game's
      0x57, 0x41, 0x56, 0x41, 0x57,                                                              // 20 bytes
      0x41, 0x5F, 0x41, 0x5E, 0x5F,                                                              // epilogue
      0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};                                                       // mov eax,1; ret
  VirtualProtect(g_hit_buf, sizeof g_hit_buf, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_hit_fixture, hit, sizeof hit);
  FlushInstructionCache(GetCurrentProcess(), g_hit_buf, sizeof g_hit_buf);
  static const uint8_t area[] = {
      0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,  // the game's
      0x41, 0x57,                                                                                      // 18 bytes
      0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D,                                // pops
      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax,AoeRecorder; jmp rax
  VirtualProtect(g_aoe_fixture, sizeof g_aoe_fixture, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_aoe_fixture, area, sizeof area);
  const uint64_t aoerec = reinterpret_cast<uint64_t>(&AoeRecorder);
  memcpy(g_aoe_fixture + 31, &aoerec, 8);
  FlushInstructionCache(GetCurrentProcess(), g_aoe_fixture, sizeof g_aoe_fixture);
  static const uint8_t anim_reset[] = {
      0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x33, 0xFF, 0x48, 0x8B, 0xD9,  // the game's 15
      0x48, 0x83, 0xC4, 0x30, 0x5F, 0x48, 0x8B, 0x5C, 0x24, 0x08,  // frame, push and rbx undone
      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};             // mov rax,ResetRecorder; jmp rax
  VirtualProtect(g_reset_fixture, sizeof g_reset_fixture, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_reset_fixture, anim_reset, sizeof anim_reset);
  const uint64_t resetrec = reinterpret_cast<uint64_t>(&ResetRecorder);
  memcpy(g_reset_fixture + 27, &resetrec, 8);
  FlushInstructionCache(GetCurrentProcess(), g_reset_fixture, sizeof g_reset_fixture);
  static const uint8_t thunk[] = {
      0x33, 0xC0, 0x48, 0x89, 0x81, 0xF0, 0x00, 0x00, 0x00, 0x89, 0x81, 0xF8, 0x00, 0x00, 0x00,  // the game's 28
      0x48, 0x89, 0x81, 0x08, 0x01, 0x00, 0x00, 0x89, 0x81, 0x10, 0x01, 0x00, 0x00,              // bytes
      0xE9, 0, 0, 0, 0};                                                                         // jmp start
  static const uint8_t start[] = {
      0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57,  // the game's
      0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,                                      // 22 bytes
      0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D,                    // pops
      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax,StartRecorder; jmp rax
  static const uint8_t stop[] = {
      0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // the game's
      0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00,                                                        // 23 bytes
      0x48, 0x81, 0xC4, 0xC0, 0x00, 0x00, 0x00, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C,        // undone
      0x5F, 0x5E, 0x5D,
      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax,StopRecorder; jmp rax
  for (uint8_t* f : {g_attack_thunk_fixture, g_attack_start_fixture, g_attack_stop_fixture})
    VirtualProtect(f, 64, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_attack_thunk_fixture, thunk, sizeof thunk);
  const int32_t to_start = (int32_t)(reinterpret_cast<uintptr_t>(g_attack_start_fixture) -
                                     reinterpret_cast<uintptr_t>(g_attack_thunk_fixture + sizeof thunk));
  memcpy(g_attack_thunk_fixture + 29, &to_start, 4);
  memcpy(g_attack_start_fixture, start, sizeof start);
  const uint64_t startrec = reinterpret_cast<uint64_t>(&StartRecorder);
  memcpy(g_attack_start_fixture + 35, &startrec, 8);
  memcpy(g_attack_stop_fixture, stop, sizeof stop);
  const uint64_t stoprec = reinterpret_cast<uint64_t>(&StopRecorder);
  memcpy(g_attack_stop_fixture + 43, &stoprec, 8);
  for (uint8_t* f : {g_attack_thunk_fixture, g_attack_start_fixture, g_attack_stop_fixture})
    FlushInstructionCache(GetCurrentProcess(), f, 64);
  const uint32_t ids[3] = {0x0BADF00D, kFakeAttackId, 0x7FFFFFFF}, type = 0xA15, count = 3;
  for (int i = 0; i < 3; ++i) {
    memcpy(g_fake_attacks + 40 * i, &type, 4);
    memcpy(g_fake_attacks + 40 * i + 4, &ids[i], 4);
  }
  const uint8_t* records = g_fake_attacks;
  memcpy(g_fake_attack_table + 0x28, &count, 4);
  memcpy(g_fake_attack_table + 0x38, &records, 8);
  memcpy(g_setter_fixture, setter, sizeof setter);
  const uint64_t varrec = reinterpret_cast<uint64_t>(&VarRecorder);
  memcpy(g_setter_fixture + 27, &varrec, 8);
  memcpy(g_mover_step_fixture, step, sizeof step);
  const uint64_t steprec = reinterpret_cast<uint64_t>(&StepRecorder);
  memcpy(g_mover_step_fixture + 30, &steprec, 8);
  memcpy(g_move_request_fixture, request, sizeof request);
  const uint64_t mover = reinterpret_cast<uint64_t>(&MoveRecorder);
  memcpy(g_move_request_fixture + 18, &mover, 8);
  memcpy(g_tick_fixture, tick, sizeof tick);
  const int32_t to_tree = (int32_t)(reinterpret_cast<uintptr_t>(g_tree_fixture) - reinterpret_cast<uintptr_t>(g_tick_fixture + 14));
  memcpy(g_tick_fixture + 10, &to_tree, 4);
  memcpy(g_tree_fixture, tree, sizeof tree);
  const uint64_t treerec = reinterpret_cast<uint64_t>(&TreeRecorder);
  memcpy(g_tree_fixture + 12, &treerec, 8);
  for (uint8_t* f : {g_move_request_fixture, g_tick_fixture, g_tree_fixture, g_setter_fixture, g_mover_step_fixture,
                     g_mover_update_fixture})
    FlushInstructionCache(GetCurrentProcess(), f, 64);
  for (void*& s : g_fake_mover_vtable) s = reinterpret_cast<void*>(&FakeOther);
  g_fake_mover_vtable[1 + 0x80 / 8] = g_mover_step_fixture;
  g_fake_mover_vtable[1 + 0x100 / 8] = g_mover_update_fixture;
  void* mvt = &g_fake_mover_vtable[1];
  memcpy(g_fake_mover, &mvt, 8);
  memcpy(g_fake_mover + 0x520, &g_fake_ai, 8);
  void* movement = g_fake_mover;
  memcpy(g_fake_ai + 0x24A0, &movement, 8);
  void* c = g_fake_controller;
  memcpy(g_fake_ai + 0x2C78, &c, 8);
  void* anim_ctrl = g_fake_anim_ctrl[0];
  memcpy(g_fake_ai + 0x1970, &anim_ctrl, 8);
  memcpy(g_fake_controller, &g_fake_ai, 8);
  void* t0 = g_fake_trees[0];
  memcpy(g_fake_ai + 0x2D38, &t0, 8);
  memcpy(g_fake_trees[0] + 8, &g_fake_ai, 8);
  void* other = g_other_ai;
  memcpy(g_fake_trees[1] + 8, &other, 8);
  g_walk_case = true;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const std::wstring ini = dir + L"\\sowguard.ini";
  wchar_t v[32];
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_move_request_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"move_request_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_tick_fixture + 4) - base));
  WritePrivateProfileStringW(L"sowguard", L"brain_site_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_tree_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"tree_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_setter_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"anim_var_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_mover_step_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"mover_step_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_follower_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"follower_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_target_check_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"target_check_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_hit_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"hit_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_aoe_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"aoe_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_reset_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"anim_reset_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_attack_thunk_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"attack_thunk_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_attack_stop_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"attack_stop_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(&g_fake_attack_table_ptr) - base));
  WritePrivateProfileStringW(L"sowguard", L"attack_table_rva", v, ini.c_str());
}

// coop_send and coop_listen in one process: the sender reads the stand-in player, the
// receiver shows it through the stand-in effect once the world has started ("go"), and
// again after the player moved ("move"). The parent asks for one manual "lkp" in between.
// The effect must run on the stand-in simulation thread, never on the pad thread.
int CoopChild(const Layout& L, HMODULE proxy, bool puppet) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SowGuardTestPad";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"coop test", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance,
                           nullptr);
  g_sim_layout = &L;
  g_sim_proxy = proxy;
  g_walk_enabled = puppet;
  HANDLE sim = CreateThread(nullptr, 0, FakeSimThread, nullptr, 0, nullptr);
  bool moved = false;
  const ULONGLONG t0 = GetTickCount64();
  while (!Exists(L.dir + L"\\stop") && GetTickCount64() - t0 < 30000) {
    XINPUT_STATE st{};
    XInputGetState(0, &st);
    if (!puppet && !moved && Exists(L.dir + L"\\move")) {
      const float x = 101.5f;
      memcpy(g_fake_player + 0xAC, &x, sizeof x);
      moved = true;
    }
    Sleep(5);
  }
  InterlockedExchange(&g_sim_stop, 1);
  if (sim) {
    WaitForSingleObject(sim, 5000);
    CloseHandle(sim);
  }
  if (g_walk_case) {
    float x = 0, last_tp = 0, creep_tp = 0;
    memcpy(&x, g_fake_player + 0xAC, sizeof x);
    const LONG n = std::min<LONG>(g_teleports + 0, 8192);
    // With a creep phase, last_tp_x is the last put-back before it; the creep's put-backs are
    // the ones before the air phase, whose own moves are counted apart (highest y).
    const LONG before_creep = g_teleports_at_creep >= 0 ? std::min(g_teleports_at_creep, n) : n;
    const LONG creep_end = g_teleports_at_air >= 0 ? std::min(g_teleports_at_air, n) : n;
    if (before_creep) last_tp = g_teleport_x[before_creep - 1];
    if (creep_end > before_creep) creep_tp = g_teleport_x[creep_end - 1];
    // After the air phase: a put-back on the ground (the landing).
    const bool landed = n > creep_end && g_teleport_y[n - 1] < 10.0f;
    const LONG air_steps = g_air_steps1 >= 0 && g_air_steps0 >= 0 ? g_air_steps1 - g_air_steps0 : -1;
    uint32_t mode = 0, action_count = 0;
    float stick = 0;
    memcpy(&mode, g_fake_controller + 0x68, 4);
    memcpy(&action_count, g_fake_ai + 0x5F8, 4);
    uint32_t player_actions = 0;
    memcpy(&player_actions, g_fake_player + 0x5F8, 4);
    memcpy(&stick, g_fake_ai + 0x8FC + 4 * 3, 4);
    char r[1024];
    snprintf(r, sizeof r,
             "puppet_tree_while_off=%ld other_tree>0=%d puppet_tree_after_on>0=%d requests>0=%d bad=%ld types=%ld "
             "leads=%d controller_ok=%ld idle_mode=%u pinned=%d player_tp=%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f "
             "inputs>0=%d movemag=%.2f velocity=%.1f axis=%.2f stray=%ld teleports_before_move=%ld last_tp_x=%.1f "
             "steps>0=%d step_speed=%.0f creep_put_back=%d creep_tp_x>571=%d actions=%u held110=%u stick=%.2f "
             "flags=%u%u player_actions=%u player_flags=%u%u follower=%d target_standin=%d target_other=%d "
             "target_after=%d hit_standin=%d hit_other=%d hit_after=%d air_y>50=%d air_steps_few=%d landed=%d "
             "attack_starts=%ld attack_stops=%ld attack_bad=%ld aoe_standin=%d aoe_other=%d aoe_after=%d "
             "reset_standin=%d reset_other=%d reset_after=%d",
             g_puppet_tree_off, g_other_tree_off > 0, g_puppet_tree_on > 0, g_requests > 0, g_bad_requests,
             g_types_seen, g_max_lead > 50.0f, g_controller_ok, mode, n > 0 && std::fabs(g_teleport_x[n - 1] - x) < 0.01f,
             g_player_tp_xform[0], g_player_tp_xform[1], g_player_tp_xform[2], g_player_tp_xform[3],
             g_player_tp_xform[4], g_player_tp_xform[5], g_player_tp_xform[6], g_puppet_vars > 0, g_seen_movemag,
             g_seen_velocity, g_seen_axis, g_stray_vars, g_teleports_at_move, last_tp, g_step_calls > 0,
             g_step_speed, creep_end > before_creep, creep_tp > 571.5f, action_count, g_fake_ai[0x5FC + 110], stick,
             g_fake_ai[0x1129], g_fake_ai[0x113A], player_actions, g_fake_player[0x1129], g_fake_player[0x113A],
             g_follower_at_free, g_target_at_free, g_target_other, g_target_standin, g_hit_at_free, g_hit_other,
             g_hit_standin, g_air_max_y > 50.0f, air_steps >= 0 && air_steps * 2 < g_air_frames, landed,
             g_attack_starts + 0, g_attack_stops + 0, g_attack_bad + 0, g_aoe_at_free, g_aoe_other, g_aoe_standin,
             g_reset_at_free, g_reset_other, g_reset_standin);
    WriteText(L.dir + L"\\walk.txt", r);
    DestroyWindow(w);
    return 0;
  }
  if (puppet) {
    const LONG n = std::min<LONG>(g_teleports + 0, 8192);
    float max_step = 0;
    bool monotonic = true;
    for (LONG i = 1; i < n; ++i) {
      const float step = g_teleport_x[i] - g_teleport_x[i - 1];
      if (step < -1e-4f) monotonic = false;
      max_step = std::max(max_step, step);
    }
    char smooth[96];
    snprintf(smooth, sizeof smooth, "walked=%d monotonic=%d max_step_le_1=%d last_x=%.2f", g_walk_frames, monotonic,
             max_step <= 1.0f, n ? g_teleport_x[n - 1] : 0.0f);
    WriteText(L.dir + L"\\smooth.txt", smooth);
    char r[200];
    snprintf(r, sizeof r, "teleports>0=%d object_ok=%d on_sim=%d xform=%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f",
             g_teleports > 0, g_teleported == static_cast<void*>(g_fake_ai), g_teleport_thread == g_sim_thread,
             g_teleport_xform[0], g_teleport_xform[1], g_teleport_xform[2], g_teleport_xform[3], g_teleport_xform[4],
             g_teleport_xform[5], g_teleport_xform[6]);
    WriteText(L.dir + L"\\puppet.txt", r);
    DestroyWindow(w);
    return 0;
  }
  const LONG calls = std::min<LONG>(g_lkp_calls + 0, 64);
  std::string report = "calls=" + std::to_string(calls) +
                       " player_ok=" + std::to_string(g_lkp_player == static_cast<void*>(g_fake_player)) +
                       " on_sim=" + std::to_string(calls > 0 && g_lkp_thread == g_sim_thread) +
                       " vtable_restored=" + std::to_string(g_fake_vtable[2] == reinterpret_cast<void*>(&FakeOther)) +
                       " seen=";
  char b[64];
  for (LONG i = 0; i < calls; ++i) {
    snprintf(b, sizeof b, "%s%.2f,%.2f,%.2f", i ? ";" : "", g_lkp_seen[i][0], g_lkp_seen[i][1], g_lkp_seen[i][2]);
    report += b;
  }
  WriteText(L.dir + L"\\coop.txt", report);
  DestroyWindow(w);
  return 0;
}

// probe "stacks": a stand-in with the entry bytes of the game's move-request setter
// (3F4BD8: sub rsp,28h; mov dword [rcx+68h],1), called from two known functions on two
// objects. The relocated entry must keep doing its job while stacks are recorded.
alignas(16) uint8_t g_move_fixture[32];
const uint8_t kMoveFixtureCode[] = {0x48, 0x83, 0xEC, 0x28, 0xC7, 0x41, 0x68, 0x01,
                                    0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3};
alignas(16) uint8_t g_stack_objects[2][0x80];
volatile LONG g_stack_rounds = 0, g_stack_bad = 0, g_stack_calls_a = 0, g_stack_calls_b = 0;
using MoveFixture_t = void(__fastcall*)(void*);
// Different bodies (no identical-code folding) with work after the call (no tail jump),
// so each caller shows up as its own frame.
__declspec(noinline) void StacksCallerA(void* o) {
  reinterpret_cast<MoveFixture_t>(reinterpret_cast<void*>(g_move_fixture))(o);
  InterlockedIncrement(&g_stack_calls_a);
}
__declspec(noinline) void StacksCallerB(void* o) {
  reinterpret_cast<MoveFixture_t>(reinterpret_cast<void*>(g_move_fixture))(o);
  InterlockedAdd(&g_stack_calls_b, 2);
}

DWORD WINAPI StacksCallerThread(void*) {
  while (!g_sim_stop) {
    for (int k = 0; k < 2; ++k) {
      uint8_t* o = g_stack_objects[k];
      const uint32_t zero = 0;
      memcpy(o + 0x68, &zero, 4);
      (k ? StacksCallerB : StacksCallerA)(o);
      uint32_t v = 0;
      memcpy(&v, o + 0x68, 4);
      if (v != 1) InterlockedIncrement(&g_stack_bad);
    }
    InterlockedIncrement(&g_stack_rounds);
    Sleep(1);
  }
  return 0;
}

void BuildStacksFixtures(const std::wstring& dir) {
  DWORD old = 0;
  VirtualProtect(g_move_fixture, sizeof g_move_fixture, PAGE_EXECUTE_READWRITE, &old);
  memcpy(g_move_fixture, kMoveFixtureCode, sizeof kMoveFixtureCode);
  FlushInstructionCache(GetCurrentProcess(), g_move_fixture, sizeof g_move_fixture);
  const uint64_t marks[2] = {0x5A5A0001, 0x5A5A0002};
  for (int k = 0; k < 2; ++k) memcpy(g_stack_objects[k], &marks[k], 8);
  // Only this process knows where they are, so it adds them to the ini itself.
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const std::wstring ini = dir + L"\\sowguard.ini";
  wchar_t v[32];
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_move_fixture) - base));
  WritePrivateProfileStringW(L"sowguard", L"test_stacks_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(&StacksCallerA) - base));
  WritePrivateProfileStringW(L"sowguard", L"test_caller_a_rva", v, ini.c_str());
  swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(&StacksCallerB) - base));
  WritePrivateProfileStringW(L"sowguard", L"test_caller_b_rva", v, ini.c_str());
}

int StacksChild(const Layout& L) {
  HANDLE t = CreateThread(nullptr, 0, StacksCallerThread, nullptr, 0, nullptr);
  const ULONGLONG t0 = GetTickCount64();
  while (!Exists(L.dir + L"\\stop") && GetTickCount64() - t0 < 30000) Sleep(20);
  InterlockedExchange(&g_sim_stop, 1);
  if (t) {
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
  }
  char report[96];
  snprintf(report, sizeof report, "rounds>0=%d bad=%ld restored=%d", g_stack_rounds > 0, g_stack_bad,
           memcmp(g_move_fixture, kMoveFixtureCode, sizeof kMoveFixtureCode) == 0);
  WriteText(L.dir + L"\\stacks.txt", report);
  return 0;
}

// background_run=1: focus-loss messages must not reach the window procedure (activation
// still does), and the cursor imports must point at the guard.
volatile LONG g_inactive_seen = 0, g_active_seen = 0, g_killfocus_seen = 0;
LRESULT CALLBACK TestBackgroundProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_ACTIVATEAPP) InterlockedIncrement(wp ? &g_active_seen : &g_inactive_seen);
  if (m == WM_KILLFOCUS) InterlockedIncrement(&g_killfocus_seen);
  return DefWindowProcW(w, m, wp, lp);
}

int BackgroundChild(const Layout& L) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = TestBackgroundProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SowGuardTestPad";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"background test", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                           wc.hInstance, nullptr);
  std::string log;
  for (int i = 0; i < 200 && log.find("background run on") == std::string::npos; ++i) {
    Sleep(50);
    ReadText(L.Profile(L"sowguard.log"), &log);
  }
  SendMessageW(w, WM_ACTIVATEAPP, FALSE, 0);
  SendMessageW(w, WM_ACTIVATEAPP, TRUE, 0);
  SendMessageW(w, WM_KILLFOCUS, 0, 0);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  const bool clip = reinterpret_cast<FARPROC>(&ClipCursor) != GetProcAddress(user32, "ClipCursor");
  const bool setpos = reinterpret_cast<FARPROC>(&SetCursorPos) != GetProcAddress(user32, "SetCursorPos");
  const bool focus = GetForegroundWindow() == w && GetActiveWindow() == w;
  char report[128];
  snprintf(report, sizeof report, "inactive=%ld active=%ld killfocus=%ld clip_guarded=%d setpos_guarded=%d own_focus=%d",
           g_inactive_seen, g_active_seen, g_killfocus_seen, clip, setpos, focus);
  WriteText(L.dir + L"\\background.txt", report);
  DestroyWindow(w);
  return 0;
}

// Reads the pad like the game and logs the world start after the parent's "go"; the
// save must run from a pad read, and only after the world start.
int PadSaveChild(const Layout& L, HMODULE proxy) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SowGuardTestPad";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"pad test", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance,
                           nullptr);
  using Line = void (*)(const char*, const char*);
  auto line = reinterpret_cast<Line>(GetProcAddress(proxy, "SowGuardSelfTest_DebugLine"));
  bool sent = false, hold = false;
  const ULONGLONG t0 = GetTickCount64();
  while (!Exists(L.dir + L"\\stop") && GetTickCount64() - t0 < 30000) {
    XINPUT_STATE st{};
    XInputGetState(0, &st);
    if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_A) && st.Gamepad.sThumbLY == 32767) hold = true;
    if (!sent && line && Exists(L.dir + L"\\go")) {
      line("[QUESTMGR]: ", "GameModeSet - SinglePlayer");
      sent = true;
    }
    Sleep(5);
  }
  char report[64];
  snprintf(report, sizeof report, "hold=%d saves=%ld player_ok=%d", hold, g_saves,
           g_saved_player == static_cast<void*>(g_fake_player));
  WriteText(L.dir + L"\\padsave.txt", report);
  DestroyWindow(w);
  return 0;
}

// Virtual pad (autoload=1): reads user 0 through this exe's own XInputGetState import, as
// the game does, and counts A presses. With send_load the first press is answered with
// the game's world-start line, which must end the pressing.
int PadChild(const Layout& L, HMODULE proxy, bool send_load) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SowGuardTestPad";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"pad test", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance,
                           nullptr);
  using Line = void (*)(const char*, const char*);
  auto load_line = reinterpret_cast<Line>(GetProcAddress(proxy, "SowGuardSelfTest_DebugLine"));
  int presses = 0;
  bool prev = false, connected = true, packets_move = false;
  DWORD first_packet = 0;
  const ULONGLONG t0 = GetTickCount64();
  for (int i = 0; GetTickCount64() - t0 < 4000; ++i) {
    XINPUT_STATE st{};
    if (XInputGetState(0, &st) != ERROR_SUCCESS) connected = false;
    if (i == 0) first_packet = st.dwPacketNumber;
    else if (st.dwPacketNumber != first_packet) packets_move = true;
    bool a = (st.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
    if (a && !prev && ++presses == 1 && send_load && load_line)
      load_line("[QUESTMGR]: ", "GameModeSet - SinglePlayer");
    prev = a;
    Sleep(5);
  }
  using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
  auto real = reinterpret_cast<GetState>(GetProcAddress(GetModuleHandleW(L"XINPUT9_1_0.dll"), "XInputGetState"));
  XINPUT_STATE a{}, b{};
  bool other_same = real && XInputGetState(1, &a) == real(1, &b);
  char report[96];
  snprintf(report, sizeof report, "presses=%d connected=%d packets=%d other=%d", presses, connected, packets_move,
           other_same);
  WriteText(L.dir + L"\\pad.txt", report);
  DestroyWindow(w);
  return 0;
}

int Child(const std::wstring& name, const std::wstring& dir, const std::wstring& arg) {
  Layout L(dir);
  HMODULE fake_steam = nullptr;
  if (name == L"steam_block" || name == L"steam_wrapper_mismatch" || name == L"steam_cloud") {
    // Like the game: steam_api64.dll is loaded before the guard's thread looks for it.
    fake_steam = LoadLibraryW((dir + L"\\steam_api64.dll").c_str());
    if (!fake_steam) return 5;
    if (name == L"steam_wrapper_mismatch") {
      auto* f = reinterpret_cast<uint8_t*>(GetProcAddress(fake_steam, "SteamAPI_ISteamRemoteStorage_FileWrite"));
      DWORD old = 0;
      VirtualProtect(f, 1, PAGE_EXECUTE_READWRITE, &old);
      f[0] = 0xC3;  // no longer mov rax,[rcx]; jmp [rax]
      VirtualProtect(f, 1, old, &old);
    }
  }
  const bool recv_case = name == L"coop_puppet_recv" || name == L"coop_puppet_recv_nobuttons";
  if (name == L"coop_loop" || name == L"coop_puppet" || name == L"coop_puppet_walk" || recv_case) {
    BuildPlayerFixtures();
    BuildLkpFixture();
    if (name != L"coop_loop") {
      BuildPuppetFixtures();
      if (name == L"coop_puppet_walk" || recv_case) BuildWalkFixtures(dir);
      wchar_t t[32];
      swprintf_s(t, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_teleport_fixture) -
                                                 reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
      WritePrivateProfileStringW(L"sowguard", L"teleport_rva", t, (dir + L"\\sowguard.ini").c_str());
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const std::wstring ini = dir + L"\\sowguard.ini";
    wchar_t v[64];
    swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(&g_fake_gameclient_ptr) - base));
    WritePrivateProfileStringW(L"sowguard", L"gameclient_rva", v, ini.c_str());
    swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_lkp_fixture) - base));
    WritePrivateProfileStringW(L"sowguard", L"lkp_rva", v, ini.c_str());
    swprintf_s(v, L"%llX:1", (unsigned long long)(reinterpret_cast<uintptr_t>(g_fake_vtable) - base));
    WritePrivateProfileStringW(L"sowguard", L"sim_hook", v, ini.c_str());
    swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_fake_vtable) - base));
    WritePrivateProfileStringW(L"sowguard", L"test_vtable_rva", v, ini.c_str());
    const unsigned port = 47100 + GetCurrentProcessId() % 500;
    swprintf_s(v, L"127.0.0.1:%u", port);
    WritePrivateProfileStringW(L"sowguard", L"coop_send", v, ini.c_str());
    swprintf_s(v, L"%u", port);
    WritePrivateProfileStringW(L"sowguard", L"coop_listen", v, ini.c_str());
    // A receiver only: the test itself sends the packets (so nothing here records inputs).
    if (recv_case) WritePrivateProfileStringW(L"sowguard", L"coop_send", nullptr, ini.c_str());
  }
  if (name == L"probe_stacks") BuildStacksFixtures(dir);
  if (name == L"probe_pad_save") {
    // Only this process knows where its stand-ins are, so it adds them to the ini itself.
    BuildPlayerFixtures();
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    wchar_t v[32];
    swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(&g_fake_gameclient_ptr) - base));
    WritePrivateProfileStringW(L"sowguard", L"gameclient_rva", v, (dir + L"\\sowguard.ini").c_str());
    swprintf_s(v, L"%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(g_save_handler) - base));
    WritePrivateProfileStringW(L"sowguard", L"save_handler_rva", v, (dir + L"\\sowguard.ini").c_str());
  }
  const bool hook_case = name == L"probe_hook" || name == L"probe_hook_refused" || name == L"probe_write_off";
  if (hook_case) {
    DWORD old = 0;
    VirtualProtect(g_dp_area, sizeof g_dp_area, PAGE_EXECUTE_READWRITE, &old);
    if (name == L"probe_hook_refused") g_dp_area[0] = 0x90;  // no longer the known stub
    // Only this process knows its RVAs, so it adds debug_channels to the ini itself.
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    wchar_t channels[64];
    swprintf_s(channels, L"%llX,%llX", (unsigned long long)(reinterpret_cast<uintptr_t>(&g_channels[0]) - base),
               (unsigned long long)(reinterpret_cast<uintptr_t>(&g_channels[2]) - base));
    WritePrivateProfileStringW(L"sowguard", L"debug_channels", channels, (dir + L"\\sowguard.ini").c_str());
  }
  HMODULE proxy = LoadLibraryW((dir + L"\\bink2w64.dll").c_str());
  if (!proxy) {
    printf("    LoadLibrary failed (%lu)\n", GetLastError());
    return 2;
  }
  bool ok = true;

  if (name == L"load_only") return 0;
  if (name == L"probe_serve") return ProbeChild(L);
  if (name == L"steam_block") return SteamChild(L, fake_steam);
  if (name == L"steam_cloud") return SteamCloudChild(L, fake_steam);
  if (name == L"steam_wrapper_mismatch") {
    Sleep(10000);  // the guard should have stopped the process by now
    return 1;
  }
  if (hook_case) return HookChild(L, name);
  if (name == L"pad_load" || name == L"pad_max") return PadChild(L, proxy, name == L"pad_load");
  if (name == L"probe_pad_save") return PadSaveChild(L, proxy);
  if (name == L"coop_loop" || name == L"coop_puppet" || name == L"coop_puppet_walk" || recv_case)
    return CoopChild(L, proxy, name != L"coop_loop");
  if (name == L"background_run") return BackgroundChild(L);
  if (name == L"probe_stacks") return StacksChild(L);

  if (name == L"redirect_write") {
    ok &= Check(CreateDirectoryW(L.sow.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS, "create save dir");
    ok &= Check(WriteText(L.sow + L"\\new.sav", "NEW"), "write new.sav at the owner path");
  } else if (name == L"redirect_read") {
    ok &= Check(FileIs(L.sow + L"\\p.sav", "PROFILE"), "owner path p.sav reads the profile copy");
    HANDLE f = CreateFileW((L.sow + L"\\o.sav").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD err = GetLastError();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    ok &= Check(f == INVALID_HANDLE_VALUE && err == ERROR_FILE_NOT_FOUND, "owner-only o.sav is invisible");
    ok &= Check(GetFileAttributesW((L.sow + L"\\o.sav").c_str()) == INVALID_FILE_ATTRIBUTES, "GetFileAttributes o.sav fails");
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    ok &= Check(GetFileAttributesExW((L.sow + L"\\p.sav").c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow == 7,
                "GetFileAttributesEx p.sav sees the profile file (7 bytes)");
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((L.sow + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) names.push_back(fd.cFileName);
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    std::sort(names.begin(), names.end());
    ok &= Check(names == std::vector<std::wstring>{L"c.sav", L"p.sav"}, "directory listing shows only profile files");
  } else if (name == L"path_forms") {
    std::wstring fwd = L.sow;
    std::replace(fwd.begin(), fwd.end(), L'\\', L'/');
    std::wstring lower = L.sow;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    ok &= Check(WriteText(fwd + L"/f1.sav", "1"), "forward slashes");
    ok &= Check(WriteText(lower + L"\\f2.sav", "2"), "lower case");
    ok &= Check(WriteText(L"\\\\?\\" + L.sow + L"\\f3.sav", "3"), "\\\\?\\ prefix");
    ok &= Check(SetCurrentDirectoryW(L.local_wb.c_str()) != FALSE, "cd into owner WB Games");
    ok &= Check(WriteText(L"Shadow of War\\f4.sav", "4"), "relative to current directory");
    HANDLE d = CreateFileW(L.sow.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ok &= Check(d != INVALID_HANDLE_VALUE, "open owner save dir handle");
    if (d != INVALID_HANDLE_VALUE) {
      ok &= Check(NtWriteRelative(d, L"f5.sav", "5"), "NtCreateFile relative to a directory handle");
      CloseHandle(d);
    }
  } else if (name == L"rename_delete") {
    ok &= Check(WriteText(L.sow + L"\\a.sav", "A"), "write a.sav");
    ok &= Check(MoveFileExW((L.sow + L"\\a.sav").c_str(), (L.sow + L"\\b.sav").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE,
                "rename a.sav -> b.sav");
    ok &= Check(DeleteFileW((L.sow + L"\\c.sav").c_str()) != FALSE, "delete c.sav");
    ok &= Check(CreateDirectoryW((L.sow + L"\\sub").c_str(), nullptr) && RemoveDirectoryW((L.sow + L"\\sub").c_str()),
                "create and remove sub dir");
    ok &= Check(CreateDirectoryW((L.sow + L"\\keep").c_str(), nullptr) != FALSE, "create keep dir");
  } else if (name == L"steam_remote") {
    std::string s;
    ok &= Check(!ReadText(L.remote + L"\\cloud.sav", &s), "owner cloud.sav is invisible");
    ok &= Check(CreateDirectoryW(L.remote.c_str(), nullptr) != FALSE, "create remote dir");
    ok &= Check(WriteText(L.remote + L"\\new.sav", "STEAM"), "write remote\\new.sav");
  } else if (name == L"install_read") {
    ok &= Check(FileIs(L.install + L"\\game.bin", "GAME"), "read owner install file");
  } else if (name == L"guard_install_write") {
    HANDLE f = CreateFileW((L.install + L"\\game.bin").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    printf("    still alive after opening install file for write (handle %p)\n", f);
    return 1;
  } else if (name == L"guard_install_delete") {
    DeleteFileW((L.install + L"\\game.bin").c_str());
    printf("    still alive after deleting install file\n");
    return 1;
  } else if (name == L"guard_create_in_install") {
    WriteText(L.install + L"\\new.bin", "X");
    printf("    still alive after creating a file in the install\n");
    return 1;
  } else if (name == L"guard_rename_into_install") {
    ok &= Check(WriteText(L.sow + L"\\m.sav", "M"), "write m.sav (profile)");
    MoveFileExW((L.sow + L"\\m.sav").c_str(), (L.install + L"\\evil.bin").c_str(), MOVEFILE_REPLACE_EXISTING);
    printf("    still alive after renaming into the install\n");
    return 1;
  } else if (name == L"network_blocked" || name == L"network_allowed") {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int err = WSAGetLastError();
    if (s != INVALID_SOCKET) closesocket(s);
    printf("    socket() %s (WSA error %d)\n", s == INVALID_SOCKET ? "failed" : "succeeded", err);
    ok &= Check((s == INVALID_SOCKET) == (name == L"network_blocked"), "socket result as configured");
  } else if (name == L"network_offline") {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok &= Check(t != INVALID_SOCKET && u != INVALID_SOCKET, "sockets can be created");
    closesocket(t);
    closesocket(u);
    auto unreachable = [&](int err, const char* what) {
      printf("    %s -> %d\n", what, err);
      return Check(err == WSAENETUNREACH, "  ... fails with WSAENETUNREACH (10051)");
    };
    ok &= unreachable(TryConnect("192.0.2.1", 80), "tcp connect 192.0.2.1:80");
    ok &= unreachable(TryConnectEx("192.0.2.1", 443), "ConnectEx 192.0.2.1:443");
    ok &= unreachable(TrySendTo("192.0.2.1", 53), "udp sendto 192.0.2.1:53");
    ok &= unreachable(TryUdpConnectSend("192.0.2.1", 3478), "udp connect+send 192.0.2.1:3478");
    ok &= unreachable(TryConnect("2001:db8::1", 80), "tcp connect [2001:db8::1]:80");
    ok &= unreachable(TryConnectEx("2001:db8::1", 443), "ConnectEx [2001:db8::1]:443");
    ok &= unreachable(TrySendTo("2001:db8::1", 53), "udp sendto [2001:db8::1]:53");
    DWORD ms = 0;
    DWORD herr = WinHttpTry(L"192.0.2.1", &ms);
    printf("    WinHTTP GET http://192.0.2.1/ -> error %lu after %lu ms\n", herr, ms);
    ok &= Check(herr == ERROR_WINHTTP_CANNOT_CONNECT && ms < 2000, "WinHTTP fails to connect at once (12029, < 2 s)");
    // Loopback must keep working (local co-op tests will need it).
    SOCKET l4 = INVALID_SOCKET, l6 = INVALID_SOCKET;
    unsigned short p4 = Listen("127.0.0.1", &l4), p6 = Listen("::1", &l6);
    ok &= Check(p4 && TryConnect("127.0.0.1", p4) == 0, "tcp connect to a 127.0.0.1 listener works");
    ok &= Check(p4 && TryConnectEx("127.0.0.1", p4) == 0, "ConnectEx to a 127.0.0.1 listener works");
    ok &= Check(p6 && TryConnect("::1", p6) == 0, "tcp connect to a ::1 listener works");
    ok &= Check(UdpLoopbackEcho(), "udp datagram over 127.0.0.1 arrives");
    if (l4 != INVALID_SOCKET) closesocket(l4);
    if (l6 != INVALID_SOCKET) closesocket(l6);
  } else if (name == L"net_probe") {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    printf("    tcp connect   127.0.0.1:0x1201  -> %d\n", TryConnect("127.0.0.1", 0x1201));
    printf("    tcp connect   192.0.2.1:0x1202  -> %d\n", TryConnect("192.0.2.1", 0x1202));
    printf("    connectex     127.0.0.1:0x1203  -> %d\n", TryConnectEx("127.0.0.1", 0x1203));
    printf("    connectex     192.0.2.1:0x1204  -> %d\n", TryConnectEx("192.0.2.1", 0x1204));
    printf("    udp sendto    127.0.0.1:0x1205  -> %d\n", TrySendTo("127.0.0.1", 0x1205));
    printf("    udp sendto    192.0.2.1:0x1206  -> %d\n", TrySendTo("192.0.2.1", 0x1206));
    printf("    udp conn+send 192.0.2.1:0x1207  -> %d\n", TryUdpConnectSend("192.0.2.1", 0x1207));
    printf("    tcp connect   ::1:0x1208        -> %d\n", TryConnect("::1", 0x1208));
    printf("    tcp connect   2001:db8::1:0x1209 -> %d\n", TryConnect("2001:db8::1", 0x1209));
    printf("    connectex     2001:db8::1:0x120a -> %d\n", TryConnectEx("2001:db8::1", 0x120a));
    printf("    udp sendto    2001:db8::1:0x120b -> %d\n", TrySendTo("2001:db8::1", 0x120b));
  } else if (name == L"steam_appid_ok") {
    wchar_t v[32] = {};
    GetEnvironmentVariableW(L"SteamAppId", v, 32);
    ok &= Check(wcscmp(v, L"356190") == 0, "SteamAppId set to 356190");
  } else if (name == L"forwarders") {
    // A forwarder loads its target on first resolution, as the game's static import
    // of BinkGoto does at load time.
    ok &= Check(GetProcAddress(proxy, "BinkGoto") != nullptr, "resolve BinkGoto (the game's static import)");
    HMODULE orig = GetModuleHandleW(L"bink2w64_.dll");
    ok &= Check(orig != nullptr, "bink2w64_.dll loaded through the forwarders");
    if (orig) {
      auto* base = reinterpret_cast<BYTE*>(orig);
      auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
      auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
      auto* ex = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dd.VirtualAddress);
      auto* names = reinterpret_cast<DWORD*>(base + ex->AddressOfNames);
      int same = 0;
      for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        const char* n = reinterpret_cast<const char*>(base + names[i]);
        FARPROC a = GetProcAddress(proxy, n), b = GetProcAddress(orig, n);
        if (a && a == b) ++same;
        else printf("    export %s differs\n", n);
      }
      printf("    %d of %lu exports resolve to the original\n", same, ex->NumberOfNames);
      ok &= Check(same == 73 && ex->NumberOfNames == 73, "all 73 Bink exports forward to the original");
    }
  } else if (name == L"hash") {
    using Hash_t = int (*)(const wchar_t*, char*);
    auto hash = reinterpret_cast<Hash_t>(GetProcAddress(proxy, "SowGuardSelfTest_HashFile"));
    char out[65] = {};
    ok &= Check(hash && hash(arg.c_str(), out), "hash the game exe");
    printf("    sha256 %s\n", out);
    ok &= Check(strcmp(out, kGameExeSha256) == 0, "matches the SHA-256 recorded in the notes");
  } else {
    printf("    unknown case\n");
    return 3;
  }
  return ok ? 0 : 1;
}

// ---------------------------------------------------------------- parent

struct Env {
  std::wstring root, proxy, orig, exe_to_hash;
};

struct Case {
  std::wstring name;
  std::wstring child;  // child case to run (defaults to name)
  DWORD expect;
  std::vector<std::pair<std::wstring, std::wstring>> ini;
  std::function<bool(const Layout&)> post;
  bool seed_appid = false;
  bool drop_ini = false;
  std::function<bool(const Layout&)> live;  // runs while the child is alive, then a "stop" file ends it
  bool fake_steam = false;                  // copy build\fake\steam_api64.dll into the case
};

void PrepareCase(const Env& env, const Layout& L, const Case& c) {
  RemoveTree(L.dir);
  MakeDirs(L.sow);
  MakeDirs(L.dir + L"\\owner\\Documents\\WB Games");
  MakeDirs(L.remote);
  MakeDirs(L.install);
  MakeDirs(L.Profile(L"LocalAppData\\WB Games\\Shadow of War"));
  WriteText(L.sow + L"\\o.sav", "OWNER");
  WriteText(L.remote + L"\\cloud.sav", "CLOUD");
  WriteText(L.install + L"\\game.bin", "GAME");
  WriteText(L.PSow(L"p.sav"), "PROFILE");
  WriteText(L.PSow(L"c.sav"), "DELETE-ME");
  CopyFileW(Self().c_str(), (L.dir + L"\\test_guard.exe").c_str(), FALSE);
  CopyFileW(env.proxy.c_str(), (L.dir + L"\\bink2w64.dll").c_str(), FALSE);
  CopyFileW(env.orig.c_str(), (L.dir + L"\\bink2w64_.dll").c_str(), FALSE);
  if (c.seed_appid) WriteText(L.dir + L"\\steam_appid.txt", "356190\r\n");
  if (c.fake_steam) {
    std::wstring self = Self();
    CopyFileW((self.substr(0, self.find_last_of(L'\\')) + L"\\fake\\steam_api64.dll").c_str(),
              (L.dir + L"\\steam_api64.dll").c_str(), FALSE);
  }
  if (c.drop_ini) return;

  std::map<std::wstring, std::wstring> ini = {
      {L"test_mode", L"1"},
      {L"profile", L.dir + L"\\profile"},
      {L"allowed_root", L.dir},
      {L"network", L"offline"},
      {L"owner_localappdata", L.dir + L"\\owner\\Local"},
      {L"owner_documents", L.dir + L"\\owner\\Documents"},
      {L"owner_steam", L.dir + L"\\owner\\Steam"},
      {L"owner_install", L.dir + L"\\owner\\Steam\\steamapps\\common\\ShadowOfWar"},
  };
  for (const auto& kv : c.ini) ini[kv.first] = kv.second;
  std::string text = "[sowguard]\r\n";
  for (const auto& kv : ini) {
    std::wstring line = kv.first + L"=" + kv.second + L"\r\n";
    char buf[2048];
    int n = WideCharToMultiByte(CP_ACP, 0, line.c_str(), -1, buf, sizeof buf, nullptr, nullptr);
    text.append(buf, n - 1);
  }
  WriteText(L.dir + L"\\sowguard.ini", text);
}

DWORD RunChild(const Layout& L, const std::wstring& child, const std::wstring& arg,
               const std::function<bool(const Layout&)>& live = nullptr, bool* live_ok = nullptr) {
  std::wstring cmd = L"\"" + L.dir + L"\\test_guard.exe\" case " + child + L" \"" + L.dir + L"\" \"" + arg + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, L.dir.c_str(), &si, &pi)) return 0xFFFFFFFF;
  if (live) {
    *live_ok = live(L);
    WriteText(L.dir + L"\\stop", "");
  }
  DWORD exit_code = 0xFFFFFFFE;
  if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exit_code);
  else TerminateProcess(pi.hProcess, 0xDEAD);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return exit_code;
}

bool LogHas(const Layout& L, const char* needle) {
  std::string s;
  HANDLE f = CreateFileW(L.Profile(L"sowguard.log").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  std::vector<char> buf(1 << 20);
  DWORD got = 0;
  ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr);
  CloseHandle(f);
  return std::string(buf.data(), got).find(needle) != std::string::npos;
}

bool WaitFile(const std::wstring& path, int ms) {
  for (int waited = 0; waited < ms; waited += 50) {
    if (Exists(path)) return true;
    Sleep(50);
  }
  return Exists(path);
}

struct ProbePipe {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool Open(const std::wstring& name) {
    for (int i = 0; i < 100 && h == INVALID_HANDLE_VALUE; ++i) {
      h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        WaitNamedPipeW(name.c_str(), 100);
        Sleep(50);
      }
    }
    return h != INVALID_HANDLE_VALUE;
  }
  std::string Ask(const std::string& command) {
    std::string line = command + "\n", out;
    DWORD n = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &n, nullptr);
    char buf[4096];
    while (out.size() < 4 || out.compare(out.size() - 4, 4, "END\n") != 0) {
      if (!ReadFile(h, buf, sizeof buf, &n, nullptr) || n == 0) break;
      out.append(buf, n);
    }
    return out;
  }
  ~ProbePipe() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};

bool Starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Talks to the probe in the running child and checks every command against the
// fixtures the child reported.
bool ProbeLive(const Layout& L) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000) && WaitFile(L.dir + L"\\probe_info.txt", 10000),
                  "probe wrote its pipe name; child reported its fixtures");
  if (!ok) return false;
  std::string name, info, r;
  ReadText(L.Profile(L"sowprobe.txt"), &name);
  ReadText(L.dir + L"\\probe_info.txt", &info);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  unsigned long long marker = 0, insn = 0, target = 0, ptr = 0, heap = 0;
  sscanf_s(info.c_str(), "marker=%llX insn=%llX target=%llX ptr=%llX heap=%llX", &marker, &insn, &target, &ptr, &heap);
  ProbePipe p;
  ok &= Check(p.Open(std::wstring(name.begin(), name.end())), "connect to the probe pipe");
  if (!ok) return false;
  {
    std::string f = p.Ask("findq 5A5A1234ABCD0001");
    char a[48], b[48];
    snprintf(a, sizeof a, "abs:%llX\n", heap + 32 * 8);
    snprintf(b, sizeof b, "abs:%llX\n", heap + 100 * 8);
    ok &= Check(Starts(f, "ok hits=2 ") && f.find(a) != std::string::npos && f.find(b) != std::string::npos,
                "findq finds both heap copies of a value, and not its own stack copy");
  }
  for (int i = 0; i < 100; ++i) {
    r = p.Ask("ping");
    if (r.find("ready=1") != std::string::npos) break;
    Sleep(100);
  }
  printf("    %s", r.substr(0, r.find('\n') + 1).c_str());
  ok &= Check(r.find("ready=1") != std::string::npos, "ping reports ready (ready pattern seen)");
  r = p.Ask("module");
  ok &= Check(Starts(r, "ok base=") && r.find("section .text ") != std::string::npos, "module lists the sections");
  char rva[64];
  snprintf(rva, sizeof rva, "rva:%llX\n", marker);
  r = p.Ask("scan " + MarkerPattern());
  ok &= Check(Starts(r, "ok hits=1 ") && r.find(rva) != std::string::npos, "scan finds the marker once, at its RVA");
  std::string wild = MarkerPattern();
  wild.replace(9, 5, "? ??");  // bytes 3 and 4 become wildcards
  r = p.Ask("scan " + wild);
  ok &= Check(r.find(rva) != std::string::npos, "wildcard scan finds it too");
  std::string want_bytes;
  char b[4];
  for (size_t i = 0; i + 1 < sizeof kProbeMarker; ++i) {
    snprintf(b, sizeof b, "%02x", (unsigned char)kProbeMarker[i]);
    want_bytes += std::string(i ? " " : "") + b;
  }
  r = p.Ask(std::string("read ") + std::string(rva, strlen(rva) - 1) + " " + std::to_string(sizeof kProbeMarker - 1));
  ok &= Check(r.find(want_bytes + "\n") != std::string::npos, "read returns the marker bytes");
  char cmd[96], want[64];
  snprintf(cmd, sizeof cmd, "rip rva:%llX 3 7", insn);
  snprintf(want, sizeof want, "ok rva:%llX\n", target);
  r = p.Ask(cmd);
  ok &= Check(r == std::string(want) + "END\n", "rip resolves lea rax,[rip+disp] to its target");
  snprintf(cmd, sizeof cmd, "deref rva:%llX 0", ptr);
  r = p.Ask(cmd);
  ok &= Check(r.find("-> 1122334455667788\n") != std::string::npos, "deref follows a pointer to the known value");
  r = p.Ask("read abs:10 8");
  ok &= Check(Starts(r, "err unreadable"), "reading unmapped memory answers err");
  r = p.Ask("frobnicate");
  ok &= Check(Starts(r, "err unknown"), "unknown command answers err");
  r = p.Ask("ping");
  ok &= Check(Starts(r, "ok pid="), "probe still answers after errors");
  return ok;
}

// Debug-print hook and write command, against the fixtures in HookChild.
// want: "hooked", "refused" or "off" (probe_write=0).
bool HookLive(const Layout& L, const std::string& want) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000) && WaitFile(L.dir + L"\\probe_info.txt", 10000),
                  "probe wrote its pipe name; child reported its fixtures");
  if (!ok) return false;
  std::string name, info, r;
  ReadText(L.Profile(L"sowprobe.txt"), &name);
  ReadText(L.dir + L"\\probe_info.txt", &info);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  unsigned long long area = 0, target = 0;
  sscanf_s(info.c_str(), "area=%llX write=%llX", &area, &target);
  ProbePipe p;
  ok &= Check(p.Open(std::wstring(name.begin(), name.end())), "connect to the probe pipe");
  if (!ok) return false;
  for (int i = 0; i < 100; ++i) {
    r = p.Ask("ping");
    if (r.find("debug_print=" + want) != std::string::npos) break;
    Sleep(100);
  }
  printf("    %s", r.substr(0, r.find('\n') + 1).c_str());
  ok &= Check(r.find("debug_print=" + want) != std::string::npos, ("debug print state is " + want).c_str());
  char cmd[128];
  snprintf(cmd, sizeof cmd, "read rva:%llX 16", area);
  r = p.Ask(cmd);
  if (want == "hooked") {
    ok &= Check(r.find("\ne9 ") != std::string::npos, "stub now starts with jmp rel32 (no register touched)");
    ok &= Check(r.find(" cc cc cc cc cc cc cc 90 90 c3 cc\n") != std::string::npos,
                "rest of the 12 bytes int3; next function untouched");
  } else {
    // refused: the child changed byte 0 to 90 beforehand; off: the original stub
    const char* stub = want == "refused" ? "\n90 89 44 24 18 4c 89 4c 24 20 c3 cc 90 90 c3 cc\n"
                                         : "\n4c 89 44 24 18 4c 89 4c 24 20 c3 cc 90 90 c3 cc\n";
    ok &= Check(r.find(stub) != std::string::npos, "stub and next function left exactly as they were");
  }
  snprintf(cmd, sizeof cmd, "write rva:%llX 41 42 43 44 45 46 47 48", target);
  r = p.Ask(cmd);
  if (want == "off") {
    ok &= Check(Starts(r, "err write disabled"), "write refused without probe_write=1");
  } else {
    ok &= Check(Starts(r, "ok wrote 8"), "write accepted with probe_write=1");
    snprintf(cmd, sizeof cmd, "read rva:%llX 8", target);
    ok &= Check(p.Ask(cmd).find("41 42 43 44 45 46 47 48\n") != std::string::npos, "write reads back");
  }
  WriteText(L.dir + L"\\go", "");
  ok &= Check(WaitFile(L.dir + L"\\called", 20000), "child made its debug-print calls without crashing");
  std::string regs;
  ReadText(L.dir + L"\\regs.txt", &regs);
  printf("    registers across the call: %s\n", regs.c_str());
  ok &= Check(regs == "regs ok", "rax, rcx, rdx, r8-r11, xmm0-5 and the caller's home slots unchanged");
  std::string chan;
  ReadText(L.dir + L"\\chan.txt", &chan);
  ok &= Check(chan == "chan ok", want == "hooked" ? "debug_channels set exactly the listed flags"
                                                  : "debug_channels left alone without the hook");
  r = p.Ask("dbg 256");
  if (want != "hooked") return ok & Check(Starts(r, "err debug print"), "dbg answers err when not hooked");
  ok &= Check(Starts(r, "ok lines=1006 "), "1006 lines captured (5 single + 4 threads x 250 + register check)");
  // The ring keeps only the last 256 lines and the single calls came first, so read the file.
  std::string file;
  HANDLE f = CreateFileW(L.Profile(L"debugprint.log").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    std::vector<char> buf(1 << 20);
    DWORD got = 0;
    ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(f);
    file.assign(buf.data(), got);
  }
  for (const char* line : {"[TEST]: hello 42 world", "[TEST]: float 1.50 and 7", "[TEST]: many 1 2 3 4 5 6",
                           "[TEST]: <fault while formatting> bad pointer %s", "[TEST]: <bad format> count %n",
                           "[TEST]: regs r8=8888888888888888 r9=9999999999999999"})
    ok &= Check(file.find(line) != std::string::npos, line);
  r = p.Ask("dbgstats");
  ok &= Check(r.find("1000\t[LOOP]: ") != std::string::npos && r.find("6\t[TEST]: ") != std::string::npos,
              "per-category counts: 1000 [LOOP], 6 [TEST]");
  return ok;
}

// "player", "pad" and "save" against the stand-ins in PadSaveChild.
bool PadSaveLive(const Layout& L) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000), "probe wrote its pipe name");
  if (!ok) return false;
  std::string name, r;
  ReadText(L.Profile(L"sowprobe.txt"), &name);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  ProbePipe p;
  ok &= Check(p.Open(std::wstring(name.begin(), name.end())), "connect to the probe pipe");
  if (!ok) return false;
  for (int i = 0; i < 100 && r.find("ready=1") == std::string::npos; ++i) {
    r = p.Ask("ping");
    Sleep(100);
  }
  for (int i = 0; i < 100 && !LogHas(L, "now reads the virtual pad"); ++i) Sleep(50);
  r = p.Ask("save");
  ok &= Check(Starts(r, "err world not started"), "save refused before the world starts");
  WriteText(L.dir + L"\\go", "");
  for (int i = 0; i < 100 && !LogHas(L, "world start logged on thread"); ++i) Sleep(50);
  r = p.Ask("player");
  printf("    %s", r.c_str());
  ok &= Check(r.find("pos=1.500,-2.250,300.000 rot=0.0000,0.7071,0.0000,0.7071") != std::string::npos,
              "player: position and rotation from the stand-in player");
  ok &= Check(r.find("health=250.00 max=500.00 frac=0.500") != std::string::npos,
              "player: health, max and fraction from the stand-in character");
  r = p.Ask("pad 1000 0 32767 0 0 400");
  ok &= Check(Starts(r, "ok holding for 400 ms"), "pad accepted");
  Sleep(700);
  r = p.Ask("save");
  ok &= Check(Starts(r, "ok Player.Save queued"), "save queued once the world has started");
  for (int i = 0; i < 100 && !LogHas(L, "Player.Save returned"); ++i) Sleep(50);
  return ok;
}

// coop_send -> coop_listen over loopback, shown through the stand-in effect.
bool CoopLive(const Layout& L) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000), "probe wrote its pipe name");
  if (!ok) return false;
  std::string name, r;
  ReadText(L.Profile(L"sowprobe.txt"), &name);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  ProbePipe p;
  ok &= Check(p.Open(std::wstring(name.begin(), name.end())), "connect to the probe pipe");
  if (!ok) return false;
  for (int i = 0; i < 100; ++i) {
    r = p.Ask("remote");
    if (Starts(r, "ok packets=") && !Starts(r, "ok packets=0") && atoi(r.c_str() + 11) >= 10) break;
    Sleep(100);
  }
  printf("    %s", r.c_str());
  ok &= Check(r.find("lost=0 bad=0") != std::string::npos && r.find("present=1 pos=1.500,-2.250,300.000") != std::string::npos &&
                  r.find("rot=0.0000,0.7071,0.0000,0.7071 health=250.00 max=500.00") != std::string::npos,
              "remote: packets arrive over loopback with the stand-in player's position, rotation, health");
  ok &= Check(r.find("shown=0") != std::string::npos, "nothing shown before the world starts");
  WriteText(L.dir + L"\\go", "");
  for (int i = 0; i < 100 && !LogHas(L, "last-known-position effect #1 at"); ++i) Sleep(50);
  wchar_t vt[32] = {};
  GetPrivateProfileStringW(L"sowguard", L"test_vtable_rva", L"", vt, 32, (L.dir + L"\\sowguard.ini").c_str());
  std::string vts;
  for (const wchar_t* c = vt; *c; ++c) vts += (char)*c;  // hex digits
  r = p.Ask("count " + vts + " 2 1 400");
  printf("    %s", r.c_str());
  ok &= Check(r.find("slot 2 ") != std::string::npos && r.find("(simulation)") != std::string::npos,
              "count: slot 2 is called on the simulation thread");
  r = p.Ask("count " + vts + " 1 1 100");
  ok &= Check(Starts(r, "err slot 1 does not point into the game's code"), "count refuses the hooked slot 1");
  r = p.Ask("lkp 10 20 30");
  ok &= Check(Starts(r, "ok last-known-position effect queued"), "manual lkp queued");
  for (int i = 0; i < 100 && !LogHas(L, "effect #2 at 10.0,20.0,30.0"); ++i) Sleep(50);
  WriteText(L.dir + L"\\move", "");
  for (int i = 0; i < 100 && !LogHas(L, "effect #3 at 101.5"); ++i) Sleep(50);
  Sleep(600);  // no further shows while the player stands still
  return ok;
}

// coop_show=puppet: the nearest stand-in character is chosen and teleported to the
// received player on the simulation thread.
bool PuppetLive(const Layout& L) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000), "probe wrote its pipe name");
  if (!ok) return false;
  std::string name, r;
  ReadText(L.Profile(L"sowprobe.txt"), &name);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  ProbePipe p;
  ok &= Check(p.Open(std::wstring(name.begin(), name.end())), "connect to the probe pipe");
  if (!ok) return false;
  for (int i = 0; i < 100 && !LogHas(L, "coop: first packet"); ++i) Sleep(50);
  r = p.Ask("puppet");
  ok &= Check(Starts(r, "ok puppet none"), "no puppet before the world starts");
  WriteText(L.dir + L"\\go", "");
  for (int i = 0; i < 200 && !LogHas(L, "sim: puppet move #3"); ++i) Sleep(50);
  r = p.Ask("remote");
  printf("    %s", r.c_str());
  ok &= Check(r.find(" puppet abs:") != std::string::npos, "remote shows the puppet");
  ok &= Check(LogHas(L, "coop: puppet abs:") && LogHas(L, "distance=10.0"), "the stand-in AI 10 units away was picked");
  WriteText(L.dir + L"\\move", "");
  Sleep(5000);  // 200 frames (Sleep(5) is ~15.6 ms each), plus the 50 ms delay
  ok &= Check(LogHas(L, "puppet moved every frame, 50 ms behind the sender"), "log: per-frame interpolation");
  r = p.Ask("puppet off");
  ok &= Check(Starts(r, "ok puppet none"), "puppet off");
  return ok;
}

// probe "stacks": wrong or too few entry bytes are refused; filtered by the first
// argument only caller A's stacks are recorded, unfiltered both callers'.
bool StacksLive(const Layout& L) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000), "probe wrote its pipe name");
  if (!ok) return false;
  std::string name, r;
  ReadText(L.Profile(L"sowprobe.txt"), &name);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  ProbePipe p;
  ok &= Check(p.Open(std::wstring(name.begin(), name.end())), "connect to the probe pipe");
  if (!ok) return false;
  auto key = [&](const wchar_t* k) {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"sowguard", k, L"", v, 32, (L.dir + L"\\sowguard.ini").c_str());
    std::string text;
    for (const wchar_t* c = v; *c; ++c) text += (char)*c;  // hex digits
    return text;
  };
  const std::string f = key(L"test_stacks_rva");
  const uintptr_t a = strtoull(key(L"test_caller_a_rva").c_str(), nullptr, 16);
  const uintptr_t b = strtoull(key(L"test_caller_b_rva").c_str(), nullptr, 16);
  // Frames whose rva lies in [lo, hi): a caller's body, cut off where the other one starts.
  auto frames_in = [](const std::string& text, uintptr_t lo, uintptr_t other) {
    const uintptr_t hi = other > lo ? std::min<uintptr_t>(lo + 0x40, other) : lo + 0x40;
    size_t n = 0, pos = 0;
    while ((pos = text.find(" rva:", pos)) != std::string::npos) {
      const uintptr_t v = strtoull(text.c_str() + pos + 5, nullptr, 16);
      if (v >= lo && v < hi) ++n;
      pos += 5;
    }
    return n;
  };
  r = p.Ask("stacks rva:" + f + " 4883EC28C74168010000FF 100");
  ok &= Check(Starts(r, "err the entry bytes differ"), "stacks refuses entry bytes that differ");
  r = p.Ask("stacks rva:" + f + " 4883EC28 100");
  ok &= Check(Starts(r, "err usage: stacks"), "stacks refuses fewer than 5 entry bytes");
  r = p.Ask("stacks rva:" + f + " 4883EC28C7416801000000 600 5A5A0001");
  printf("    %s", r.c_str());
  const size_t m = r.find(" matched=");
  ok &= Check(Starts(r, "ok calls=") && m != std::string::npos && atoi(r.c_str() + m + 9) > 0,
              "stacks: calls recorded while hooked");
  ok &= Check(frames_in(r, a, b) > 0 && frames_in(r, b, a) == 0,
              "filtered by the first argument: only caller A's stacks");
  r = p.Ask("stacks rva:" + f + " 4883EC28C7416801000000 300");
  ok &= Check(frames_in(r, a, b) > 0 && frames_in(r, b, a) > 0, "unfiltered: both callers' stacks");
  ok &= Check(LogHas(L, "probe: recording call stacks at rva:"), "log: recording");
  return ok;
}

// coop_puppet_ai=off, coop_puppet_move=both: the behavior-tree call is filtered once the
// world starts; while the stand-in walks, the puppet is pinned and asked to walk, then run;
// when it stops, the controller goes idle. Then "teleport" and "puppet ai on".
// coop_puppet_recv: the receiver alone. The test sends version-5 packets (the layout in
// coop.cpp) with two animation inputs, a root-motion speed of 600 units/s (a free-running
// sender) and two game actions (the A button, 110, held; the stick, 3, at 0.91) to the child's
// coop_listen port for 5 s. From 3.9 to 4.35 s the sender is off the ground on an arc up to
// ~100 units (below coop_anim_snap's 150 up or down): in the air (locomotion mode 2) for the
// first 30 ms, then on a wall (mode 3, on-ground flag 0), as in a climb: the puppet must
// follow it there. At 4.6 s the child "frees" the puppet (as the game does with a
// stand-in it unloads), or with coop_puppet_actions=off kills it: the guard must drop it.
#pragma pack(push, 1)
struct TestPacket {
  char magic[4];
  uint16_t version, flags;
  uint32_t seq;
  uint8_t move_mode, on_ground, reserved[2];
  uint64_t time_us;
  float pos[3], rot[4], health, max_health;
  uint32_t input_mask;
  float inputs[25];
  float root_speed;
  uint32_t action_count;
  struct {
    uint16_t index;
    uint8_t held, pressed;
    float value;
  } actions[48];
  uint32_t attack_id, attack_stamp;
};
#pragma pack(pop)
static_assert(sizeof(TestPacket) == 564, "coop packet version 8");

bool RecvLive(const Layout& L) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000), "probe wrote its pipe name");
  const unsigned port = GetPrivateProfileIntW(L"sowguard", L"coop_listen", 0, (L.dir + L"\\sowguard.ini").c_str());
  ok &= Check(port != 0, "the child chose a listen port");
  if (!ok) return false;
  WriteText(L.dir + L"\\go", "");
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons((u_short)port);
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  TestPacket pk{};
  memcpy(pk.magic, "SOWC", 4);
  pk.version = 8;
  pk.on_ground = 1;
  pk.flags = 1;
  pk.root_speed = 600.0f;
  pk.action_count = 2;
  pk.actions[0] = {110, 1, 0, 1.0f};
  pk.actions[1] = {3, 1, 0, 0.91f};
  const float pos[3] = {11.5f, -2.25f, 300.0f}, rot[4] = {0, 0, 0, 1};
  memcpy(pk.pos, pos, sizeof pos);
  memcpy(pk.rot, rot, sizeof rot);
  pk.health = pk.max_health = 300.0f;
  pk.input_mask = 1u | (1u << 4);  // RawMoveMag, PlayerVelocity
  pk.inputs[0] = 0.6f;
  pk.inputs[4] = 150.0f;
  // At 1.5 s the sender jumps 500 units (a put-back past coop_anim_snap); at 2.5 s it lets go
  // of the stick, jumps 60 more and creeps on at 20 units/s from then on (never past
  // coop_anim_snap), as a player does for a second or more after stopping: the puppet must
  // still be put back.
  bool jumped = false, creeping = false, freed = false, airborne = false;
  float creep_x0 = 0;
  ULONGLONG creep_t0 = 0;
  for (const ULONGLONG t0 = GetTickCount64(); GetTickCount64() - t0 < 5000;) {
    if (!freed && GetTickCount64() - t0 >= 4600) {
      wchar_t actions[8] = {};
      GetPrivateProfileStringW(L"sowguard", L"coop_puppet_actions", L"", actions, 8, (L.dir + L"\\sowguard.ini").c_str());
      WriteText(L.dir + (wcscmp(actions, L"off") == 0 ? L"\\dead" : L"\\free"), "");
      freed = true;
    }
    if (!jumped && GetTickCount64() - t0 >= 1500) {
      WriteText(L.dir + L"\\move", "");  // the child notes how many teleports came before this
      Sleep(50);
      pk.pos[0] += 500.0f;
      jumped = true;
    }
    if (!creeping && GetTickCount64() - t0 >= 2500) {
      WriteText(L.dir + L"\\creep", "");
      Sleep(50);
      pk.inputs[0] = 0.02f;  // RawMoveMag: stick let go
      pk.pos[0] += 60.0f;
      creep_x0 = pk.pos[0];
      creep_t0 = GetTickCount64();
      creeping = true;
    } else if (creeping) {
      pk.pos[0] = creep_x0 + 20.0f * (GetTickCount64() - creep_t0) / 1000.0f;
    }
    const float s = (GetTickCount64() - t0) / 1000.0f - 3.9f;
    if (s >= 0.0f && s < 0.45f) {
      if (!airborne) WriteText(L.dir + L"\\air", "");
      airborne = true;
      pk.move_mode = s < 0.03f ? 2 : 3;
      pk.on_ground = s < 0.03f ? 1 : 0;
      pk.pos[1] = -2.25f + 2000.0f * s * (0.45f - s);
    } else {
      if (airborne && !Exists(L.dir + L"\\airend")) WriteText(L.dir + L"\\airend", "");
      pk.move_mode = 1;
      pk.on_ground = 1;
      pk.pos[1] = -2.25f;
    }
    // The sender attacks from 3.0 to 3.4 s, then the same attack again (a new start) from 3.55
    // to 3.75 s: the puppet must start it twice and stop it twice.
    const ULONGLONG ms = GetTickCount64() - t0;
    const bool attacking = (ms >= 3000 && ms < 3400) || (ms >= 3550 && ms < 3750);
    pk.attack_id = attacking ? 0x221FBF97u : 0;
    pk.attack_stamp = attacking ? (ms < 3400 ? 3000u : 3550u) : 0;
    ++pk.seq;
    QueryPerformanceCounter(&c);
    pk.time_us = (uint64_t)(c.QuadPart * 1000000 / f.QuadPart);
    sendto(sock, reinterpret_cast<const char*>(&pk), sizeof pk, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    Sleep(16);
  }
  closesocket(sock);
  Sleep(300);
  return Check(LogHas(L, "coop: the puppet gets the sender's animation inputs"), "log: the puppet gets the inputs");
}

bool WalkLive(const Layout& L) {
  bool ok = Check(WaitFile(L.Profile(L"sowprobe.txt"), 10000), "probe wrote its pipe name");
  if (!ok) return false;
  std::string name, r;
  ReadText(L.Profile(L"sowprobe.txt"), &name);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  ProbePipe p;
  ok &= Check(p.Open(std::wstring(name.begin(), name.end())), "connect to the probe pipe");
  if (!ok) return false;
  for (int i = 0; i < 100 && !LogHas(L, "coop: first packet"); ++i) Sleep(50);
  WriteText(L.dir + L"\\go", "");
  for (int i = 0; i < 200 && !(LogHas(L, "goes through the puppet filter") && LogHas(L, "sim: puppet move #3")); ++i)
    Sleep(50);
  r = p.Ask("puppet");
  printf("    %s", r.c_str());
  ok &= Check(r.find(" ai=off ") != std::string::npos && r.find("tree_hook=in") != std::string::npos,
              "puppet AI off: the behavior-tree call goes through the filter");
  WriteText(L.dir + L"\\move", "");
  Sleep(3500);  // 2.5 s of walking, then standing still
  r = p.Ask("puppet");
  printf("    %s", r.c_str());
  const size_t i = r.find(" idles=");
  ok &= Check(i != std::string::npos && atoi(r.c_str() + i + 7) >= 1, "the puppet was sent idle once the sender stopped");
  r = p.Ask("teleport 7 8 9");
  ok &= Check(Starts(r, "ok player teleport queued"), "teleport queued");
  r = p.Ask("puppet ai on");
  ok &= Check(Starts(r, "ok puppet ai on"), "puppet ai on");
  WriteText(L.dir + L"\\aion", "");
  for (int k = 0; k < 40 && !LogHas(L, "player teleported to 7.0,8.0,9.0"); ++k) Sleep(50);
  Sleep(500);
  return ok;
}

int RunCases(const Env& env, const std::vector<Case>& cases, bool expect_failures) {
  int passed = 0, failed = 0;
  for (const auto& c : cases) {
    Layout L(env.root + L"\\" + c.name);
    PrepareCase(env, L, c);
    std::map<std::wstring, std::string> before, after;
    Snapshot(L.dir + L"\\owner", L"", &before);
    wprintf(L"[%s]\n", c.name.c_str());
    fflush(stdout);
    bool live_ok = true;
    DWORD code = RunChild(L, c.child.empty() ? c.name : c.child, env.exe_to_hash, c.live, &live_ok);
    Snapshot(L.dir + L"\\owner", L"", &after);
    bool ok = live_ok;
    ok &= Check(code == c.expect, "exit code");
    if (code != c.expect) printf("    got 0x%08lX, want 0x%08lX\n", code, c.expect);
    ok &= Check(before == after, "fake owner tree unchanged");
    if (c.post) ok &= c.post(L);
    bool counted_pass = expect_failures ? !ok : ok;
    printf("  => %s%s\n", ok ? "PASS" : "FAIL", expect_failures ? (ok ? " (mutant survived!)" : " (mutant caught)") : "");
    (counted_pass ? passed : failed)++;
  }
  printf("\nRESULT: %s (%d of %zu %s)\n", failed == 0 ? "PASS" : "FAIL", passed, cases.size(),
         expect_failures ? "mutant cases caught" : "cases passed");
  return failed == 0 ? 0 : 1;
}

std::vector<Case> AllCases(const Env& env) {
  auto no_file = [](const std::wstring& p) { return !Exists(p); };
  std::vector<Case> v;
  v.push_back({L"redirect_write", L"", 0, {}, [=](const Layout& L) {
                 return Check(FileIs(L.PSow(L"new.sav"), "NEW"), "new.sav landed in the profile") &
                        Check(no_file(L.sow + L"\\new.sav"), "not in the owner folder");
               }});
  v.push_back({L"redirect_read", L"", 0, {}, nullptr});
  v.push_back({L"path_forms", L"", 0, {}, [=](const Layout& L) {
                 bool ok = true;
                 for (int i = 1; i <= 5; ++i) {
                   std::wstring f = L"f" + std::to_wstring(i) + L".sav";
                   ok &= FileIs(L.PSow(f), std::to_string(i)) && no_file(L.sow + L"\\" + f);
                 }
                 return Check(ok, "f1..f5 all in the profile, none in the owner folder");
               }});
  v.push_back({L"rename_delete", L"", 0, {}, [=](const Layout& L) {
                 return Check(FileIs(L.PSow(L"b.sav"), "A") && no_file(L.PSow(L"a.sav")), "rename happened in the profile") &
                        Check(no_file(L.PSow(L"c.sav")), "delete happened in the profile") &
                        Check(Exists(L.PSow(L"keep")) && no_file(L.PSow(L"sub")), "directories in the profile");
               }});
  v.push_back({L"steam_remote", L"", 0, {}, [=](const Layout& L) {
                 return Check(FileIs(L.Profile(L"steam-userdata\\356190\\remote\\new.sav"), "STEAM"),
                              "Steam userdata write landed in the profile");
               }});
  v.push_back({L"install_read", L"", 0, {}, nullptr});
  auto fatal_logged = [](const Layout& L) { return Check(LogHas(L, "PROTECTED write blocked"), "log names the blocked write"); };
  v.push_back({L"guard_install_write", L"", kExitProtected, {}, fatal_logged});
  v.push_back({L"guard_install_delete", L"", kExitProtected, {}, fatal_logged});
  v.push_back({L"guard_create_in_install", L"", kExitProtected, {}, [=](const Layout& L) {
                 return fatal_logged(L) & Check(no_file(L.install + L"\\new.bin"), "no file created");
               }});
  v.push_back({L"guard_rename_into_install", L"", kExitProtected, {}, [=](const Layout& L) {
                 return fatal_logged(L) & Check(no_file(L.install + L"\\evil.bin"), "nothing renamed into the install");
               }});
  v.push_back({L"network_offline", L"", 0, {}, [=](const Layout& L) {
                 return Check(LogHas(L, "offline: connect to 192.0.2.1:80 refused"), "log names the refused connect");
               }});
  v.push_back({L"network_deny_sockets", L"network_blocked", 0, {{L"network", L"deny_sockets"}}, nullptr});
  v.push_back({L"network_allowed", L"", 0, {{L"network", L"allow"}}, nullptr});
  v.push_back({L"outside_root", L"load_only", kExitOutsideRoot, {{L"allowed_root", env.root + L"\\elsewhere"}}, nullptr});
  v.push_back({L"exe_identity", L"load_only", kExitExeIdentity, {{L"check_exe", L"1"}}, nullptr});
  v.push_back({L"steam_appid_missing", L"load_only", kExitSteamAppId, {{L"check_steam", L"1"}}, nullptr});
  Case appid{L"steam_appid_ok", L"", 0, {{L"check_steam", L"1"}}, nullptr};
  appid.seed_appid = true;
  v.push_back(appid);
  wchar_t real_local[1024] = {};
  GetEnvironmentVariableW(L"LOCALAPPDATA", real_local, 1024);
  v.push_back({L"real_paths_refused", L"load_only", kExitConfig, {{L"owner_localappdata", real_local}}, nullptr});
  Case no_ini{L"no_ini", L"load_only", kExitConfig, {}, nullptr};
  no_ini.drop_ini = true;
  v.push_back(no_ini);
  std::string marker = MarkerPattern();
  Case probe{L"probe_serve", L"", 0,
             {{L"probe", L"1"}, {L"probe_ready_pattern", std::wstring(marker.begin(), marker.end())},
              {L"probe_wait_sec", L"10"}},
             [=](const Layout& L) { return Check(LogHas(L, "probe: ready after"), "log records when it became ready"); }};
  probe.live = ProbeLive;
  v.push_back(probe);
  const std::wstring dp_site(kDpCallSitePattern, kDpCallSitePattern + strlen(kDpCallSitePattern));
  const std::vector<std::pair<std::wstring, std::wstring>> hook_ini = {
      {L"probe", L"1"}, {L"probe_ready_pattern", std::wstring(marker.begin(), marker.end())},
      {L"probe_wait_sec", L"10"}, {L"probe_write", L"1"}, {L"hook_debug_print", L"1"},
      {L"debug_print_pattern", dp_site}};
  Case hooked{L"probe_hook", L"", 0, hook_ini, nullptr};
  hooked.live = [](const Layout& L) { return HookLive(L, "hooked"); };
  v.push_back(hooked);
  Case refused{L"probe_hook_refused", L"", 0, hook_ini, [=](const Layout& L) {
                 return Check(LogHas(L, "does not look like the known 12-byte stub"), "log says why it refused");
               }};
  refused.live = [](const Layout& L) { return HookLive(L, "refused"); };
  v.push_back(refused);
  auto no_write_ini = hook_ini;
  no_write_ini[3].second = L"0";  // probe_write=0
  Case write_off{L"probe_write_off", L"", 0, no_write_ini, [=](const Layout& L) {
                   return Check(LogHas(L, "needs probe_write=1"), "log says the hook needs probe_write");
                 }};
  write_off.live = [](const Layout& L) { return HookLive(L, "off"); };
  v.push_back(write_off);
  Case steam{L"steam_block", L"", 0, {{L"steam_trace", L"1"}}, [=](const Layout& L) {
               std::string r;
               ReadText(L.dir + L"\\steam.txt", &r);
               printf("    %s\n", r.c_str());
               return Check(r == "steam ok", "every write method answered by the guard, none reached Steam; reads pass") &
                      Check(LogHas(L, "remote-storage writes blocked (12 methods"), "log: guard installed after Steam init") &
                      Check(LogHas(L, "blocked FileWrite('save.sav', 5 bytes)"), "log names the blocked FileWrite") &
                      Check(LogHas(L, "user-stats writes blocked (10 methods"), "log: user-stats guard installed too") &
                      Check(LogHas(L, "blocked SetAchievement('ACH_TEST')"), "log names the blocked achievement") &
                      Check(LogHas(L, "blocked SetStat('ACH_TEST', 5)"), "log names the blocked stat and its value") &
                      Check(LogHas(L, "blocked SetAchievement('ACH_LATE')"), "achievement still logged after 400 stats") &
                      Check(!LogHas(L, "blocked SetStat('STAT_FLOOD', 399)"), "stat log stops at its budget") &
                      Check(LogHas(L, "remote-storage reads traced (10 methods"), "steam_trace: read methods wrapped") &
                      Check(LogHas(L, "read FileRead('save.sav') -> 7 of 5 bytes"), "steam_trace: FileRead logged, "
                                                                                     "Steam's answer passed on");
             }};
  steam.fake_steam = true;
  v.push_back(steam);
  Case cloud{L"steam_cloud", L"", 0, {{L"steam_cloud", L"sandbox"}}, [=](const Layout& L) {
               std::string r;
               ReadText(L.dir + L"\\cloud.txt", &r);
               printf("    %s\n", r.c_str());
               const std::wstring dir = L.Profile(L"steam-userdata\\356190");
               return Check(r == "cloud ok", "cloud calls answered from the sandbox folder, none reached Steam") &
                      Check(!Exists(dir + L"\\escape.sav") && !Exists(L.Profile(L"steam-userdata\\escape.sav")),
                            "names with .. or a drive never leave the folder") &
                      Check(LogHas(L, "sandbox cloud FileWrite('\\save.sav') -> 5 bytes written"), "log names the write") &
                      Check(LogHas(L, "sandbox cloud FileRead('\\save.sav') -> 5 of 16 bytes"), "log names the read");
             }};
  cloud.fake_steam = true;
  v.push_back(cloud);
  Case steam_bad{L"steam_wrapper_mismatch", L"", kExitSteamGuard, {}, [=](const Layout& L) {
                   return Check(LogHas(L, "is not the expected one-slot wrapper"), "log says which wrapper was wrong");
                 }};
  steam_bad.fake_steam = true;
  v.push_back(steam_bad);
  v.push_back({L"probe_off", L"load_only", 0, {}, [=](const Layout& L) {
                 return Check(!Exists(L.Profile(L"sowprobe.txt")), "no probe without probe=1");
               }});
  const std::vector<std::pair<std::wstring, std::wstring>> pad_ini = {
      {L"autoload", L"1"},          {L"autoload_iat_section", L".rdata"}, {L"autoload_window_class", L"SowGuardTestPad"},
      {L"autoload_first_ms", L"300"}, {L"autoload_every_ms", L"400"},     {L"autoload_max_presses", L"4"},
      {L"autoload_hold_ms", L"150"}};
  auto pad_report = [](const Layout& L, const char* want, const char* log_line, const char* what) {
    std::string r;
    ReadText(L.dir + L"\\pad.txt", &r);
    printf("    %s\n", r.c_str());
    return Check(r == want, what) & Check(LogHas(L, "XInputGetState slot rva:"), "log: import slot found and patched") &
           Check(LogHas(L, log_line), log_line);
  };
  v.push_back({L"pad_load", L"", 0, pad_ini, [=](const Layout& L) {
                 return pad_report(L, "presses=1 connected=1 packets=1 other=1", "the game reports the world starting",
                                   "one A press, then the world-start line stops the pressing; pad always connected");
               }});
  auto pad_save_ini = pad_ini;
  for (auto& kv : pad_save_ini)
    if (kv.first == L"autoload_first_ms") kv.second = L"60000";  // no A presses during this case
  pad_save_ini.push_back({L"probe", L"1"});
  pad_save_ini.push_back({L"probe_write", L"1"});
  pad_save_ini.push_back({L"probe_ready_pattern", std::wstring(marker.begin(), marker.end())});
  pad_save_ini.push_back({L"probe_wait_sec", L"10"});
  Case pad_save{L"probe_pad_save", L"", 0, pad_save_ini, [=](const Layout& L) {
                  std::string r;
                  ReadText(L.dir + L"\\padsave.txt", &r);
                  printf("    %s\n", r.c_str());
                  return Check(r == "hold=1 saves=1 player_ok=1",
                               "the pad held A and the left stick; the save handler ran once, with the player") &
                         Check(LogHas(L, "running Player.Save on the pad thread"), "log: ran from the pad read");
                }};
  pad_save.live = PadSaveLive;
  v.push_back(pad_save);
  auto coop_ini = pad_save_ini;
  coop_ini.push_back({L"coop_show", L"lkp"});
  Case coop{L"coop_loop", L"", 0, coop_ini, [=](const Layout& L) {
              std::string r;
              ReadText(L.dir + L"\\coop.txt", &r);
              printf("    %s\n", r.c_str());
              return Check(r == "calls=3 player_ok=1 on_sim=1 vtable_restored=1 "
                                "seen=1.50,-2.25,300.00;10.00,20.00,30.00;101.50,-2.25,300.00",
                           "effect shown on the simulation thread at the received position, the manual one, and "
                           "again only after a move; counted slot restored") &
                     Check(LogHas(L, "coop: first packet"), "log: first packet received") &
                     Check(LogHas(L, "cannot show the remote player yet: world not started"),
                           "log: waits for the world start");
            }};
  coop.live = CoopLive;
  v.push_back(coop);
  auto puppet_ini = pad_save_ini;
  puppet_ini.push_back({L"coop_show", L"puppet"});
  Case puppet{L"coop_puppet", L"", 0, puppet_ini, [=](const Layout& L) {
                std::string r;
                ReadText(L.dir + L"\\puppet.txt", &r);
                printf("    %s\n", r.c_str());
                std::string m;
                ReadText(L.dir + L"\\smooth.txt", &m);
                printf("    %s\n", m.c_str());
                return Check(r == "teleports>0=1 object_ok=1 on_sim=1 xform=101.50,-2.25,300.00,0.0000,0.7071,0.0000,0.7071",
                             "the puppet was teleported on the simulation thread, ending at the walked-to position "
                             "with the received rotation") &
                       Check(m == "walked=200 monotonic=1 max_step_le_1=1 last_x=101.50",
                             "while the stand-in walked, the puppet moved forward in small steps (no jumps)");
              }};
  puppet.live = PuppetLive;
  v.push_back(puppet);
  auto walk_ini = puppet_ini;
  walk_ini.push_back({L"coop_puppet_ai", L"off"});
  walk_ini.push_back({L"coop_puppet_move", L"both"});
  walk_ini.push_back({L"coop_run_speed", L"250"});
  Case walk{L"coop_puppet_walk", L"", 0, walk_ini, [=](const Layout& L) {
              std::string r;
              ReadText(L.dir + L"\\walk.txt", &r);
              printf("    %s\n", r.c_str());
              return Check(r.rfind("puppet_tree_while_off=0 other_tree>0=1 puppet_tree_after_on>0=1 ", 0) == 0,
                           "while its AI was off the puppet's behavior tree never ran, another character's did, "
                           "and it ran again after 'puppet ai on'") &
                     Check(r.find("requests>0=1 bad=0 types=3 leads=1 controller_ok=1 idle_mode=2 pinned=1 ") !=
                               std::string::npos,
                           "move requests went to the puppet's controller (destination, navigator, factor 1), "
                           "walking then running, aimed ahead; idle at the end; pinned to the sender") &
                     Check(r.find("player_tp=7.00,8.00,9.00,0.0000,0.7071,0.0000,0.7071") != std::string::npos,
                           "probe teleport moved the player and kept its rotation") &
                     Check(r.find("inputs>0=1 movemag=0.75 velocity=210.0 axis=-0.50 stray=0") != std::string::npos,
                           "the sender's animation inputs reached the puppet's animation object, nothing else") &
                     Check(r.find("steps>0=1 step_speed=200") != std::string::npos,
                           "outside anim mode the puppet keeps its own root-motion speed") &
                     Check(r.find("actions=377 held110=0 stick=0.00 flags=11") != std::string::npos,
                           "a sender without buttons gives the puppet full, empty action lists and the "
                           "player flags") &
                     Check(r.find("player_actions=0 player_flags=00") != std::string::npos,
                           "sending and receiving in one copy (two-way): only the puppet's action lists and "
                           "flags are written, never this copy's own player's") &
                     Check(r.find(" attack_starts=1 attack_stops=1 attack_bad=0") != std::string::npos,
                           "the attack this copy's player was in was sent, started on the puppet through the "
                           "game's start (by its name hash) and stopped when the player's ended");
            }};
  walk.live = WalkLive;
  v.push_back(walk);
  auto recv_ini = puppet_ini;
  recv_ini.push_back({L"coop_puppet_move", L"anim"});
  Case recv{L"coop_puppet_recv", L"", 0, recv_ini, [=](const Layout& L) {
              std::string r;
              ReadText(L.dir + L"\\walk.txt", &r);
              printf("    %s\n", r.c_str());
              return Check(r.find("inputs>0=1 movemag=0.02 velocity=150.0 axis=0.00 stray=0") != std::string::npos,
                           "a receiver that records nothing itself still sets the received inputs on the puppet") &
                     Check(r.find("creep_put_back=1 creep_tp_x>571=1") != std::string::npos,
                           "anim mode: put back while the sender creeps at 20 units/s with its stick let go") &
                     Check(r.find("actions=377 held110=1 stick=0.91 flags=11") != std::string::npos,
                           "the puppet's action lists are full and hold the sender's buttons, with the "
                           "player flags set (its graph reads them)") &
                     Check(r.find("teleports_before_move=0 last_tp_x=511.5") != std::string::npos,
                           "anim mode: not pinned while the puppet is where the sender is; put back once the "
                           "sender is 500 units away") &
                     Check(r.find("steps>0=1 step_speed=200") != std::string::npos,
                           "with the sender's buttons the puppet keeps its own root-motion speed") &
                     Check(r.find(" follower=1") != std::string::npos,
                           "the puppet is marked as the player's follower (out of the player's combat)") &
                     Check(r.find(" target_standin=0 target_other=1 target_after=1") != std::string::npos,
                           "the attack target check rejects the puppet and accepts others; once the "
                           "puppet is dropped it rejects nothing") &
                     Check(r.find(" air_y>50=1 air_steps_few=1 landed=1") != std::string::npos,
                           "while the sender is off the ground (in the air, then on a wall) the "
                           "puppet's own transform follows it up the arc (under the put-back distance) "
                           "with its movement step skipped; on landing it is teleported back onto the "
                           "ground") &
                     Check(r.find(" hit_standin=0 hit_other=1 hit_after=1") != std::string::npos,
                           "hits on the puppet are dropped, others go through; once the puppet is "
                           "dropped nothing is") &
                     Check(r.find(" aoe_standin=0 aoe_other=1 aoe_after=1") != std::string::npos,
                           "area ticks skip the puppet and evaluate others; once the puppet is dropped "
                           "they skip nothing") &
                     Check(r.find(" reset_standin=0 reset_other=1 reset_after=1") != std::string::npos,
                           "a teleport's animation reset skips the puppet's controller (put-backs keep its "
                           "animation) and runs on others; once the puppet is dropped it skips nothing") &
                     Check(r.find(" attack_starts=2 attack_stops=2 attack_bad=0") != std::string::npos,
                           "the sender's attack was started on the puppet and stopped when the sender's "
                           "ended, and started again when the sender repeated it") &
                     Check(LogHas(L, "is gone (unloaded or freed); no puppet now"),
                           "log: the freed puppet was dropped (so a new one is picked or spawned)");
            }};
  recv.live = RecvLive;
  v.push_back(recv);
  auto nobuttons_ini = recv_ini;
  nobuttons_ini.push_back({L"coop_puppet_actions", L"off"});
  nobuttons_ini.push_back({L"coop_puppet_follower", L"off"});
  nobuttons_ini.push_back({L"coop_puppet_targetable", L"on"});
  Case nobuttons{L"coop_puppet_recv_nobuttons", L"", 0, nobuttons_ini, [=](const Layout& L) {
                   std::string r;
                   ReadText(L.dir + L"\\walk.txt", &r);
                   printf("    %s\n", r.c_str());
                   return Check(r.find("actions=0 held110=0 stick=0.00 flags=00") != std::string::npos,
                                "coop_puppet_actions=off: the puppet's action lists and flags stay an AI's") &
                          Check(r.find("steps>0=1 step_speed=600") != std::string::npos,
                                "without the buttons, anim mode scales the puppet's own root motion (200 "
                                "units/s) to the sender's (600) before its movement step") &
                          Check(r.find(" follower=0") != std::string::npos,
                                "coop_puppet_follower=off: the puppet's follower byte stays 0") &
                          Check(r.find(" target_standin=1 target_other=1") != std::string::npos &&
                                    r.find(" hit_standin=1 hit_other=1") != std::string::npos,
                                "coop_puppet_targetable=on: attacks may pick and hit the puppet") &
                          Check(LogHas(L, "is dead (health 0); no puppet now"),
                                "log: the killed puppet was dropped (so a new one is picked or spawned)");
                 }};
  nobuttons.live = RecvLive;
  v.push_back(nobuttons);
  Case stacks{L"probe_stacks", L"", 0, pad_save_ini, [=](const Layout& L) {
                std::string r;
                ReadText(L.dir + L"\\stacks.txt", &r);
                printf("    %s\n", r.c_str());
                return Check(r == "rounds>0=1 bad=0 restored=1",
                             "the relocated entry kept working while hooked; the entry bytes are back");
              }};
  stacks.live = StacksLive;
  v.push_back(stacks);
  auto bg_ini = pad_ini;
  for (auto& kv : bg_ini)
    if (kv.first == L"autoload_first_ms") kv.second = L"60000";
  bg_ini.push_back({L"background_run", L"1"});
  v.push_back({L"background_run", L"", 0, bg_ini, [=](const Layout& L) {
                 std::string r;
                 ReadText(L.dir + L"\\background.txt", &r);
                 printf("    %s\n", r.c_str());
                 return Check(r == "inactive=0 active=1 killfocus=0 clip_guarded=1 setpos_guarded=1 own_focus=1",
                              "focus loss swallowed, activation passed, cursor imports guarded, the window "
                              "reports itself in front") &
                        Check(LogHas(L, "focus-loss message 0x1C swallowed"), "log: WM_ACTIVATEAPP swallowed");
               }});
  v.push_back({L"pad_max", L"", 0, pad_ini, [=](const Layout& L) {
                 return pad_report(L, "presses=4 connected=1 packets=1 other=1",
                                   "no world start reported after 4 presses",
                                   "without a world-start line, exactly the 4 presses allowed");
               }});
  v.push_back({L"forwarders", L"", 0, {}, nullptr});
  v.push_back({L"hash", L"", 0, {}, nullptr});
  return v;
}

}  // namespace

// The "original" Bink DLL must not be a sowguard build (easy to pass by mistake once
// the guard is staged as bink2w64.dll in a test copy).
bool IsSowguardBuild(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  std::vector<char> buf(GetFileSize(f, nullptr));
  DWORD got = 0;
  ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr);
  CloseHandle(f);
  return std::string(buf.data(), got).find("SowGuardSelfTest_HashFile") != std::string::npos;
}

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 5 && std::wstring(argv[1]) != L"case" && IsSowguardBuild(argv[4])) {
    fwprintf(stderr, L"%s is a sowguard build, not the original bink2w64.dll\n", argv[4]);
    return 64;
  }
  if (argc >= 4 && std::wstring(argv[1]) == L"case") return Child(argv[2], argv[3], argc >= 5 ? argv[4] : L"");
  if (argc >= 6 && std::wstring(argv[1]) == L"run") {
    Env env{argv[2], argv[3], argv[4], argv[5]};
    RemoveTree(env.root);
    MakeDirs(env.root);
    std::vector<Case> cases = AllCases(env);
    if (argc >= 7)
      cases.erase(std::remove_if(cases.begin(), cases.end(), [&](const Case& c) { return c.name != argv[6]; }),
                  cases.end());
    return RunCases(env, cases, false);
  }
  if (argc >= 5 && std::wstring(argv[1]) == L"probe") {
    // Diagnostic, not a test: trace the AFD requests behind each socket call.
    Env env{argv[2], argv[3], argv[4], L""};
    RemoveTree(env.root);
    MakeDirs(env.root);
    Case c{L"net_probe", L"", 0, {{L"network", L"allow"}, {L"net_trace", L"1"}}, nullptr};
    Layout L(env.root + L"\\net_probe");
    PrepareCase(env, L, c);
    DWORD code = RunChild(L, c.name, L"");
    printf("child exit 0x%08lX\n", code);
    return code == 0 ? 0 : 1;
  }
  if (argc >= 5 && std::wstring(argv[1]) == L"mutant") {
    // Negative control: a build with the redirect and guard compiled out must fail
    // the cases that depend on them.
    Env env{argv[2], argv[3], argv[4], L""};
    RemoveTree(env.root);
    MakeDirs(env.root);
    std::vector<Case> picked;
    for (auto& c : AllCases(env))
      if (c.name == L"redirect_write" || c.name == L"redirect_read" || c.name == L"guard_install_write" ||
          c.name == L"guard_rename_into_install" || c.name == L"steam_remote" ||
          c.name == L"network_offline" || c.name == L"probe_hook" ||
          c.name == L"steam_block" || c.name == L"pad_load" ||
          c.name == L"steam_cloud" || c.name == L"probe_pad_save" || c.name == L"coop_loop" ||
          c.name == L"background_run" || c.name == L"coop_puppet" || c.name == L"probe_stacks" ||
          c.name == L"coop_puppet_walk" || c.name == L"coop_puppet_recv" || c.name == L"coop_puppet_recv_nobuttons")
        picked.push_back(c);
    return RunCases(env, picked, true);
  }
  fwprintf(stderr, L"usage: see the comment at the top of test_guard.cpp\n");
  return 64;
}
