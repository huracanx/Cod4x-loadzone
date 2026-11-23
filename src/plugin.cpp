// Adapted from Rex109/CoD4QOL. See README and LICENSE.md.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include "detours.h"
#include "common.hpp"

#if !defined(_M_IX86)
#error This plugin must be built for Win32 (x86).
#endif

namespace {
// Known CoD4x builds. CRC selects a candidate build; code addresses are
// resolved from executable-section signatures, never from filename or image base.
constexpr uint32_t kCRC21_1 = 0xb1822ed5u;
constexpr uint32_t kCRC21_8 = 0xcb77ae89u;
// Engine-side addresses below are valid only for the supplied iw3mp.exe build.
constexpr uint32_t kIw3mpCRC = 0x651accbau;

struct BytePattern {
    const unsigned char* bytes;
    const char* mask; // 'x' = exact byte, '?' = wildcard
    size_t length;
};
constexpr unsigned char kCmdAddBytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x28,0xa1,0x28,0x7a,0x3a,0x63,
    0x8b,0x00,0x89,0x45,0xf4,0xeb,0x4b,0x8b,0x45,0xf4,0x8b,0x40,0x04
};
constexpr unsigned char kInitBytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x78,0xc7,0x45,0xf4,0x00,0x00,0x00,
    0x00,0xb8,0x10,0xd1,0xc9,0x0c,0x8b,0x48,0x18,0x8b,0x55,0xf4
};
constexpr unsigned char kBuildPathBytes[] = {
    0x55,0x89,0xe5,0x53,0x83,0xec,0x74,0x83,0x7d,0x0c,0x01,
    0x74,0x0b,0x83,0x7d,0x0c,0x02,0x74,0x44,0xe9,0xb8,0x00,0x00,0x00
};
constexpr BytePattern kCmdAddPattern{ kCmdAddBytes, "xxxxxxxxxxxxxxxxxxxxxxxx", sizeof(kCmdAddBytes) };
constexpr BytePattern kInitPattern{ kInitBytes, "xxxxxxxxxxxxxxxxxxxxxxxx", sizeof(kInitBytes) };
constexpr BytePattern kBuildPathPattern{ kBuildPathBytes, "xxxxxxxxxxxxxxxxxxxxxxxx", sizeof(kBuildPathBytes) };

bool getExecutableText(HMODULE module, const unsigned char*& begin, size_t& size) {
    if (!module) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(section[i].Name, ".text", 5) == 0 &&
            (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            const size_t n = section[i].Misc.VirtualSize;
            if (!n || n > 0x1000000u) return false;
            begin = reinterpret_cast<const unsigned char*>(module) + section[i].VirtualAddress;
            size = n;
            return true;
        }
    }
    return false;
}

bool patternMatches(const unsigned char* at, const BytePattern& pattern) {
    for (size_t i = 0; i < pattern.length; ++i)
        if (pattern.mask[i] == 'x' && at[i] != pattern.bytes[i]) return false;
    return true;
}

// A signature is accepted only if exactly one match exists in .text.
bool findUniquePattern(HMODULE module, const BytePattern& pattern, uintptr_t& rva) {
    const unsigned char* text = nullptr;
    size_t textSize = 0;
    if (!getExecutableText(module, text, textSize) || !pattern.length ||
        pattern.length > textSize) return false;
    size_t matches = 0;
    uintptr_t found = 0;
    for (size_t i = 0; i <= textSize - pattern.length; ++i) {
        if (patternMatches(text + i, pattern)) {
            found = static_cast<uintptr_t>(text + i -
                reinterpret_cast<const unsigned char*>(module));
            if (++matches > 1) return false;
        }
    }
    if (matches != 1) return false;
    rva = found;
    return true;
}

