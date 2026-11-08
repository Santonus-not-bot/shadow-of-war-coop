// sowguard: keeps a Shadow of War test copy away from the owner's install and saves.
//
// Built as a bink2w64.dll proxy: every Bink export forwards to bink2w64_.dll (the
// original, renamed), see bink_forwards.h. The game imports bink2w64.dll statically, so
// this runs before the game's own code. On load it checks that it is inside a test
// copy, then hooks six ntdll file syscalls in this process only, to
//   - redirect the save folders (LocalAppData\WB Games, Documents\WB Games and Steam
//     userdata\<id>\356190) into a sandbox profile,
//   - terminate the game if anything writes to the owner's install, the owner's save
//     folders or the game's Steam userdata folder (fail closed),
//   - optionally deny sockets (\Device\Afd) so the copy stays offline,
// Config: sowguard.ini next to the DLL. Log: <profile>\sowguard.log.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include "bink_forwards.h"
#include "coop.h"
#include "input.h"
#include "probe.h"
#include "simhook.h"
#include "steamguard.h"

#pragma comment(lib, "advapi32.lib")

#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif

namespace {

constexpr DWORD kExitConfig = 0xC0DE0001;
constexpr DWORD kExitOutsideRoot = 0xC0DE0002;
constexpr DWORD kExitExeIdentity = 0xC0DE0003;
constexpr DWORD kExitSteamAppId = 0xC0DE0004;
constexpr DWORD kExitHook = 0xC0DE0005;
constexpr DWORD kExitProtected = 0xC0DE0010;

// Steam build 3095001 (see games/shadow-of-war/NOTES.md in coop-anything).
constexpr ULONGLONG kExeSize = 117732784;
constexpr const char* kExeSha256 = "ed0d95cabb033cd66aaa99d699374881846c3cb03ec345f1123d82d45e0bdaef";
constexpr const wchar_t* kAppId = L"356190";
constexpr size_t npos = std::wstring::npos;

// ---------------------------------------------------------------- SHA-256
// Own implementation so DllMain does not have to load bcrypt providers under the
// loader lock.
struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64] = {};
  size_t n = 0;
  uint64_t bits = 0;

  static uint32_t Ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

  void Block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = hh + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  void Update(const uint8_t* p, size_t len) {
    bits += (uint64_t)len * 8;
    while (len) {
      size_t take = std::min(64 - n, len);
      memcpy(buf + n, p, take);
      n += take; p += take; len -= take;
      if (n == 64) { Block(buf); n = 0; }
    }
  }

  std::string Hex() {
    uint64_t total = bits;
    uint8_t pad = 0x80, zero = 0;
    Update(&pad, 1);
    while (n != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(total >> (56 - 8 * i));
    Update(len, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
  }
};

bool HashFile(const std::wstring& path, std::string* hex, ULONGLONG* size) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  Sha256 sha;
  std::vector<uint8_t> buf(1 << 20);
  ULONGLONG total = 0;
  DWORD got = 0;
  while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
    sha.Update(buf.data(), got);
    total += got;
  }
  CloseHandle(f);
  *hex = sha.Hex();
  *size = total;
  return true;
}

// ---------------------------------------------------------------- state

struct Rule {
  std::wstring from;    // DOS path, e.g. C:\Users\x\AppData\Local\WB Games
  std::wstring suffix;  // when set: one wildcard component, then this (e.g. \356190)
  std::wstring to;      // redirect target; empty for protect and watch rules
  std::wstring fromU, suffixU;
};

enum class NetMode { kAllow, kOffline, kDenySockets, kCoopPeer };

struct State {
  std::wstring profile;
  NetMode net = NetMode::kOffline;
  bool net_trace = false;
  std::vector<Rule> redirects, protect, watch;
  std::vector<std::pair<std::wstring, std::wstring>> devices;  // \DEVICE\HARDDISKVOLUME3 -> C:
  HANDLE log = INVALID_HANDLE_VALUE;
  CRITICAL_SECTION log_lock;
  volatile LONG log_lines = 0;
  volatile LONG read_notes = 0;
  volatile LONG net_notes = 0;
  volatile uintptr_t peer_sock = 0;  // network=coop_peer: the mod's own socket
  volatile LONG peer_ip = 0, peer_port = 0, peer_any = 0, peer_notes = 0;
} g;

thread_local int t_depth = 0;

// Our own file calls (logging, GetFinalPathNameByHandle) pass through the hooks
// untouched.
struct Reentry {
  bool outer;
  Reentry() : outer(t_depth++ == 0) {}
  ~Reentry() { --t_depth; }
};

// ---------------------------------------------------------------- logging

void Log(const wchar_t* fmt, ...) {
  if (g.log == INVALID_HANDLE_VALUE) return;
  wchar_t msg[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
  va_end(ap);
  wchar_t line[2200];
  _snwprintf_s(line, _TRUNCATE, L"[%10llu] %s\r\n", GetTickCount64(), msg);
  char utf8[6600];
  int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, nullptr, nullptr);
  if (n <= 1) return;
  EnterCriticalSection(&g.log_lock);
  DWORD written = 0;
  WriteFile(g.log, utf8, (DWORD)(n - 1), &written, nullptr);
  LeaveCriticalSection(&g.log_lock);
}

bool LogBudget() { return InterlockedIncrement(&g.log_lines) <= 5000; }

[[noreturn]] void Fatal(DWORD code, const wchar_t* fmt, ...) {
  wchar_t msg[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
  va_end(ap);
  Log(L"FATAL 0x%08lX %s", code, msg);
  if (g.log != INVALID_HANDLE_VALUE) FlushFileBuffers(g.log);
  OutputDebugStringW(L"sowguard: FATAL ");
  OutputDebugStringW(msg);
  TerminateProcess(GetCurrentProcess(), code);
  ExitProcess(code);
}

// ---------------------------------------------------------------- paths

std::wstring Upper(const std::wstring& s) {
  if (s.empty()) return s;
  std::wstring out(s.size(), L'\0');
  int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.data(), (int)s.size(), out.data(),
                        (int)out.size(), nullptr, nullptr, 0);
  if (n != (int)s.size())
    for (size_t i = 0; i < s.size(); ++i) out[i] = (wchar_t)towupper(s[i]);
  return out;
}

std::wstring Slashes(std::wstring p) {
  for (auto& c : p)
    if (c == L'/') c = L'\\';
  return p;
}

std::wstring CleanDir(std::wstring p) {
  p = Slashes(std::move(p));
  while (p.size() > 3 && p.back() == L'\\') p.pop_back();
  return p;
}

bool IsAbsoluteDos(const std::wstring& p) {
  return p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
  if (name.empty()) return dir;
  if (!dir.empty() && dir.back() == L'\\') return dir + name;
  return dir + L'\\' + name;
}

bool StartsWithI(const std::wstring& s, const wchar_t* pre) {
  size_t n = wcslen(pre);
  return s.size() >= n && _wcsnicmp(s.c_str(), pre, n) == 0;
}

// Length of the match when p starts with pre on a path-component boundary, else npos.
size_t MatchPrefix(const std::wstring& p, const std::wstring& pre, size_t at = 0) {
  if (at > p.size() || p.size() - at < pre.size()) return npos;
  if (p.compare(at, pre.size(), pre) != 0) return npos;
  size_t end = at + pre.size();
  if (end != p.size() && p[end] != L'\\') return npos;
  return end;
}

size_t MatchRule(const std::wstring& pathU, const Rule& r) {
  size_t n = MatchPrefix(pathU, r.fromU);
  if (n == npos || r.suffixU.empty()) return n;
  if (n >= pathU.size()) return npos;
  size_t slash = pathU.find(L'\\', n + 1);
  if (slash == npos || slash == n + 1) return npos;
  return MatchPrefix(pathU, r.suffixU, slash);
}

