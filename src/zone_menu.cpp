// Adapted from Rex109/CoD4QOL. See README and LICENSE.md.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include "detours.h"
#include "common.hpp"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
constexpr wchar_t kIniName[] = L"loadzone.ini";
constexpr const char* kGameWindowTitle = "Call of Duty 4 X"; // CoD4x

struct Settings { UINT toggleKey; };
Settings g_cfg{ VK_F6 };
bool     g_registered = false;
bool     g_visible    = false;
bool     g_waitingKey = false;
HMODULE  g_self       = nullptr;
char     g_dir[MAX_PATH] = {};

struct Entry { char name[64]; };
Entry g_entries[256];
int   g_count = 0;
int   g_sel   = -1;

using EndSceneFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ResetFn    = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
EndSceneFn g_origEndScene = nullptr;
ResetFn    g_origReset    = nullptr;
IDirect3DDevice9* g_device = nullptr;
bool g_imguiReady = false;
bool g_hooksInstalled = false;
WNDPROC g_origWndProc = nullptr;
HWND    g_gameHwnd = nullptr;
std::atomic<bool> g_loading{false};

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding     = ImVec2(10, 8);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(6, 4);
    s.WindowRounding    = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.06f, 0.06f, 0.96f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.09f, 0.09f, 0.09f, 0.96f);
    c[ImGuiCol_Border]          = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.46f, 0.46f, 0.46f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.13f, 0.17f, 0.23f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.15f, 0.20f, 0.28f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.16f, 0.22f, 0.31f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_Separator]       = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.23f, 0.48f, 0.84f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.23f, 0.48f, 0.84f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.28f, 0.55f, 0.92f, 1.00f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_NavHighlight]    = ImVec4(0.23f, 0.48f, 0.84f, 1.00f);
}

void iniPath(wchar_t* out, size_t cap) {
    GetModuleFileNameW(g_self, out, (DWORD)cap);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
    wcscat_s(out, cap, L"\\");
    wcscat_s(out, cap, kIniName);
}
void loadConfig() {
    wchar_t path[MAX_PATH]; iniPath(path, _countof(path));
    g_cfg.toggleKey = (UINT)GetPrivateProfileIntW(L"menu", L"toggle_key", VK_F6, path);
    if (g_cfg.toggleKey == 0 || g_cfg.toggleKey > 0xFE) g_cfg.toggleKey = VK_F6;
}
void saveConfig() {
    wchar_t path[MAX_PATH]; iniPath(path, _countof(path));
    wchar_t val[16]; swprintf_s(val, L"%u", g_cfg.toggleKey);
    WritePrivateProfileStringW(L"menu", L"toggle_key", val, path);
}

bool resolveUserDir() {
    Dvar* home = findDvar("fs_homepath");
    const char* lang = (g_langPtr ? *g_langPtr : nullptr);
    if (!home || !home->current.string || !lang || !*lang) return false;
    _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE,
                "%s\\zone\\%s\\usermaps", home->current.string, lang);
    CreateDirectoryA(g_dir, nullptr);
    return true;
}
void refreshList() {
    g_count = 0; g_sel = -1;
    if (!resolveUserDir()) return;
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.ff", g_dir);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".ff") != 0) continue;
        size_t len = (size_t)(dot - fd.cFileName);
        if (len == 0 || len >= sizeof(g_entries[0].name)) continue;
        memcpy(g_entries[g_count].name, fd.cFileName, len);
        g_entries[g_count].name[len] = 0;
        if (++g_count >= (int)_countof(g_entries)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (g_count > 0) g_sel = 0;
}

void applyHotkey(HWND hwnd) {
    if (!hwnd) return;
    if (g_registered) { UnregisterHotKey(hwnd, 1); g_registered = false; }
    g_registered = RegisterHotKey(hwnd, 1, 0, g_cfg.toggleKey) != 0;
    if (!g_registered) OutputDebugStringA("LoadZone: hotkey registration failed.\n");
    saveConfig();
}

void drawMenu() {
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(200, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("LOAD ZONE  -  usermaps", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.50f, 0.50f, 1.0f));
    ImGui::TextUnformatted("user .ff only");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (g_count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.50f, 0.50f, 1.0f));
        ImGui::TextWrapped("No user .ff files in:\n%s",
                           g_dir[0] ? g_dir : "(fs_homepath unavailable)");
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginChild("list", ImVec2(0, -60), true);
        for (int i = 0; i < g_count; ++i) {
            bool selected = (i == g_sel);
            if (ImGui::Selectable(g_entries[i].name, selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                g_sel = i;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    char err[192];
                    if (!loadZoneByName(g_entries[i].name, err, sizeof(err))) {
                        OutputDebugStringA(err);
                    } else {
                        g_visible = false;
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.50f, 0.50f, 1.0f));
    ImGui::TextUnformatted(g_waitingKey
        ? "Press a new toggle key (Esc to cancel)"
        : "Enter = load   |   Change key   |   Esc = close");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110);
    if (ImGui::Button(g_waitingKey ? "press key..." : "Change key", ImVec2(110, 0))) {
        g_waitingKey = true;
    }
    ImGui::End();
}

void renderFrame() {
    if (!g_imguiReady || !g_device) return;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_visible) drawMenu();
    if (g_waitingKey) {
        for (int vk = 1; vk <= 0xFE; ++vk) {
            if (vk == VK_ESCAPE) {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_waitingKey = false; break; }
                continue;
            }
            if (ImGui::IsKeyPressed((ImGuiKey)vk, false)) {
                g_cfg.toggleKey = (UINT)vk;
                if (g_gameHwnd) applyHotkey(g_gameHwnd);
                g_waitingKey = false;
                break;
            }
        }
    }
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

