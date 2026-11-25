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

constexpr UINT kMsgLoadZone = WM_APP + 100;
constexpr UINT kMsgMinimize = WM_APP + 101;
constexpr UINT kMsgRestore  = WM_APP + 102;

struct Settings { UINT toggleKey; };
Settings g_cfg{ VK_F6 };
bool     g_registered = false;
bool     g_visible    = false;
bool     g_cursorShown = false;
bool     g_minimized   = false;
HMODULE  g_self       = nullptr;
char     g_dir[MAX_PATH] = {};
char     g_status[192] = {};

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

// --- fullscreen/minimize state ---
D3DPRESENT_PARAMETERS g_origPP{};       // исходные параметры (как создала игра)
bool  g_haveOrigPP        = false;
bool  g_wasFullscreen     = false;      // были ли в fullscreen до сворачивания
bool  g_pendingMinimize   = false;      // нужно перевести в windowed + свернуть
bool  g_pendingRestore    = false;      // нужно вернуть fullscreen
HHOOK g_kbHook            = nullptr;
std::atomic<bool> g_winDown{false};

LRESULT CALLBACK menuWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK keyboardHookProc(int, WPARAM, LPARAM);
void tryInitImGui(IDirect3DDevice9* dev);

bool is_stock_zone(const char* nameNoExt) {
    static const char* kStock[] = {
        "iw3mp", "common_mp", "code_post_gfx", "code_post_gfx_mp", "ui_mp",
        "killhouse_mp", "mp_backlot", "mp_bloc", "mp_bog", "mp_cargoship",
        "mp_citystreets", "mp_convoy", "mp_countdown", "mp_crash",
        "mp_crossfire", "mp_farm", "mp_overgrown", "mp_pipeline", "mp_shipment",
        "mp_showdown", "mp_strike", "mp_vacant",
    };
    for (const char* s : kStock) {
        if (_stricmp(nameNoExt, s) == 0) return true;
    }
    if (_strnicmp(nameNoExt, "mp_", 3) == 0) return true;
    return false;
}

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding     = ImVec2(12, 10);
    s.FramePadding      = ImVec2(9, 5);
    s.ItemSpacing       = ImVec2(8, 5);
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.08f, 0.09f, 0.11f, 0.97f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.11f, 0.12f, 0.14f, 0.97f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.08f, 0.09f, 0.11f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(0.24f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.18f, 0.24f, 0.34f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.22f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.26f, 0.36f, 0.50f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.24f, 0.30f, 0.40f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.30f, 0.40f, 0.54f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.17f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.21f, 0.26f, 0.33f, 1.00f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.28f, 0.32f, 0.38f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.42f, 0.50f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.44f, 0.52f, 0.62f, 1.00f);
    c[ImGuiCol_Separator]       = ImVec4(0.24f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.38f, 0.62f, 0.92f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.38f, 0.62f, 0.92f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.46f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.14f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]= ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_NavHighlight]    = ImVec4(0.38f, 0.62f, 0.92f, 1.00f);
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

bool resolveUserDir() {
    Dvar* home = findDvar("fs_homepath");
    const char* lang = (g_langPtr ? *g_langPtr : nullptr);
    if (!home || !home->current.string || !lang || !*lang) return false;
    _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE,
                "%s\\zone\\%s", home->current.string, lang);
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
        char name[64];
        memcpy(name, fd.cFileName, len); name[len] = 0;
        if (is_stock_zone(name)) continue;
        memcpy(g_entries[g_count].name, name, len + 1);
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
}

void setCursorVisible(bool visible) {
    if (visible == g_cursorShown) return;
    if (visible) {
        ClipCursor(nullptr);
        ReleaseCapture();
        SetCursor(LoadCursorA(nullptr, IDC_ARROW));
        g_cursorShown = true;
    } else {
        SetCursor(NULL);
        g_cursorShown = false;
    }
}

void requestLoad(int idx) {
    if (idx < 0 || idx >= g_count) return;
    if (!g_gameHwnd) return;
    PostMessageA(g_gameHwnd, kMsgLoadZone, 0, (LPARAM)idx);
}

// --- Полноценное сворачивание эксклюзивного fullscreen D3D9 ---
// 1. Перевести device в windowed через Reset (только в потоке окна!).
// 2. ShowWindow(SW_MINIMIZE) — теперь окно обычное и ОС его свернёт.
// 3. При восстановлении — Reset обратно в fullscreen.