// DOS form ("C:\dir\file") of an NT object name, or "" when it is not a local disk path.
std::wstring NtToDos(const std::wstring& nt) {
  static const wchar_t* const kPrefixes[] = {L"\\??\\", L"\\DosDevices\\", L"\\GLOBAL??\\"};
  for (const wchar_t* pre : kPrefixes) {
    if (!StartsWithI(nt, pre)) continue;
    std::wstring rest = Slashes(nt.substr(wcslen(pre)));
    if (rest.size() >= 2 && iswalpha(rest[0]) && rest[1] == L':' && (rest.size() == 2 || rest[2] == L'\\'))
      return rest;
    return L"";
  }
  std::wstring u = Upper(nt);
  for (const auto& dev : g.devices) {
    size_t n = MatchPrefix(u, dev.first);
    if (n != npos) return dev.second + Slashes(nt.substr(n));
  }
  return L"";
}

std::wstring HandleDos(HANDLE h) {
  wchar_t buf[1024];
  DWORD n = GetFinalPathNameByHandleW(h, buf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 1024) return L"";
  std::wstring s(buf, n);
  if (StartsWithI(s, L"\\\\?\\UNC\\")) return L"";
  if (StartsWithI(s, L"\\\\?\\")) s = s.substr(4);
  return s;
}

std::wstring OaName(const OBJECT_ATTRIBUTES* oa) {
  if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer) return L"";
  return std::wstring(oa->ObjectName->Buffer, oa->ObjectName->Length / sizeof(wchar_t));
}

std::wstring OaDos(const OBJECT_ATTRIBUTES* oa) {
  if (!oa) return L"";
  std::wstring name = OaName(oa);
  if (oa->RootDirectory) {
    std::wstring root = HandleDos(oa->RootDirectory);
    return root.empty() ? L"" : Join(root, Slashes(name));
  }
  return NtToDos(name);
}

// ---------------------------------------------------------------- decisions

struct Decision {
  bool redirect = false;
  std::wstring target;  // DOS path when redirect
};

void Guard(const std::wstring& dos, bool write, const wchar_t* op) {
  std::wstring u = Upper(dos);
  for (const auto& r : g.protect) {
    if (MatchRule(u, r) == npos) continue;
    if (write) Fatal(kExitProtected, L"PROTECTED write blocked: %s %s", op, dos.c_str());
    if (InterlockedIncrement(&g.read_notes) <= 200) Log(L"read of protected path: %s %s", op, dos.c_str());
    return;
  }
  if (!write) return;
  for (const auto& r : g.watch)
    if (MatchRule(u, r) != npos && LogBudget()) Log(L"WATCH write: %s %s", op, dos.c_str());
}

Decision DecideDos(const std::wstring& dos, bool write, const wchar_t* op) {
  Decision d;
#ifndef SOWGUARD_MUTANT  // negative-control build: no redirect, no guard
  std::wstring u = Upper(dos);
  for (const auto& r : g.redirects) {
    size_t n = MatchRule(u, r);
    if (n == npos) continue;
    d.redirect = true;
    d.target = r.to + dos.substr(n);
    if (LogBudget()) Log(L"redirect %s%s %s -> %s", op, write ? L" (write)" : L"", dos.c_str(), d.target.c_str());
    Guard(d.target, write, op);
    return d;
  }
  Guard(dos, write, op);
#else
  (void)dos; (void)write; (void)op;
#endif
  return d;
}

struct OaCopy {
  OBJECT_ATTRIBUTES oa;
  UNICODE_STRING us;
  std::wstring buf;
};

void MakeOa(const OBJECT_ATTRIBUTES* src, const std::wstring& dos, OaCopy* c) {
  c->buf = L"\\??\\" + dos;
  c->us.Buffer = c->buf.data();
  c->us.Length = (USHORT)(c->buf.size() * sizeof(wchar_t));
  c->us.MaximumLength = c->us.Length;
  c->oa = *src;
  c->oa.RootDirectory = nullptr;
  c->oa.ObjectName = &c->us;
}

bool IsWriteAccess(ACCESS_MASK a) {
  return (a & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC |
               WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED)) != 0;
}

bool IsAfd(const OBJECT_ATTRIBUTES* oa) {
  if (!oa || oa->RootDirectory) return false;
  return MatchPrefix(Upper(OaName(oa)), L"\\DEVICE\\AFD") != npos;
}

// ---------------------------------------------------------------- hooks

using NtCreateFile_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                         ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtOpenFile_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
using NtQueryAttr_t = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
using NtSetInfo_t = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using NtDeleteFile_t = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);

NtCreateFile_t o_NtCreateFile;
NtOpenFile_t o_NtOpenFile;
NtQueryAttr_t o_NtQueryAttributesFile;
NtQueryAttr_t o_NtQueryFullAttributesFile;
NtSetInfo_t o_NtSetInformationFile;
NtDeleteFile_t o_NtDeleteFile;

NTSTATUS NTAPI HkNtCreateFile(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                              PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disp, ULONG opts, PVOID ea,
                              ULONG ea_len) {
  Reentry re;
  if (re.outer) {
    if (g.net == NetMode::kDenySockets && IsAfd(oa)) {
      if (InterlockedIncrement(&g.net_notes) <= 20) Log(L"socket denied (network=deny_sockets): %s", OaName(oa).c_str());
      return STATUS_ACCESS_DENIED;
    }
    std::wstring dos = OaDos(oa);
    if (!dos.empty()) {
      bool write = IsWriteAccess(access) || disp != FILE_OPEN || (opts & FILE_DELETE_ON_CLOSE);
      Decision d = DecideDos(dos, write, L"NtCreateFile");
      if (d.redirect) {
        OaCopy c;
        MakeOa(oa, d.target, &c);
        return o_NtCreateFile(h, access, &c.oa, io, alloc, attrs, share, disp, opts, ea, ea_len);
      }
    }
  }
  return o_NtCreateFile(h, access, oa, io, alloc, attrs, share, disp, opts, ea, ea_len);
}

NTSTATUS NTAPI HkNtOpenFile(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io, ULONG share,
                            ULONG opts) {
  Reentry re;
  if (re.outer) {
    std::wstring dos = OaDos(oa);
    if (!dos.empty()) {
      bool write = IsWriteAccess(access) || (opts & FILE_DELETE_ON_CLOSE);
      Decision d = DecideDos(dos, write, L"NtOpenFile");
      if (d.redirect) {
        OaCopy c;
        MakeOa(oa, d.target, &c);
        return o_NtOpenFile(h, access, &c.oa, io, share, opts);
      }
    }
  }
  return o_NtOpenFile(h, access, oa, io, share, opts);
}

NTSTATUS QueryAttr(NtQueryAttr_t orig, const wchar_t* op, POBJECT_ATTRIBUTES oa, PVOID info) {
  Reentry re;
  if (re.outer) {
    std::wstring dos = OaDos(oa);
    if (!dos.empty()) {
      Decision d = DecideDos(dos, false, op);
      if (d.redirect) {
        OaCopy c;
        MakeOa(oa, d.target, &c);
        return orig(&c.oa, info);
      }
    }
  }
  return orig(oa, info);
}

NTSTATUS NTAPI HkNtQueryAttributesFile(POBJECT_ATTRIBUTES oa, PVOID info) {
  return QueryAttr(o_NtQueryAttributesFile, L"NtQueryAttributesFile", oa, info);
}

NTSTATUS NTAPI HkNtQueryFullAttributesFile(POBJECT_ATTRIBUTES oa, PVOID info) {
  return QueryAttr(o_NtQueryFullAttributesFile, L"NtQueryFullAttributesFile", oa, info);
}

NTSTATUS NTAPI HkNtDeleteFile(POBJECT_ATTRIBUTES oa) {
  Reentry re;
  if (re.outer) {
    std::wstring dos = OaDos(oa);
    if (!dos.empty()) {
      Decision d = DecideDos(dos, true, L"NtDeleteFile");
      if (d.redirect) {
        OaCopy c;
        MakeOa(oa, d.target, &c);
        return o_NtDeleteFile(&c.oa);
      }
    }
  }
  return o_NtDeleteFile(oa);
}