struct CmdArgs { int nesting; int localClientNum[8]; int controllerIndex[8]; int argc[8]; const char** argv[8]; };
union DvarValue { bool enabled; int integer; float value; float vector[4]; const char* string; };
struct Dvar { const char* name; const char* description; uint16_t flags; int8_t type; bool modified; DvarValue current; };
struct XZoneInfo { const char* name; int allocFlags; int freeFlags; };
static_assert(offsetof(Dvar, current) == 12, "Unexpected dvar layout");
using InitFn = void (__cdecl*)();
using BuildPathFn = void (__cdecl*)(const char*, int, int, char*);
using AddCommandFn = void* (__cdecl*)(const char*, void (__cdecl*)());
using PrintFn = void (__cdecl*)(int, const char*, char);
using LoadFn = void (__cdecl*)(XZoneInfo*, unsigned, int);
InitFn originalInit = nullptr;
BuildPathFn originalBuildPath = nullptr;
AddCommandFn addCommand = nullptr;
auto print = reinterpret_cast<PrintFn>(0x4fca50);
auto load = reinterpret_cast<LoadFn>(0x48a2b0);
auto args = reinterpret_cast<CmdArgs*>(0x1410b40);
bool registered = false;
bool hasLoadedZone = false; // Tracks requests in this process, not the engine zone list.
constexpr int kZoneFlag = 0x40;
char activeName[65] = {};
char activePath[MAX_PATH] = {};
std::atomic<bool> loading{false};