bool applyReset(const D3DPRESENT_PARAMETERS& pp) {
    if (!g_device) return false;
    if (g_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_device->Reset(&pp);
    if (g_imguiReady) ImGui_ImplDX9_CreateDeviceObjects();
    return SUCCEEDED(hr);
}

void doMinimizeGame() {
    if (!g_device || !g_gameHwnd) return;
    if (!g_haveOrigPP) return; // ещё не знаем исходный режим — игра сама справится
    if (g_minimized) return;

    // Прячем меню и курсор
    g_visible = false;
    setCursorVisible(false);

    // Запоминаем, были ли в fullscreen (по исходным параметрам)
    g_wasFullscreen = (g_origPP.Windowed == FALSE);

    if (g_wasFullscreen) {
        // Переводим device в windowed, чтобы окно стало обычным и ОС смогла его свернуть
        D3DPRESENT_PARAMETERS pp = g_origPP;
        pp.Windowed = TRUE;
        pp.FullScreen_RefreshRateInHz = 0;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        if (!applyReset(pp)) {
            OutputDebugStringA("LoadZone: Reset to windowed failed, fallback to SW_MINIMIZE.\n");
            // Fallback: хотя бы попробуем штатное сворачивание
            PostMessageA(g_gameHwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            return;
        }
    }

    // Теперь окно обычное — штатное сворачивание работает
    g_minimized = true;
    ShowWindow(g_gameHwnd, SW_MINIMIZE);
    PostMessageA(g_gameHwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
}

void doRestoreGame() {
    if (!g_device || !g_gameHwnd) return;
    if (!g_minimized) return;
    if (!g_wasFullscreen) {
        g_minimized = false;
        return;
    }
    // Возвращаем исходный fullscreen
    if (applyReset(g_origPP)) {
        g_minimized = false;
        g_wasFullscreen = false;
    } else {
        OutputDebugStringA("LoadZone: Reset back to fullscreen failed.\n");
        g_minimized = false;
        g_wasFullscreen = false;
    }
}

void drawMenu() {
    ImGui::SetNextWindowSize(ImVec2(480, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(200, 180), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("LOAD ZONE  -  .ff", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.66f, 0.74f, 1.0f));
    ImGui::TextUnformatted("custom .ff files");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (g_count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
        ImGui::TextWrapped("No custom .ff files in:\n%s",
                           g_dir[0] ? g_dir : "(fs_homepath unavailable)");
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginChild("list", ImVec2(0, -64), true);
        for (int i = 0; i < g_count; ++i) {
            bool selected = (i == g_sel);
            if (ImGui::Selectable(g_entries[i].name, selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                g_sel = i;
                if (ImGui::IsMouseDoubleClicked(0)) requestLoad(i);
            }
        }
        ImGui::EndChild();
    }
    ImGui::Separator();
    bool canLoad = (g_sel >= 0 && g_sel < g_count);
    if (!canLoad) ImGui::BeginDisabled();
    if (ImGui::Button("Load", ImVec2(100, 0))) requestLoad(g_sel);
    if (!canLoad) ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
    ImGui::TextUnformatted(g_status[0] ? g_status :
        "Enter/double-click = load   |   hotkey = toggle   |   Esc = close");
    ImGui::PopStyleColor();
    ImGui::End();
}

void renderFrame() {
    if (!g_imguiReady || !g_device) return;
    if (g_minimized || (g_gameHwnd && IsIconic(g_gameHwnd))) return;

    ImGuiIO& io = ImGui::GetIO();
    if (g_visible) {
        POINT p{};
        if (GetCursorPos(&p) && ScreenToClient(g_gameHwnd, &p)) {
            io.MousePos = ImVec2((float)p.x, (float)p.y);
        }
        io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    }
    io.MouseDrawCursor = g_visible;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (g_visible) {
        POINT p{};
        if (GetCursorPos(&p) && ScreenToClient(g_gameHwnd, &p)) {
            io.MousePos = ImVec2((float)p.x, (float)p.y);
        }
    }
    ImGui::NewFrame();
    if (g_visible) drawMenu();
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

void tryInitImGui(IDirect3DDevice9* dev) {
    if (g_imguiReady) return;
    g_device = dev;
    if (!g_gameHwnd) {
        g_gameHwnd = FindWindowA(nullptr, kGameWindowTitle);
        if (!g_gameHwnd) { OutputDebugStringA("LoadZone: game window not found.\n"); return; }
    }
    // Запоминаем исходные D3DPRESENT_PARAMETERS с swapchain'а игры.
    // Это нужно, чтобы вернуть именно тот fullscreen-режим, который был.
    if (!g_haveOrigPP) {
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
            if (SUCCEEDED(sc->GetPresentParameters(&g_origPP))) {
                g_haveOrigPP = true;
                char msg[128];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "LoadZone: orig PP %ux%u windowed=%d refresh=%u\n",
                    g_origPP.BackBufferWidth, g_origPP.BackBufferHeight,
                    (int)g_origPP.Windowed, g_origPP.FullScreen_RefreshRateInHz);
                OutputDebugStringA(msg);
            }
            sc->Release();
        }
    }
    if (!g_origWndProc) {
        g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)menuWndProc);
        if (!g_origWndProc) { OutputDebugStringA("LoadZone: SetWindowLongPtr failed.\n"); return; }
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyStyle();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
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

// Низкоуровневый хук: ловим Win+D / Win+M. Просто Win не трогаем (она открывает Пуск).
LRESULT CALLBACK keyboardHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool isUp   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
        if (kb->vkCode == VK_LWIN || kb->vkCode == VK_RWIN) {
            g_winDown.store(isDown);
        } else if (isDown && g_winDown.load()) {
            if (kb->vkCode == 'D' || kb->vkCode == 'M') {
                // Win+D / Win+M — просим поток окна перевести в windowed и свернуть.
                if (g_gameHwnd) PostMessageA(g_gameHwnd, kMsgMinimize, 0, 0);
            }
        } else if (isUp) {
            // ничего
        }
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

LRESULT CALLBACK trackedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK menuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Системные сообщения всегда отдаём игре.
    if (msg == WM_SYSCOMMAND || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
        msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE || msg == WM_NCACTIVATE ||
        msg == WM_SIZE || msg == WM_KILLFOCUS || msg == WM_SETFOCUS) {
        return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
    }

    if (g_visible && msg == WM_INPUT) return 0;

    if (g_imguiReady && g_visible &&
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;

    if (msg == kMsgLoadZone) {
        int idx = (int)lp;
        if (idx >= 0 && idx < g_count) {
            if (g_loading.load(std::memory_order_acquire)) {
                _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "another zone is loading");
            } else {
                char err[192];
                if (loadZoneByName(g_entries[idx].name, err, sizeof(err))) {
                    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "loaded: %s",
                                g_entries[idx].name);
                    g_visible = false;
                    setCursorVisible(false);
                } else {
                    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "%s", err);
                    OutputDebugStringA(err);
                }
            }
        }
        return 0;
    }
    if (msg == kMsgMinimize) {
        doMinimizeGame();
        return 0;
    }
    if (msg == kMsgRestore) {
        doRestoreGame();
        return 0;
    }
    if (msg == WM_HOTKEY && wp == 1) {
        g_visible = !g_visible;
        if (g_visible) {
            refreshList();
            setCursorVisible(true);
            if (GetForegroundWindow() == hwnd) SetFocus(hwnd);
        } else {
            setCursorVisible(false);
        }
        return 0;
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

// Отслеживаем свёрнутость/восстановление, чтобы:
// - при сворачивании спрятать меню и не рисовать;
// - при восстановлении вернуть fullscreen (doRestoreGame).
LRESULT CALLBACK trackedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE) {
        if (wp == SIZE_MINIMIZED) {
            g_minimized = true;
            g_visible = false;
            setCursorVisible(false);
        } else if (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) {
            if (g_minimized && g_wasFullscreen) {
                // Просим вернуть fullscreen в потоке окна (мы уже в нём).
                PostMessageA(hwnd, kMsgRestore, 0, 0);
            } else {
                g_minimized = false;
            }
        }
    } else if (msg == WM_ACTIVATEAPP && wp == FALSE) {
        if (g_visible) { g_visible = false; setCursorVisible(false); }
    }
    return menuWndProc(hwnd, msg, wp, lp);
}

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

    // Низкоуровневый хук клавиатуры для Win+D / Win+M.
    g_kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleA(nullptr), 0);
    if (!g_kbHook) OutputDebugStringA("LoadZone: keyboard hook failed.\n");

    OutputDebugStringA("LoadZone: DX9 hooks installed.\n");
    return true;
}
} // namespace

void initZoneMenu(void* moduleHandle) {
    g_self = (HMODULE)moduleHandle;
    loadConfig();
    if (!installHooks()) {
        OutputDebugStringA("LoadZone: installHooks failed; menu will not initialize.\n");
    }
}

void shutdownZoneMenu() {
    setCursorVisible(false);
    g_minimized = false;
    g_wasFullscreen = false;
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
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