// Rename and hard-link requests carry a target path (FILE_RENAME_INFORMATION layout:
// flags at 0, RootDirectory at 8, FileNameLength at 16, FileName at 20).
NTSTATUS NTAPI HkNtSetInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls) {
  Reentry re;
  const bool rename_or_link = cls == 10 || cls == 11 || cls == 65 || cls == 72;
  if (re.outer && rename_or_link && info && len >= 20) {
    auto* p = static_cast<uint8_t*>(info);
    HANDLE root = *reinterpret_cast<HANDLE*>(p + 8);
    ULONG name_len = *reinterpret_cast<ULONG*>(p + 16);
    if (20ull + name_len <= len) {
      std::wstring name(reinterpret_cast<wchar_t*>(p + 20), name_len / sizeof(wchar_t));
      std::wstring dos;
      if (root) {
        std::wstring r = HandleDos(root);
        if (!r.empty()) dos = Join(r, Slashes(name));
      } else if (!name.empty() && name[0] == L'\\') {
        dos = NtToDos(name);
      }
      // A bare file name renames inside the source's own directory, which was
      // already checked when the source was opened.
      if (!dos.empty()) {
        Decision d = DecideDos(dos, true, cls == 11 || cls == 72 ? L"link target" : L"rename target");
        if (d.redirect) {
          std::wstring nt = L"\\??\\" + d.target;
          std::vector<uint8_t> copy(20 + nt.size() * sizeof(wchar_t) + sizeof(wchar_t), 0);
          memcpy(copy.data(), p, 8);
          ULONG new_len = (ULONG)(nt.size() * sizeof(wchar_t));
          memcpy(copy.data() + 16, &new_len, sizeof new_len);
          memcpy(copy.data() + 20, nt.data(), new_len);
          return o_NtSetInformationFile(h, io, copy.data(), (ULONG)copy.size(), cls);
        }
      }
    }
  }
  return o_NtSetInformationFile(h, io, info, len, cls);
}

using NtDeviceIoControlFile_t = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG,
                                                  PVOID, ULONG);
NtDeviceIoControlFile_t o_NtDeviceIoControlFile;

constexpr ULONG kFileDeviceNetwork = 0x12;  // AFD ioctls are 0x12xxx

// AFD requests that carry a destination, as measured on Windows 11 26200 with
// test_guard.exe probe (see the coop-anything notes):
//   0x12007 connect (TCP and UDP):  sockaddr at offset 24
//   0x120C7 ConnectEx:              sockaddr at offset 10 (after a 1-entry TDI header)
//   0x12023 sendto (UDP):           sockaddr pointer at offset 96, its length at 88
constexpr ULONG kAfdConnect = 0x12007;
constexpr ULONG kAfdSuperConnect = 0x120C7;
constexpr ULONG kAfdSendDatagram = 0x12023;
constexpr NTSTATUS kStatusNetworkUnreachable = (NTSTATUS)0xC000023CL;  // -> WSAENETUNREACH

enum class Dest { kLoopback, kRemote, kUnknown };

// p points at a sockaddr of len bytes.
Dest ClassifySockaddr(const uint8_t* p, size_t len, wchar_t* text, size_t text_len, uint32_t* ip = nullptr,
                      uint16_t* port = nullptr) {
  if (len < 2) return Dest::kUnknown;
  uint16_t family = (uint16_t)(p[0] | p[1] << 8);
  if (family == 2 /* AF_INET */ && len >= 8) {
    if (ip) memcpy(ip, p + 4, 4);
    if (port) *port = (uint16_t)(p[2] << 8 | p[3]);
    swprintf_s(text, text_len, L"%u.%u.%u.%u:%u", p[4], p[5], p[6], p[7], p[2] << 8 | p[3]);
    return p[4] == 127 ? Dest::kLoopback : Dest::kRemote;
  }
  if (family == 23 /* AF_INET6 */ && len >= 24) {
    const uint8_t* a = p + 8;
    swprintf_s(text, text_len, L"[%02x%02x:%02x%02x:...:%02x%02x]:%u", a[0], a[1], a[2], a[3], a[14], a[15],
               p[2] << 8 | p[3]);
    static const uint8_t kLoop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (memcmp(a, kLoop6, 16) == 0) return Dest::kLoopback;
    if (memcmp(a, kMapped, 12) == 0 && a[12] == 127) return Dest::kLoopback;
    return Dest::kRemote;
  }
  swprintf_s(text, text_len, L"family %u", family);
  return Dest::kUnknown;
}