// Original IW3 dvar lookup convention uses EDI, not the usual cdecl argument.
__declspec(naked) Dvar* findDvar(const char*) {
    __asm {
        push eax
        pushad
        mov edi, [esp + 28h]
        mov eax, 056B5D0h
        call eax
        mov [esp + 20h], eax
        popad
        pop eax
        ret
    }
}
void say(const char* text) { print(0, text, 0); }
void __cdecl loadZone() {
    if (args->nesting < 0 || args->nesting >= 8 || args->argc[args->nesting] != 2) {
        say("Usage: loadzone <name_without_ff>\n"); return;
    }
    const char* name = args->argv[args->nesting][1];
    if (!valid_zone_name(name)) {
        say("LoadZone: use 1-64 letters, digits, _ or -; no path or .ff\n"); return;
    }
    if (loading.load(std::memory_order_acquire)) {
        say("LoadZone: another zone is loading\n"); return;
    }
    Dvar* home = findDvar("fs_homepath");
    const char* lang = *reinterpret_cast<const char**>(0xCC147D4);
    if (!home || !home->current.string || !lang || !*lang) {
        say("LoadZone: fs_homepath or game language is unavailable\n"); return;
    }
    int n = _snprintf_s(activePath, sizeof(activePath), _TRUNCATE, "%s\\zone\\%s\\%s.ff", home->current.string, lang, name);
    if (n < 0) { say("LoadZone: path too long\n"); return; }
    DWORD attrs = GetFileAttributesA(activePath);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        say("LoadZone: .ff not found in fs_homepath/zone/<language>/\n"); return;
    }
    strcpy_s(activeName, name);
    loading.store(true, std::memory_order_release);
    XZoneInfo zones[2]{};
    unsigned count = 0;
    if (hasLoadedZone) {
        // Engine frees *every* zone carrying this flag, not just our previous name.
        zones[count++] = XZoneInfo{nullptr, 0, kZoneFlag};
        say("LoadZone: replacing previous zone (free flag 0x40)...\n");
    }
    zones[count++] = XZoneInfo{activeName, kZoneFlag, 0};
    say("LoadZone: loading...\n");
    load(zones, count, 1); // Unload first, then synchronously load the new zone.
    hasLoadedZone = true;  // Return alone is not proof the assets were loaded correctly.
    loading.store(false, std::memory_order_release);
    say("LoadZone: load call returned.\n");
}
void __cdecl unloadZone() {
    if (args->nesting < 0 || args->nesting >= 8 || args->argc[args->nesting] > 1) {
        say("Usage: unloadzone\n"); return;
    }
    if (!hasLoadedZone) {
        say("UnloadZone: no zone loaded by this command\n"); return;
    }
    if (loading.load(std::memory_order_acquire)) {
        say("UnloadZone: another zone is loading\n"); return;
    }
    loading.store(true, std::memory_order_release);
    XZoneInfo zones[1]{};
    // Engine frees *every* zone carrying this flag, not just activeName.
    zones[0] = XZoneInfo{nullptr, 0, kZoneFlag};
    say("UnloadZone: unloading...\n");
    load(zones, 1, 1); // freeFlags=0x40, synchronous.
    hasLoadedZone = false;
    activeName[0] = '\0';
    activePath[0] = '\0';
    loading.store(false, std::memory_order_release);
    say("UnloadZone: unload call returned.\n");
}
void __cdecl buildPath(const char* filename, int ffDir, int pathlen, char* path) {
    if (filename && path && pathlen > 0 && loading.load(std::memory_order_acquire) && _stricmp(filename, activeName) == 0) {
        strncpy_s(path, static_cast<size_t>(pathlen), activePath, _TRUNCATE);
        return;
    }
    originalBuildPath(filename, ffDir, pathlen, path);
}
void __cdecl initHook() {
    if (!registered) {
        addCommand("loadzone", loadZone);
        addCommand("unloadzone", unloadZone);
        registered = true;
    }
    originalInit();
}
bool moduleCRC(HMODULE mod, uint32_t& out) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(mod, path, MAX_PATH)) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return false;
    unsigned char buffer[8192];
    uint32_t c = 0xffffffffu;
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0) c = crc32_update(c, buffer, n);
    bool ok = ferror(f) == 0;
    fclose(f);
    out = c ^ 0xffffffffu;
    return ok;
}
void initialize(HMODULE self) {
    HMODULE exe = GetModuleHandleA("iw3mp.exe");
    if (!exe || reinterpret_cast<uintptr_t>(exe) != 0x400000) return;

    // print/load/args/findDvar and the language pointer are absolute IW3
    // addresses. They are only used with this exact executable build.
    uint32_t exeCrc = 0;
    if (!moduleCRC(exe, exeCrc) || exeCrc != kIw3mpCRC) {
        OutputDebugStringA("LoadZone: unsupported iw3mp.exe build; engine addresses are unverified.\\n");
        return;
    }

    HMODULE cod4x = GetModuleHandleA("cod4x_021.dll");
    uint32_t crc = 0;
    if (!cod4x || !moduleCRC(cod4x, crc)) {
        OutputDebugStringA("LoadZone: cod4x_021.dll is missing or unreadable.\\n");
        return;
    }
    if (crc != kCRC21_1 && crc != kCRC21_8) {
        OutputDebugStringA("LoadZone: unsupported cod4x_021.dll CRC32.\\n");
        return;
    }

    uintptr_t initRva = 0, buildPathRva = 0, cmdAddRva = 0;
    const bool resolved =
        findUniquePattern(cod4x, kInitPattern, initRva) &&
        findUniquePattern(cod4x, kBuildPathPattern, buildPathRva) &&
        findUniquePattern(cod4x, kCmdAddPattern, cmdAddRva);
    if (!resolved) {
        OutputDebugStringA("LoadZone: required CoD4x signatures were not uniquely found; refusing hooks.\\n");
        return;
    }

    // These byte signatures have currently been verified only against 21.1.
    // 21.8 is recognized, but must not be hooked until its own signatures and
    // function calling conventions have been independently validated.
    if (crc == kCRC21_8) {
        OutputDebugStringA("LoadZone: CoD4x 21.8 detected; its signatures are not validated yet. No hooks installed.\\n");
        return;
    }

    auto base = reinterpret_cast<uintptr_t>(cod4x);
    originalInit = reinterpret_cast<InitFn>(base + initRva);
    originalBuildPath = reinterpret_cast<BuildPathFn>(base + buildPathRva);
    addCommand = reinterpret_cast<AddCommandFn>(base + cmdAddRva);
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCSTR>(self), &pinned)) return;
    LONG status = DetourTransactionBegin();
    if (status != NO_ERROR) return;
    status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&originalInit), reinterpret_cast<PVOID>(initHook));
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&originalBuildPath), reinterpret_cast<PVOID>(buildPath));
    if (status != NO_ERROR) { DetourTransactionAbort(); OutputDebugStringA("LoadZone: hook setup failed.\\n"); return; }
    if (DetourTransactionCommit() != NO_ERROR) OutputDebugStringA("LoadZone: hook commit failed.\\n");
}
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        initialize(instance);
    }
    return TRUE;
}