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

// Глобальные переменные темы, которые ожидает модифицированный imgui_widgets.cpp
// (extern-объявления внутри third_party/imgui). Без них линковка падает с LNK2019.
float   content_animation = 1.0f;                                  // статическая альфа виджетов
float   accent_colour[4]  = { 0.87f, 0.65f, 0.72f, 1.00f };        // розовый акцент темы
ImFont* poppins           = nullptr;                               // кастомные шрифты (не используются меню)
ImFont* font_icon         = nullptr;

namespace {
constexpr wchar_t kIniName[] = L"loadzone.ini";
constexpr const char* kGameWindowTitle = "Call of Duty 4 X"; // CoD4x
constexpr UINT kMsgLoadZone = WM_APP + 100; // загрузка в потоке окна (игровом)

struct Settings { UINT toggleKey; };
Settings g_cfg{ VK_F6 };
bool     g_registered = false;
bool     g_visible    = false;
bool     g_cursorShown = false;
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

LRESULT CALLBACK menuWndProc(HWND, UINT, WPARAM, LPARAM);
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
    s.WindowPadding     = ImVec2(14, 12);
    s.FramePadding      = ImVec2(12, 7);
    s.ItemSpacing       = ImVec2(10, 8);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.WindowRounding    = 9.0f;
    s.ChildRounding     = 7.0f;
    s.FrameRounding     = 7.0f;
    s.GrabRounding      = 7.0f;
    s.ScrollbarRounding = 9.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    s.GrabMinSize       = 12.0f;
    s.ScrollbarSize     = 10.0f;
    ImVec4* c = s.Colors;
    // Тёмная тема с розовым акцентом (как на референсе)
    const ImVec4 pink        = ImVec4(0.87f, 0.65f, 0.72f, 1.00f); // основной розовый
    const ImVec4 pink_hover  = ImVec4(0.93f, 0.72f, 0.79f, 1.00f);
    const ImVec4 pink_active = ImVec4(0.97f, 0.79f, 0.85f, 1.00f);
    c[ImGuiCol_WindowBg]        = ImVec4(0.055f, 0.055f, 0.065f, 0.98f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.070f, 0.070f, 0.082f, 1.00f);
    c[ImGuiCol_Border]          = ImVec4(0.150f, 0.150f, 0.170f, 1.00f);
    c[ImGuiCol_BorderShadow]    = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Text]            = ImVec4(0.84f, 0.84f, 0.87f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.42f, 0.42f, 0.47f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.17f, 0.17f, 0.19f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.87f, 0.65f, 0.72f, 0.28f);
    c[ImGuiCol_Button]          = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.87f, 0.65f, 0.72f, 0.22f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.87f, 0.65f, 0.72f, 0.38f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.085f, 0.085f, 0.098f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.11f, 0.11f, 0.125f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0.055f, 0.055f, 0.065f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.34f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = pink;
    c[ImGuiCol_Separator]       = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_SeparatorHovered]= pink_hover;
    c[ImGuiCol_SeparatorActive] = pink_active;
    c[ImGuiCol_CheckMark]       = pink;
    c[ImGuiCol_SliderGrab]      = pink;
    c[ImGuiCol_SliderGrabActive]= pink_active;
    c[ImGuiCol_TitleBg]         = ImVec4(0.055f, 0.055f, 0.065f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.055f, 0.055f, 0.065f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]= ImVec4(0.055f, 0.055f, 0.065f, 1.00f);
    c[ImGuiCol_MenuBarBg]       = ImVec4(0.070f, 0.070f, 0.082f, 1.00f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.085f, 0.085f, 0.098f, 0.98f);
    c[ImGuiCol_Tab]             = ImVec4(0.085f, 0.085f, 0.098f, 1.00f);
    c[ImGuiCol_TabHovered]      = pink;
    c[ImGuiCol_TabActive]       = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_NavHighlight]    = pink;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
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
    saveConfig();
}

void setCursorVisible(bool visible) {
    if (visible == g_cursorShown) return;
    if (visible) {
        ClipCursor(nullptr);
        ReleaseCapture();
        while (ShowCursor(TRUE) < 0) {}
        g_cursorShown = true;
    } else {
        while (ShowCursor(FALSE) >= 0) {}
        g_cursorShown = false;
    }
}

void requestLoad(int idx) {
    if (idx < 0 || idx >= g_count) return;
    if (!g_gameHwnd) return;
    PostMessageA(g_gameHwnd, kMsgLoadZone, 0, (LPARAM)idx);
}

void drawMenu() {
    ImGui::SetNextWindowSize(ImVec2(830, 515), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(200, 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("LOAD ZONE  -  .ff", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.87f, 0.65f, 0.72f, 1.0f));
    ImGui::TextUnformatted("custom .ff files");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (g_count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.87f, 0.65f, 0.72f, 1.0f));
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
    if (ImGui::Button("Load", ImVec2(90, 0))) requestLoad(g_sel);
    if (!canLoad) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Minimize game", ImVec2(130, 0))) {
        if (g_gameHwnd) ShowWindow(g_gameHwnd, SW_MINIMIZE);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.65f, 1.0f));
    ImGui::TextUnformatted(g_status[0] ? g_status :
        "Enter/double-click = load   |   F6/custom = toggle   |   Esc = close");
    ImGui::PopStyleColor();
    ImGui::End();
}

void renderFrame() {
    if (!g_imguiReady || !g_device) return;
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
    if (!g_origWndProc) {
        g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)menuWndProc);
        if (!g_origWndProc) { OutputDebugStringA("LoadZone: SetWindowLongPtr failed.\n"); return; }
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyStyle();
    ImGuiIO& io = ImGui::GetIO();
    poppins   = io.FontDefault; // подмена для кастомных виджетов, если вдруг будут вызваны
    font_icon = io.FontDefault;
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

LRESULT CALLBACK menuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_visible && msg == WM_INPUT) return 0;
    if (g_imguiReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;

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
    if (msg == WM_HOTKEY && wp == 1) {
        g_visible = !g_visible;
        if (g_visible) {
            refreshList();
            setCursorVisible(true);
            // Не форсируем foreground — иначе окно «дёргается» и мешает Alt+Tab.
            if (GetForegroundWindow() == hwnd) SetFocus(hwnd);
        } else {
            setCursorVisible(false);
        }
        return 0;
    }
    if (msg == WM_ACTIVATEAPP && wp == FALSE) {
        // Alt+Tab: окно теряет активацию — скрываем меню и явно сворачиваем игру,
        // чтобы она гарантированно уходила в трей.
        if (g_visible) { g_visible = false; setCursorVisible(false); }
        ShowWindow(hwnd, SW_MINIMIZE);
        return 0;
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
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