// Kept free of C++ objects so __try can guard reads of the caller's buffers.
Dest AfdDestination(ULONG code, const uint8_t* in, ULONG in_len, wchar_t* text, size_t text_len, uint32_t* ip,
                    uint16_t* port) {
  __try {
    if (code == kAfdConnect && in_len > 24) return ClassifySockaddr(in + 24, in_len - 24, text, text_len, ip, port);
    if (code == kAfdSuperConnect && in_len > 10) return ClassifySockaddr(in + 10, in_len - 10, text, text_len, ip, port);
    if (code == kAfdSendDatagram && in_len >= 104) {
      ULONG addr_len = *reinterpret_cast<const ULONG*>(in + 88);
      const uint8_t* addr = *reinterpret_cast<const uint8_t* const*>(in + 96);
      if (!addr || addr_len < 2 || addr_len > 128) return Dest::kUnknown;
      return ClassifySockaddr(addr, addr_len, text, text_len, ip, port);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    swprintf_s(text, text_len, L"unreadable request");
  }
  return Dest::kUnknown;
}

void TraceAfd(HANDLE h, ULONG code, const void* in, ULONG in_len) {
  if (InterlockedIncrement(&g.net_notes) > 400) return;
  wchar_t hex[3 * 128 + 1] = {};
  const auto* b = static_cast<const uint8_t*>(in);
  ULONG n = in ? std::min<ULONG>(in_len, 128) : 0;
  for (ULONG i = 0; i < n; ++i) swprintf_s(hex + 3 * i, 4, L"%02x ", b[i]);
  Log(L"afd ioctl 0x%05lX handle %p in_len %lu: %s", code, h, in_len, hex);
}

NTSTATUS NTAPI HkNtDeviceIoControlFile(HANDLE h, HANDLE event, PVOID apc, PVOID apc_ctx, PIO_STATUS_BLOCK io,
                                       ULONG code, PVOID in, ULONG in_len, PVOID out, ULONG out_len) {
  if ((code >> 12) == kFileDeviceNetwork && t_depth == 0) {
    Reentry re;
    if (g.net_trace) TraceAfd(h, code, in, in_len);
#ifndef SOWGUARD_MUTANT
    // Offline: fail every outgoing connect or datagram that is not loopback with
    // "network unreachable", as an unplugged PC would. Sockets themselves still work.
    if ((g.net == NetMode::kOffline || g.net == NetMode::kCoopPeer) &&
        (code == kAfdConnect || code == kAfdSuperConnect || code == kAfdSendDatagram)) {
      wchar_t text[96] = L"?";
      uint32_t ip = 0;
      uint16_t port = 0;
      Dest d = AfdDestination(code, static_cast<const uint8_t*>(in), in_len, text, 96, &ip, &port);
      // network=coop_peer: the mod's own socket may send to its peer (a client: the host's address; a host: the
      // address its valid packets came from). No other socket, and no other address for a client, is let through.
      const bool peer_ok = d == Dest::kRemote && g.net == NetMode::kCoopPeer && ip != 0 && g.peer_sock != 0 &&
                           (uintptr_t)h == g.peer_sock &&
                           (g.peer_any || ((uint32_t)g.peer_ip == ip && (uint16_t)g.peer_port == port));
      if (peer_ok) {
        if (InterlockedIncrement(&g.net_notes) <= 200 && InterlockedIncrement(&g.peer_notes) <= 3)
          Log(L"coop_peer: the co-op socket may send to %s", text);
      } else if (d != Dest::kLoopback) {
        if (InterlockedIncrement(&g.net_notes) <= 200)
          Log(L"offline: %s to %s refused (network unreachable)%s",
              code == kAfdSendDatagram ? L"sendto" : code == kAfdSuperConnect ? L"ConnectEx" : L"connect", text,
              d == Dest::kUnknown ? L" [unparsed request, blocked]" : L"");
        if (io) {
          io->Status = kStatusNetworkUnreachable;
          io->Information = 0;
        }
        return kStatusNetworkUnreachable;
      }
    }
#endif
  }
  return o_NtDeviceIoControlFile(h, event, apc, apc_ctx, io, code, in, in_len, out, out_len);
}

// Every hooked function must be a plain x64 syscall stub:
//   4C 8B D1 B8 <id32> F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3
// The stub has no position-dependent bytes, so a 24-byte copy is a working
// trampoline. Anything else (another hook already there, a new Windows layout) stops
// the game instead of guessing.
constexpr uint8_t kStubTail[16] = {0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,
                                   0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3};

struct HookSpec {
  const char* name;
  void* detour;
  void** original;
};

void InstallHooks() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  std::vector<HookSpec> specs = {
      {"NtCreateFile", (void*)&HkNtCreateFile, (void**)&o_NtCreateFile},
      {"NtOpenFile", (void*)&HkNtOpenFile, (void**)&o_NtOpenFile},
      {"NtQueryAttributesFile", (void*)&HkNtQueryAttributesFile, (void**)&o_NtQueryAttributesFile},
      {"NtQueryFullAttributesFile", (void*)&HkNtQueryFullAttributesFile, (void**)&o_NtQueryFullAttributesFile},
      {"NtSetInformationFile", (void*)&HkNtSetInformationFile, (void**)&o_NtSetInformationFile},
      {"NtDeleteFile", (void*)&HkNtDeleteFile, (void**)&o_NtDeleteFile},
      {"NtDeviceIoControlFile", (void*)&HkNtDeviceIoControlFile, (void**)&o_NtDeviceIoControlFile},
  };
  const size_t kCount = specs.size();
  auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!tramp) Fatal(kExitHook, L"VirtualAlloc for trampolines failed (%lu)", GetLastError());

  std::vector<uint8_t*> targets(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    auto* fn = reinterpret_cast<uint8_t*>(GetProcAddress(ntdll, specs[i].name));
    if (!fn) Fatal(kExitHook, L"ntdll!%S not found", specs[i].name);
    if (!(fn[0] == 0x4C && fn[1] == 0x8B && fn[2] == 0xD1 && fn[3] == 0xB8) || memcmp(fn + 8, kStubTail, 16) != 0)
      Fatal(kExitHook, L"ntdll!%S is not a plain syscall stub (already hooked by something else?)", specs[i].name);
    memcpy(tramp + 32 * i, fn, 24);
    *specs[i].original = tramp + 32 * i;
    targets[i] = fn;
  }
  DWORD old = 0;
  if (!VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old))
    Fatal(kExitHook, L"VirtualProtect trampolines failed (%lu)", GetLastError());

  for (size_t i = 0; i < kCount; ++i) {
    uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; dq detour
    memcpy(jmp + 6, &specs[i].detour, 8);
    if (!VirtualProtect(targets[i], sizeof jmp, PAGE_EXECUTE_READWRITE, &old))
      Fatal(kExitHook, L"VirtualProtect ntdll!%S failed (%lu)", specs[i].name, GetLastError());
    memcpy(targets[i], jmp, sizeof jmp);
    VirtualProtect(targets[i], sizeof jmp, old, &old);
    FlushInstructionCache(GetCurrentProcess(), targets[i], sizeof jmp);
    Log(L"hooked ntdll!%S", specs[i].name);
  }
}

// ---------------------------------------------------------------- config

std::wstring IniStr(const std::wstring& ini, const wchar_t* key) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(L"sowguard", key, L"", buf, 2048, ini.c_str());
  return buf;
}

bool IniBool(const std::wstring& ini, const wchar_t* key, bool def) {
  return GetPrivateProfileIntW(L"sowguard", key, def ? 1 : 0, ini.c_str()) != 0;
}

