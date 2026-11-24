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
// Exact offsets for the two supplied CoD4x client builds.
// The CRC is calculated from the DLL contents, not its filename.
struct BuildOffsets {
    uint32_t crc;
    const char* version;
    uintptr_t cmdAddRva;
    uintptr_t initRva;
    uintptr_t buildPathRva;
    uintptr_t validateIwdRva;
};
constexpr BuildOffsets kBuilds[] = {
    // CoD4x 21.1 (CRC32 B1822ED5)
    { 0xb1822ed5u, "21.1", 0x2116cu, 0x8327eu, 0x37fdfu, 0x3aa7cu },
    // CoD4x 21.8 (CRC32 CB77AE89)
    { 0xcb77ae89u, "21.8", 0x5c810u, 0x359b0u, 0x2cf10u, 0x30b40u },
};

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

// --- Для меню (см. common.hpp) ---
HMODULE g_self = nullptr;
const char** g_langPtr = reinterpret_cast<const char**>(0xCC147D4);

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

// Общий загрузчик: используется консолью (loadZone) и меню (zone_menu.cpp).
bool loadZoneByName(const char* name, char* err, size_t errCap) {
    if (!valid_zone_name(name)) {
        _snprintf_s(err, errCap, _TRUNCATE,
            "LoadZone: use 1-64 letters, digits, _ or -; no path or .ff\n");
        return false;
    }
    if (loading.load(std::memory_order_acquire)) {
        _snprintf_s(err, errCap, _TRUNCATE, "LoadZone: another zone is loading\n");
        return false;
    }
    Dvar* home = findDvar("fs_homepath");
    const char* lang = (g_langPtr ? *g_langPtr : nullptr);
    if (!home || !home->current.string || !lang || !*lang) {
        _snprintf_s(err, errCap, _TRUNCATE,
            "LoadZone: fs_homepath or game language is unavailable\n");
        return false;
    }
    int n = _snprintf_s(activePath, sizeof(activePath), _TRUNCATE,
                        "%s\\zone\\%s\\%s.ff", home->current.string, lang, name);
    if (n < 0) {
        _snprintf_s(err, errCap, _TRUNCATE, "LoadZone: path too long\n");
        return false;
    }
    DWORD attrs = GetFileAttributesA(activePath);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        _snprintf_s(err, errCap, _TRUNCATE,
            "LoadZone: .ff not found in fs_homepath/zone/<language>/\n");
        return false;
    }
    HANDLE h = CreateFileA(activePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf_s(err, errCap, _TRUNCATE,
            "LoadZone: cannot open .ff for reading (err %lu)\n", GetLastError());
        return false;
    }
    CloseHandle(h);

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
    return true;
}

bool unloadLoadedZone(char* err, size_t errCap) {
    if (!hasLoadedZone) {
        _snprintf_s(err, errCap, _TRUNCATE,
            "UnloadZone: no zone loaded by this command\n");
        return false;
    }
    if (loading.load(std::memory_order_acquire)) {
        _snprintf_s(err, errCap, _TRUNCATE, "UnloadZone: another zone is loading\n");
        return false;
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
    return true;
}

void __cdecl loadZone() {
    if (args->nesting < 0 || args->nesting >= 8 || args->argc[args->nesting] != 2) {
        say("Usage: loadzone <name_without_ff>\n"); return;
    }
    char err[192];
    if (!loadZoneByName(args->argv[args->nesting][1], err, sizeof(err))) say(err);
}
void __cdecl unloadZone() {
    if (args->nesting < 0 || args->nesting >= 8 || args->argc[args->nesting] > 1) {
        say("Usage: unloadzone\n"); return;
    }
    char err[192];
    if (!unloadLoadedZone(err, sizeof(err))) say(err);
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
        initZoneMenu(g_self); // <-- меню (F6 по умолчанию, клавиша из loadzone.ini)
    }
    originalInit();
}
// Applies a single-byte patch inside the cod4x client module.
bool patchByte(uintptr_t address, unsigned char value) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    *reinterpret_cast<unsigned char*>(address) = value;
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, oldProtect, &ignored);
    return true;
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
    if (reinterpret_cast<uintptr_t>(GetModuleHandleA("iw3mp.exe")) != 0x400000) return;
    HMODULE cod4x = GetModuleHandleA("cod4x_021.dll");
    uint32_t crc = 0;
    if (!cod4x || !moduleCRC(cod4x, crc)) {
        OutputDebugStringA("LoadZone: missing/unreadable cod4x_021.dll.\n");
        return;
    }

    const BuildOffsets* build = nullptr;
    for (const auto& candidate : kBuilds) {
        if (candidate.crc == crc) {
            build = &candidate;
            break;
        }
    }
    if (!build) {
        OutputDebugStringA("LoadZone: unsupported cod4x_021.dll CRC32; supported builds are 21.1 and 21.8.\n");
        return;
    }

    auto base = reinterpret_cast<uintptr_t>(cod4x);
    // Neutralize the startup IWD validation (FS_ValidateIwdFiles): it compares
    // every loaded .iwd against a hardcoded list of stock checksums and shows an
    // "Invalid IWD files detected!" error for any custom archive found in main/.
    // A single RET at the function entry disables the whole check; the function
    // runs long after DLL load, so patching it here is safe.
    if (patchByte(base + build->validateIwdRva, 0xc3)) {
        OutputDebugStringA("LoadZone: startup IWD validation disabled.\n");
    } else {
        OutputDebugStringA("LoadZone: failed to disable startup IWD validation.\n");
    }
    originalInit = reinterpret_cast<InitFn>(base + build->initRva);
    originalBuildPath = reinterpret_cast<BuildPathFn>(base + build->buildPathRva);
    addCommand = reinterpret_cast<AddCommandFn>(base + build->cmdAddRva);
    char message[128];
    _snprintf_s(message, sizeof(message), _TRUNCATE,
                "LoadZone: CoD4x %s detected (CRC32 %08X).\n", build->version, crc);
    OutputDebugStringA(message);
    // ASI loaders may otherwise unload modules after initialization.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCSTR>(self), &pinned)) return;
    g_self = self; // <-- для меню
    LONG status = DetourTransactionBegin();
    if (status != NO_ERROR) return;
    status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&originalInit), reinterpret_cast<PVOID>(initHook));
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&originalBuildPath), reinterpret_cast<PVOID>(buildPath));
    if (status != NO_ERROR) { DetourTransactionAbort(); OutputDebugStringA("LoadZone: hook setup failed.\n"); return; }
    if (DetourTransactionCommit() != NO_ERROR) OutputDebugStringA("LoadZone: hook commit failed.\n");
}
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        initialize(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        shutdownZoneMenu();
    }
    return TRUE;
}