HRESULT STDMETHODCALLTYPE hookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_origReset(dev, pp);
    if (SUCCEEDED(hr) && g_imguiReady) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// Инициализация откладывается до первого EndScene: окно и девайс уже готовы.
void tryInitImGui(IDirect3DDevice9* dev) {
    if (g_imguiReady) return;
    g_device = dev;
    if (!g_gameHwnd) {
        g_gameHwnd = FindWindowA(nullptr, kGameWindowTitle);
        if (!g_gameHwnd) { OutputDebugStringA("LoadZone: game window not found.\n"); return; }
    }
    if (!g_origWndProc) {
        g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)menuWndProc);
        if (!g_origWndProc) { OutputDebugStringA("LoadZone: SetWindowLongPtr failed.\n"); return; }
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyStyle();
    ImGui_ImplWin32_Init(g_gameHwnd);
    ImGui_ImplDX9_Init(dev);
    g_imguiReady = true;
    applyHotkey(g_gameHwnd);
    OutputDebugStringA("LoadZone: ImGui ready.\n");
}

HRESULT STDMETHODCALLTYPE hookEndScene(IDirect3DDevice9* dev) {
    if (!g_imguiReady) tryInitImGui(dev);
    if (g_imguiReady) renderFrame();
    return g_origEndScene(dev);
}

LRESULT CALLBACK menuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imguiReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (msg == WM_HOTKEY && wp == 1) {
        g_visible = !g_visible;
        if (g_visible) {
            refreshList();
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }
        return 0;
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

// Получить vtable EndScene/Reset через временный D3D9-девайс (если окно уже есть).
bool installHooks() {
    HWND hwnd = FindWindowA(nullptr, kGameWindowTitle);
    if (!hwnd) { OutputDebugStringA("LoadZone: no window for hook install.\n"); return false; }
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    d3d->Release();
    if (FAILED(hr) || !dev) { OutputDebugStringA("LoadZone: temp device failed.\n"); return false; }
    void** vt = *reinterpret_cast<void***>(dev);
    g_origEndScene = reinterpret_cast<EndSceneFn>(vt[42]);
    g_origReset    = reinterpret_cast<ResetFn>(vt[16]);
    dev->Release();

    LONG st = DetourTransactionBegin();
    if (st != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&g_origEndScene), reinterpret_cast<PVOID>(hookEndScene));
    DetourAttach(reinterpret_cast<PVOID*>(&g_origReset),    reinterpret_cast<PVOID>(hookReset));
    if (DetourTransactionCommit() != NO_ERROR) {
        OutputDebugStringA("LoadZone: DX9 hook commit failed.\n");
        return false;
    }
    g_hooksInstalled = true;
    OutputDebugStringA("LoadZone: DX9 hooks installed.\n");
    return true;
}
} // namespace

void initZoneMenu(void* moduleHandle) {
    g_self = (HMODULE)moduleHandle;
    loadConfig();
    // Хуки ставим сразу — иначе не получим первый EndScene.
    if (!installHooks()) {
        OutputDebugStringA("LoadZone: installHooks failed; menu will not initialize.\n");
    }
}

void shutdownZoneMenu() {
    if (g_gameHwnd) {
        if (g_registered) { UnregisterHotKey(g_gameHwnd, 1); g_registered = false; }
        if (g_origWndProc) { SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc); g_origWndProc = nullptr; }
    }
    if (g_imguiReady) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
}