std::wstring RegStr(HKEY root, const wchar_t* sub, const wchar_t* value) {
  wchar_t buf[1024];
  DWORD size = sizeof buf;
  // RRF_RT_REG_SZ also accepts REG_EXPAND_SZ and expands it.
  if (RegGetValueW(root, sub, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
  return buf;
}

std::wstring Env(const wchar_t* name) {
  wchar_t buf[1024];
  DWORD n = GetEnvironmentVariableW(name, buf, 1024);
  return n && n < 1024 ? std::wstring(buf, n) : L"";
}

std::wstring Dir(const std::wstring& path) {
  size_t slash = path.find_last_of(L'\\');
  return slash == npos ? path : path.substr(0, slash);
}

std::wstring ModulePath(HMODULE mod) {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH * 4);
  return std::wstring(buf, n);
}

void MakeDirs(const std::wstring& p) {
  for (size_t i = 3; i <= p.size(); ++i)
    if (i == p.size() || p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
}

void AddRule(std::vector<Rule>& v, const std::wstring& from, const std::wstring& suffix, const std::wstring& to) {
  Rule r;
  r.from = CleanDir(from);
  r.suffix = suffix;
  r.to = to;
  r.fromU = Upper(r.from);
  r.suffixU = Upper(suffix);
  for (const auto& existing : v)
    if (existing.fromU == r.fromU && existing.suffixU == r.suffixU) return;
  v.push_back(r);
}

std::string ReadSmallFile(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  char buf[256];
  DWORD got = 0;
  BOOL ok = ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
  CloseHandle(f);
  if (!ok) return "";
  std::string s(buf, got);
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

void Init(HMODULE self) {
  InitializeCriticalSection(&g.log_lock);
  const std::wstring ini = Dir(ModulePath(self)) + L"\\sowguard.ini";
  if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) Fatal(kExitConfig, L"missing %s", ini.c_str());

  g.profile = CleanDir(IniStr(ini, L"profile"));
  if (!IsAbsoluteDos(g.profile)) Fatal(kExitConfig, L"profile must be an absolute path");
  MakeDirs(g.profile);
  g.log = CreateFileW((g.profile + L"\\sowguard.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  Log(L"---- sowguard start, pid %lu", GetCurrentProcessId());
  SimReserveNearPages();  // before the game's heap fills the range around the image

  const bool test_mode = IniBool(ini, L"test_mode", false);
  const std::wstring net = IniStr(ini, L"network");
  if (net.empty() || net == L"offline") g.net = NetMode::kOffline;
  else if (net == L"deny_sockets") g.net = NetMode::kDenySockets;
  else if (net == L"allow") g.net = NetMode::kAllow;
  else if (net == L"coop_peer") g.net = NetMode::kCoopPeer;
  else Fatal(kExitConfig, L"network must be offline, deny_sockets or allow (got %s)", net.c_str());
  g.net_trace = IniBool(ini, L"net_trace", false);
  const std::wstring exe = ModulePath(nullptr);
  const std::wstring exe_dir = Dir(exe);
  const std::wstring exeU = Upper(exe);
  Log(L"exe %s, test_mode %d, network %s, net_trace %d", exe.c_str(), test_mode, net.empty() ? L"offline" : net.c_str(),
      g.net_trace);

  // The owner's real locations. In test mode the ini supplies fake ones and the real
  // ones are only used to refuse a config that points at them.
  const std::wstring real_steam = CleanDir(RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
  const wchar_t* kShellFolders = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
  std::vector<std::wstring> real_local = {Env(L"LOCALAPPDATA"), RegStr(HKEY_CURRENT_USER, kShellFolders, L"Local AppData")};
  std::vector<std::wstring> real_docs = {RegStr(HKEY_CURRENT_USER, kShellFolders, L"Personal"),
                                         Env(L"USERPROFILE").empty() ? L"" : Env(L"USERPROFILE") + L"\\Documents"};

  std::vector<std::wstring> local, docs;
  std::wstring steam, install;
  if (test_mode) {
    local = {IniStr(ini, L"owner_localappdata")};
    docs = {IniStr(ini, L"owner_documents")};
    steam = CleanDir(IniStr(ini, L"owner_steam"));
    install = CleanDir(IniStr(ini, L"owner_install"));
    std::vector<std::wstring> fakes = {local[0], docs[0], steam, install};
    std::vector<std::wstring> reals = real_local;
    reals.insert(reals.end(), real_docs.begin(), real_docs.end());
    reals.push_back(real_steam);
    for (const auto& f : fakes) {
      if (!IsAbsoluteDos(f)) Fatal(kExitConfig, L"test mode needs absolute owner_* paths");
      for (const auto& r : reals)
        if (!r.empty() && MatchPrefix(Upper(CleanDir(f)), Upper(CleanDir(r))) != npos)
          Fatal(kExitConfig, L"test mode owner path %s points into the real %s", f.c_str(), r.c_str());
    }
  } else {
    local = real_local;
    docs = real_docs;
    steam = real_steam;
    if (steam.empty()) Fatal(kExitConfig, L"Steam path not found in the registry");
    install = steam + L"\\steamapps\\common\\ShadowOfWar";
  }

  // 1. Only ever run inside the test area, never in the Steam folder.
  if (!test_mode || IniBool(ini, L"check_allowed_root", true)) {
    std::wstring root = CleanDir(IniStr(ini, L"allowed_root"));
    if (!IsAbsoluteDos(root)) Fatal(kExitConfig, L"allowed_root must be an absolute path");
    if (MatchPrefix(exeU, Upper(root)) == npos)
      Fatal(kExitOutsideRoot, L"exe %s is outside allowed_root %s", exe.c_str(), root.c_str());
    if (MatchPrefix(exeU, Upper(steam)) != npos)
      Fatal(kExitOutsideRoot, L"exe %s is inside the Steam folder", exe.c_str());
  }

  // 2. Only the build we studied.
  if (!test_mode || IniBool(ini, L"check_exe", false)) {
    std::string hex;
    ULONGLONG size = 0;
    if (!HashFile(exe, &hex, &size)) Fatal(kExitExeIdentity, L"cannot read %s", exe.c_str());
    if (size != kExeSize || hex != kExeSha256)
      Fatal(kExitExeIdentity, L"unknown exe: size %llu sha256 %S", size, hex.c_str());
    Log(L"exe identity ok (sha256 %S)", hex.c_str());
  }

  // 3. steam_appid.txt keeps SteamStub from relaunching the owner's install via Steam.
  if (!test_mode || IniBool(ini, L"check_steam", false)) {
    if (ReadSmallFile(exe_dir + L"\\steam_appid.txt") != "356190")
      Fatal(kExitSteamAppId, L"%s\\steam_appid.txt must contain 356190", exe_dir.c_str());
    SetEnvironmentVariableW(L"SteamAppId", kAppId);
    SetEnvironmentVariableW(L"SteamGameId", kAppId);
  }

  // Rules. Redirect targets live in the profile; the same source areas are also
  // protected, so a path that slips past the redirect (8.3 names, say) still stops
  // the game instead of writing.
  const std::wstring p_local = g.profile + L"\\LocalAppData\\WB Games";
  const std::wstring p_docs = g.profile + L"\\Documents\\WB Games";
  const std::wstring p_steam = g.profile + L"\\steam-userdata\\356190";
  for (const auto& l : local)
    if (IsAbsoluteDos(l)) {
      AddRule(g.redirects, l + L"\\WB Games", L"", p_local);
      AddRule(g.protect, l + L"\\WB Games", L"", L"");
    }
  for (const auto& d : docs)
    if (IsAbsoluteDos(d)) {
      AddRule(g.redirects, d + L"\\WB Games", L"", p_docs);
      AddRule(g.protect, d + L"\\WB Games", L"", L"");
    }
  AddRule(g.redirects, steam + L"\\userdata", L"\\356190", p_steam);
  AddRule(g.protect, steam + L"\\userdata", L"\\356190", L"");
  AddRule(g.protect, install, L"", L"");
  AddRule(g.watch, steam + L"\\userdata", L"", L"");
  for (int i = 1; i <= 4; ++i) {
    wchar_t key[32];
    swprintf_s(key, L"protect_extra%d", i);
    std::wstring extra = IniStr(ini, key);
    if (IsAbsoluteDos(extra)) AddRule(g.protect, extra, L"", L"");
  }
  MakeDirs(p_local);
  MakeDirs(p_docs);
  MakeDirs(p_steam);
  for (const auto& r : g.redirects) Log(L"rule redirect %s%s -> %s", r.from.c_str(), r.suffix.empty() ? L"" : (L"\\*" + r.suffix).c_str(), r.to.c_str());
  for (const auto& r : g.protect) Log(L"rule protect %s%s", r.from.c_str(), r.suffix.empty() ? L"" : (L"\\*" + r.suffix).c_str());
  for (const auto& r : g.watch) Log(L"rule watch %s", r.from.c_str());

  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    wchar_t drive[3] = {letter, L':', 0};
    wchar_t target[512];
    if (QueryDosDeviceW(drive, target, 512)) g.devices.emplace_back(Upper(target), drive);
  }

  InstallHooks();
  // Steam cloud-save writes are done by steam.exe, out of reach of the file hooks.
  SteamGuardConfig sg;
  sg.trace_reads = IniBool(ini, L"steam_trace", false);
  const std::wstring cloud = IniStr(ini, L"steam_cloud");
  if (cloud == L"sandbox") {
    sg.cloud_dir = p_steam + L"\\remote";
    MakeDirs(sg.cloud_dir);
  } else if (!cloud.empty() && cloud != L"block") {
    Fatal(kExitConfig, L"steam_cloud must be block or sandbox (got %s)", cloud.c_str());
  }
  StartSteamGuard(sg);
  Log(L"sowguard ready");

  if (IniBool(ini, L"autoload", false)) {
    InputConfig ic;
    auto num = [&](const wchar_t* key, unsigned def) {
      return (unsigned)GetPrivateProfileIntW(L"sowguard", key, (INT)def, ini.c_str());
    };
    ic.first_ms = num(L"autoload_first_ms", ic.first_ms);
    ic.every_ms = num(L"autoload_every_ms", ic.every_ms);
    ic.max_presses = num(L"autoload_max_presses", ic.max_presses);
    ic.hold_ms = num(L"autoload_hold_ms", ic.hold_ms);
    if (!IniStr(ini, L"autoload_window_class").empty()) ic.window_class = IniStr(ini, L"autoload_window_class");
    // Tests run a normal exe, whose import table is in .rdata.
    if (test_mode && !IniStr(ini, L"autoload_iat_section").empty()) {
      ic.iat_section.clear();
      for (wchar_t c : IniStr(ini, L"autoload_iat_section")) ic.iat_section += (char)c;  // section names are ASCII
    }
    if (!IniBool(ini, L"hook_debug_print", false))
      Log(L"autoload without hook_debug_print: the load is not seen, pressing stops only after the last press");
    ic.background_run = IniBool(ini, L"background_run", false);
    const std::wstring sim = IniStr(ini, L"sim_hook");  // <vtable rva hex>:<slot>
    if (!sim.empty()) {
      size_t colon = sim.find(L':');
      if (colon == npos) Fatal(kExitConfig, L"sim_hook must be <vtable rva hex>:<slot> (got %s)", sim.c_str());
      ic.sim_vtable_rva = wcstoull(sim.c_str(), nullptr, 16);
      ic.sim_slot = (unsigned)wcstoul(sim.c_str() + colon + 1, nullptr, 10);
      if (!ic.sim_vtable_rva || ic.sim_slot > 255) Fatal(kExitConfig, L"bad sim_hook %s", sim.c_str());
    }
    StartInput(ic);
  }

  if (IniBool(ini, L"probe", false)) {
    ProbeConfig pc;
    pc.profile = g.profile;
    // Default: the GameClient load the freecam mod uses; it only exists once unpacked.
    std::wstring ready = IniStr(ini, L"probe_ready_pattern");
    if (ready.empty()) ready = L"48 8B 0D ? ? ? ? E8 ? ? ? ? C7 47";
    char narrow[512] = {};
    WideCharToMultiByte(CP_ACP, 0, ready.c_str(), -1, narrow, sizeof narrow, nullptr, nullptr);
    pc.ready_pattern = narrow;
    pc.wait_sec = (unsigned)GetPrivateProfileIntW(L"sowguard", L"probe_wait_sec", 300, ini.c_str());
    pc.write = IniBool(ini, L"probe_write", false);
    pc.hook_debug_print = IniBool(ini, L"hook_debug_print", false);
    pc.coop = IniStr(ini, L"coop_role") == L"host" || IniStr(ini, L"coop_role") == L"client";
    // Default: the call site the save editor uses; its call target is the debug print.
    std::wstring dp = IniStr(ini, L"debug_print_pattern");
    if (dp.empty()) dp = L"E8 ? ? ? ? 48 83 C3 10 48 8D 54 24 40 48 89 5C 24 30 48 8D 4C 24 30 E8 ? ? ? ? 84 C0 75 AE";
    char narrow_dp[512] = {};
    WideCharToMultiByte(CP_ACP, 0, dp.c_str(), -1, narrow_dp, sizeof narrow_dp, nullptr, nullptr);
    pc.debug_print_pattern = narrow_dp;
    std::wstring channels = IniStr(ini, L"debug_channels");
    char narrow_ch[2048] = {};
    WideCharToMultiByte(CP_ACP, 0, channels.c_str(), -1, narrow_ch, sizeof narrow_ch, nullptr, nullptr);
    pc.debug_channels = narrow_ch;
    // Tests point these at their own stand-ins.
    if (test_mode && !IniStr(ini, L"gameclient_rva").empty())
      pc.gameclient_rva = wcstoull(IniStr(ini, L"gameclient_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"save_handler_rva").empty())
      pc.save_handler_rva = wcstoull(IniStr(ini, L"save_handler_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"lkp_rva").empty())
      pc.lkp_rva = wcstoull(IniStr(ini, L"lkp_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"move_request_rva").empty())
      pc.move_request_rva = wcstoull(IniStr(ini, L"move_request_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"brain_site_rva").empty())
      pc.brain_site_rva = wcstoull(IniStr(ini, L"brain_site_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"tree_rva").empty())
      pc.tree_rva = wcstoull(IniStr(ini, L"tree_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"teleport_rva").empty())
      pc.teleport_rva = wcstoull(IniStr(ini, L"teleport_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"anim_var_rva").empty())
      pc.anim_var_rva = wcstoull(IniStr(ini, L"anim_var_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"slowmo_rva").empty())
      pc.slowmo_rva = wcstoull(IniStr(ini, L"slowmo_rva").c_str(), nullptr, 16);
    StartProbe(pc);
  }

  // Coop over loopback: coop_send=127.0.0.1:<port>, coop_listen=<port>, coop_show=lkp.
  const std::wstring send = IniStr(ini, L"coop_send");
  const unsigned listen = (unsigned)GetPrivateProfileIntW(L"sowguard", L"coop_listen", 0, ini.c_str());
  const std::wstring net_role = IniStr(ini, L"coop_role");
  if (g.net == NetMode::kCoopPeer && net_role.empty()) Fatal(kExitConfig, L"network=coop_peer needs coop_role=host|client");
  if (!send.empty() || listen || !net_role.empty()) {
    CoopConfig cc;
    if (!net_role.empty()) {
      if (net_role != L"host" && net_role != L"client") Fatal(kExitConfig, L"coop_role must be host or client (got %s)", net_role.c_str());
      if (!send.empty() || listen) Fatal(kExitConfig, L"coop_role replaces coop_send and coop_listen; use one or the other");
      cc.net_role = net_role == L"host" ? "host" : "client";
      cc.net_port = (unsigned)GetPrivateProfileIntW(L"sowguard", L"coop_port", 0, ini.c_str());
      std::string bind_addr;
      for (wchar_t c : IniStr(ini, L"coop_bind")) bind_addr += (char)c;
      if (!bind_addr.empty()) cc.net_bind = bind_addr;
      for (wchar_t c : IniStr(ini, L"coop_host")) cc.net_peer += (char)c;
      for (wchar_t c : IniStr(ini, L"coop_password")) cc.net_password += (char)c;
      cc.state_hz = (unsigned)GetPrivateProfileIntW(L"sowguard", L"coop_state_hz", (INT)cc.state_hz, ini.c_str());
      if (cc.state_hz < 1 || cc.state_hz > 120) Fatal(kExitConfig, L"coop_state_hz must be 1..120");
      if (cc.net_role == "host" && (cc.net_port == 0 || cc.net_port > 65535)) Fatal(kExitConfig, L"a host needs coop_port 1..65535");
      if (cc.net_role == "client" && cc.net_peer.empty()) Fatal(kExitConfig, L"a client needs coop_host=<ip>:<port>");
      if (g.net == NetMode::kCoopPeer && cc.net_role == "host" && bind_addr.empty()) cc.net_bind = "0.0.0.0";
      cc.exe_sha_hex = kExeSha256;
      cc.delay_ms = 120;  // the default for a real link; coop_delay_ms below overrides
      if (cloud == L"sandbox") {
        cc.save_dir = sg.cloud_dir;
        cc.save_local_dir = p_local + L"\\Shadow of War";
      }
      const std::wstring join = IniStr(ini, L"coop_join");
      if (join == L"off") cc.join = false;
      else if (!join.empty() && join != L"on") Fatal(kExitConfig, L"coop_join must be on or off (got %s)", join.c_str());
      cc.join_timeout_sec = (unsigned)GetPrivateProfileIntW(L"sowguard", L"coop_join_timeout_sec", (INT)cc.join_timeout_sec, ini.c_str());
      if (cc.join_timeout_sec < 2 || cc.join_timeout_sec > 600) Fatal(kExitConfig, L"coop_join_timeout_sec must be 2..600");
      if (cc.net_role == "client" && cc.join && cc.save_dir.empty())
        Fatal(kExitConfig, L"a client plays in the host's save, which needs steam_cloud=sandbox (or coop_join=off)");
      cc.join_message = !test_mode;
      const std::wstring enemies = IniStr(ini, L"coop_enemies");
      if (enemies == L"on") cc.enemies = true;
      else if (!enemies.empty() && enemies != L"off") Fatal(kExitConfig, L"coop_enemies must be on or off (got %s)", enemies.c_str());
      const std::wstring slowmo = IniStr(ini, L"coop_slowmo");
      if (slowmo == L"on") cc.slowmo_off = false;
      else if (!slowmo.empty() && slowmo != L"off") Fatal(kExitConfig, L"coop_slowmo must be on or off (got %s)", slowmo.c_str());
      const std::wstring nem = IniStr(ini, L"coop_nemesis");
      if (nem == L"off") cc.nemesis = false;
      else if (nem == L"share") cc.nemesis_follow = false;
      else if (!nem.empty() && nem != L"on" && nem != L"follow") Fatal(kExitConfig, L"coop_nemesis must be off, share or follow (got %s)", nem.c_str());
      const std::wstring capt = IniStr(ini, L"coop_captain_look");
      if (capt == L"off") cc.captain_look = false;
      else if (!capt.empty() && capt != L"on") Fatal(kExitConfig, L"coop_captain_look must be on or off (got %s)", capt.c_str());
      const std::wstring hby = IniStr(ini, L"coop_hit_by");
      if (hby == L"player") cc.hit_by = 0;
      else if (hby == L"poke") cc.hit_by = 2;
      else if (!hby.empty() && hby != L"decoy") Fatal(kExitConfig, L"coop_hit_by must be decoy, poke or player (got %s)", hby.c_str());
      const std::wstring shi = IniStr(ini, L"coop_standin_hits");
      if (shi == L"on") cc.standin_hits = true;
      else if (!shi.empty() && shi != L"off") Fatal(kExitConfig, L"coop_standin_hits must be on or off (got %s)", shi.c_str());
      const std::wstring hst = IniStr(ini, L"coop_hunt_strict");
      if (hst == L"on") cc.hunt_strict = true;
      else if (!hst.empty() && hst != L"off") Fatal(kExitConfig, L"coop_hunt_strict must be on or off (got %s)", hst.c_str());
      const std::wstring hnt = IniStr(ini, L"coop_hunt");
      if (hnt == L"off") cc.hunt_retarget = false;
      else if (!hnt.empty() && hnt != L"on") Fatal(kExitConfig, L"coop_hunt must be on or off (got %s)", hnt.c_str());
      const std::wstring prm = IniStr(ini, L"coop_proxy_react");
      if (prm == L"off") cc.proxy_react = false;
      else if (!prm.empty() && prm != L"on") Fatal(kExitConfig, L"coop_proxy_react must be on or off (got %s)", prm.c_str());
      const std::wstring ldm = IniStr(ini, L"coop_local_damage");
      if (ldm == L"off") cc.local_damage_report = false;
      else if (!ldm.empty() && ldm != L"on") Fatal(kExitConfig, L"coop_local_damage must be on or off (got %s)", ldm.c_str());
      const std::wstring amir = IniStr(ini, L"coop_anim_mirror");
      if (amir == L"off") cc.anim_mirror = false;
      else if (!amir.empty() && amir != L"on") Fatal(kExitConfig, L"coop_anim_mirror must be on or off (got %s)", amir.c_str());
      const std::wstring mdata = IniStr(ini, L"coop_member_data");
      if (mdata == L"off") cc.member_data = false;
      else if (!mdata.empty() && mdata != L"on") Fatal(kExitConfig, L"coop_member_data must be on or off (got %s)", mdata.c_str());
      const std::wstring hsend = IniStr(ini, L"coop_decoy_hurt_send");
      if (hsend == L"off") cc.decoy_hurt_send = false;
      else if (!hsend.empty() && hsend != L"on") Fatal(kExitConfig, L"coop_decoy_hurt_send must be on or off (got %s)", hsend.c_str());
      const std::wstring afatal = IniStr(ini, L"coop_audio_keys_fatal");
      if (afatal == L"on") cc.audio_keys_fatal = true;
      else if (!afatal.empty() && afatal != L"off") Fatal(kExitConfig, L"coop_audio_keys_fatal must be on or off (got %s)", afatal.c_str());
      const std::wstring dfollow = IniStr(ini, L"coop_decoy_follow");
      if (dfollow == L"standin") cc.decoy_follow_remote = false;
      else if (!dfollow.empty() && dfollow != L"remote") Fatal(kExitConfig, L"coop_decoy_follow must be remote or standin (got %s)", dfollow.c_str());
      const std::wstring borrow = IniStr(ini, L"coop_dmg_borrow");
      if (borrow == L"off") cc.dmg_borrow = false;
      else if (!borrow.empty() && borrow != L"on") Fatal(kExitConfig, L"coop_dmg_borrow must be on or off (got %s)", borrow.c_str());
      const std::wstring dlimbo = IniStr(ini, L"coop_decoy_limbo");
      if (dlimbo == L"off") cc.decoy_limbo = false;
      else if (!dlimbo.empty() && dlimbo != L"on") Fatal(kExitConfig, L"coop_decoy_limbo must be on or off (got %s)", dlimbo.c_str());
      const std::wstring rrefs = IniStr(ini, L"coop_retarget_refs");
      if (rrefs == L"off") cc.retarget_refs = false;
      else if (!rrefs.empty() && rrefs != L"on") Fatal(kExitConfig, L"coop_retarget_refs must be on or off (got %s)", rrefs.c_str());
      const std::wstring unlink = IniStr(ini, L"coop_unlink_fix");
      if (unlink == L"off") cc.unlink_fix = false;
      else if (!unlink.empty() && unlink != L"on") Fatal(kExitConfig, L"coop_unlink_fix must be on or off (got %s)", unlink.c_str());
      const std::wstring huntp = IniStr(ini, L"coop_hunt_to_player");
      if (huntp == L"on") cc.hunt_to_player = true;
      else if (!huntp.empty() && huntp != L"off") Fatal(kExitConfig, L"coop_hunt_to_player must be on or off (got %s)", huntp.c_str());
      const std::wstring aiguard = IniStr(ini, L"coop_ai_param_guard");
      if (aiguard == L"off") cc.ai_param_guard = false;
      else if (!aiguard.empty() && aiguard != L"on") Fatal(kExitConfig, L"coop_ai_param_guard must be on or off (got %s)", aiguard.c_str());
      const std::wstring travel = IniStr(ini, L"coop_follow_travel");
      if (travel == L"off") cc.follow_travel = false;
      else if (!travel.empty() && travel != L"on") Fatal(kExitConfig, L"coop_follow_travel must be on or off (got %s)", travel.c_str());
      const std::wstring decoy = IniStr(ini, L"coop_decoy");
      if (decoy == L"on") cc.decoy = true;
      else if (decoy == L"off") cc.decoy = false;
      else if (!decoy.empty()) Fatal(kExitConfig, L"coop_decoy must be on or off (got %s)", decoy.c_str());
      cc.decoy_dy = (float)GetPrivateProfileIntW(L"sowguard", L"coop_decoy_dy", 0, ini.c_str());
      cc.wake_radius = (float)GetPrivateProfileIntW(L"sowguard", L"coop_wake_radius", (INT)cc.wake_radius, ini.c_str());
      cc.stuck_sec = (int)GetPrivateProfileIntW(L"sowguard", L"coop_stuck_sec", cc.stuck_sec, ini.c_str());
      cc.decoy_stall = (int)GetPrivateProfileIntW(L"sowguard", L"coop_decoy_stall", cc.decoy_stall, ini.c_str());
      cc.decoy_reanchor = (float)GetPrivateProfileIntW(L"sowguard", L"coop_decoy_reanchor", (INT)cc.decoy_reanchor, ini.c_str());
      cc.decoy_remake = (float)GetPrivateProfileIntW(L"sowguard", L"coop_decoy_remake", (INT)cc.decoy_remake, ini.c_str());
      cc.decoy_still = (float)GetPrivateProfileIntW(L"sowguard", L"coop_decoy_still", (INT)cc.decoy_still, ini.c_str());
      cc.proxy_table_max = (int)GetPrivateProfileIntW(L"sowguard", L"coop_proxy_table_max", cc.proxy_table_max, ini.c_str());
      if (cc.proxy_table_max < 0 || cc.proxy_table_max > 250) Fatal(kExitConfig, L"coop_proxy_table_max must be 0..250");
      cc.enemy_radius = (float)GetPrivateProfileIntW(L"sowguard", L"coop_enemy_radius", (INT)cc.enemy_radius, ini.c_str());
      if (cc.enemy_radius < 500 || cc.enemy_radius > 20000) Fatal(kExitConfig, L"coop_enemy_radius must be 500..20000");
    }
    if (!send.empty()) {
      const std::wstring host = L"127.0.0.1:";
      if (send.compare(0, host.size(), host) != 0) Fatal(kExitConfig, L"coop_send must be 127.0.0.1:<port> (got %s)", send.c_str());
      cc.send_port = (unsigned)wcstoul(send.c_str() + host.size(), nullptr, 10);
    }
    cc.listen_port = listen;
    if ((!send.empty() && (cc.send_port == 0 || cc.send_port > 65535)) || listen > 65535)
      Fatal(kExitConfig, L"coop ports must be 1..65535");
    cc.rate_hz = (unsigned)GetPrivateProfileIntW(L"sowguard", L"coop_rate_hz", (INT)cc.rate_hz, ini.c_str());
    cc.delay_ms = (unsigned)GetPrivateProfileIntW(L"sowguard", L"coop_delay_ms", (INT)cc.delay_ms, ini.c_str());
    if (cc.delay_ms > 1000) Fatal(kExitConfig, L"coop_delay_ms must be 0..1000");
    const std::wstring show = IniStr(ini, L"coop_show");
    if (show == L"lkp") cc.show = true;
    else if (show == L"puppet") cc.puppet = true;
    else if (!show.empty()) Fatal(kExitConfig, L"coop_show must be lkp, puppet or empty (got %s)", show.c_str());
    const std::wstring move = IniStr(ini, L"coop_puppet_move");
    if (move == L"walk") cc.follow = 2;
    else if (move == L"both") cc.follow = 3;
    else if (move == L"anim") cc.follow = 4;
    else if (!move.empty() && move != L"teleport")
      Fatal(kExitConfig, L"coop_puppet_move must be teleport, walk, both or anim (got %s)", move.c_str());
    const std::wstring ai = IniStr(ini, L"coop_puppet_ai");
    if (ai == L"off") cc.puppet_ai = false;
    else if (!ai.empty() && ai != L"on") Fatal(kExitConfig, L"coop_puppet_ai must be on or off (got %s)", ai.c_str());
    for (wchar_t c : IniStr(ini, L"coop_puppet_spawn")) {
      if (!iswalnum(c) && c != L'_') Fatal(kExitConfig, L"coop_puppet_spawn must be a character type name");
      cc.spawn_type += (char)c;
    }
    cc.run_speed = (float)GetPrivateProfileIntW(L"sowguard", L"coop_run_speed", (INT)cc.run_speed, ini.c_str());
    cc.teleport_dist =
        (float)GetPrivateProfileIntW(L"sowguard", L"coop_teleport_dist", (INT)cc.teleport_dist, ini.c_str());
    cc.anim_snap = (float)GetPrivateProfileIntW(L"sowguard", L"coop_anim_snap", (INT)cc.anim_snap, ini.c_str());
    const std::wstring inputs = IniStr(ini, L"coop_puppet_inputs");
    if (inputs == L"off") cc.puppet_inputs = false;
    else if (!inputs.empty() && inputs != L"on")
      Fatal(kExitConfig, L"coop_puppet_inputs must be on or off (got %s)", inputs.c_str());
    const std::wstring actions = IniStr(ini, L"coop_puppet_actions");
    if (actions == L"off") cc.puppet_actions = false;
    else if (!actions.empty() && actions != L"on")
      Fatal(kExitConfig, L"coop_puppet_actions must be on or off (got %s)", actions.c_str());
    const std::wstring speed = IniStr(ini, L"coop_anim_speed");
    if (speed == L"off") cc.anim_speed = false;
    else if (!speed.empty() && speed != L"on")
      Fatal(kExitConfig, L"coop_anim_speed must be on or off (got %s)", speed.c_str());
    const std::wstring follower = IniStr(ini, L"coop_puppet_follower");
    if (follower == L"off") cc.puppet_follower = false;
    else if (!follower.empty() && follower != L"on")
      Fatal(kExitConfig, L"coop_puppet_follower must be on or off (got %s)", follower.c_str());
    const std::wstring targetable = IniStr(ini, L"coop_puppet_targetable");
    if (targetable == L"on") cc.puppet_targetable = true;
    else if (!targetable.empty() && targetable != L"off")
      Fatal(kExitConfig, L"coop_puppet_targetable must be on or off (got %s)", targetable.c_str());
    const std::wstring air = IniStr(ini, L"coop_air_follow");
    if (air == L"off") cc.air_follow = false;
    else if (!air.empty() && air != L"on")
      Fatal(kExitConfig, L"coop_air_follow must be on or off (got %s)", air.c_str());
    const std::wstring attacks = IniStr(ini, L"coop_puppet_attacks");
    if (attacks == L"off") cc.puppet_attacks = false;
    else if (!attacks.empty() && attacks != L"on")
      Fatal(kExitConfig, L"coop_puppet_attacks must be on or off (got %s)", attacks.c_str());
    if (test_mode && !IniStr(ini, L"anim_var_rva").empty())
      cc.anim_var_rva = wcstoull(IniStr(ini, L"anim_var_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"mover_step_rva").empty())
      cc.mover_step_rva = wcstoull(IniStr(ini, L"mover_step_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"follower_rva").empty())
      cc.follower_rva = wcstoull(IniStr(ini, L"follower_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"target_check_rva").empty())
      cc.target_check_rva = wcstoull(IniStr(ini, L"target_check_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"hit_rva").empty())
      cc.hit_rva = wcstoull(IniStr(ini, L"hit_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"aoe_rva").empty())
      cc.aoe_rva = wcstoull(IniStr(ini, L"aoe_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"anim_reset_rva").empty())
      cc.anim_reset_rva = wcstoull(IniStr(ini, L"anim_reset_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"attack_thunk_rva").empty())
      cc.attack_thunk_rva = wcstoull(IniStr(ini, L"attack_thunk_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"attack_stop_rva").empty())
      cc.attack_stop_rva = wcstoull(IniStr(ini, L"attack_stop_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"attack_table_rva").empty())
      cc.attack_table_rva = wcstoull(IniStr(ini, L"attack_table_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"move_request_rva").empty())
      cc.move_request_rva = wcstoull(IniStr(ini, L"move_request_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"brain_site_rva").empty())
      cc.brain_site_rva = wcstoull(IniStr(ini, L"brain_site_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"tree_rva").empty())
      cc.tree_rva = wcstoull(IniStr(ini, L"tree_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"gameclient_rva").empty())
      cc.gameclient_rva = wcstoull(IniStr(ini, L"gameclient_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"lkp_rva").empty()) cc.lkp_rva = wcstoull(IniStr(ini, L"lkp_rva").c_str(), nullptr, 16);
    if (test_mode && !IniStr(ini, L"teleport_rva").empty())
      cc.teleport_rva = wcstoull(IniStr(ini, L"teleport_rva").c_str(), nullptr, 16);
    StartCoop(cc);
  }
}

}  // namespace

[[noreturn]] void SowFatal(unsigned long code, const wchar_t* fmt, ...) {
  wchar_t msg[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
  va_end(ap);
  Fatal(code, L"%s", msg);
}

void SowAllowPeerSocket(uintptr_t sock, uint32_t ipv4_net_order, uint16_t port, bool any) {
  InterlockedExchange(&g.peer_ip, (LONG)ipv4_net_order);
  InterlockedExchange(&g.peer_port, (LONG)port);
  InterlockedExchange(&g.peer_any, any ? 1 : 0);
  g.peer_sock = sock;
}

void SowLog(const wchar_t* fmt, ...) {
  wchar_t msg[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
  va_end(ap);
  Log(L"%s", msg);
}

// Test hook: feeds a game log line to the virtual pad, as the debug-print sink does.
extern "C" __declspec(dllexport) void SowGuardSelfTest_DebugLine(const char* category, const char* msg) {
  InputOnDebugLine(category, msg);
}

// Test hook: lets the offline tests check the SHA-256 code against a known file.
extern "C" __declspec(dllexport) int SowGuardSelfTest_HashFile(const wchar_t* path, char* out65) {
  std::string hex;
  ULONGLONG size = 0;
  if (!HashFile(path, &hex, &size)) return 0;
  memcpy(out65, hex.c_str(), 65);
  return 1;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(mod);
    try {
      Init(mod);
    } catch (...) {
      Fatal(kExitConfig, L"exception during init");
    }
  }
  return TRUE;
}
