// Adapted from Rex109/CoD4QOL. See README and LICENSE.md.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "common.hpp"

namespace {
constexpr UINT kIdHotkey  = 0x5A01;
constexpr int  kIdList    = 1001;
constexpr int  kIdKeyBtn  = 1002;
constexpr wchar_t kIniName[] = L"loadzone.ini";

struct Settings { UINT toggleKey; };
Settings g_cfg{ VK_F6 };
HWND     g_hwnd       = nullptr;
HWND     g_list       = nullptr;
HMODULE  g_self       = nullptr;
bool     g_registered = false;
bool     g_waitingKey = false;
char     g_dir[MAX_PATH] = {};

struct Entry { char name[64]; };
Entry g_entries[256];
int   g_count = 0;

// Палитра в духе skeet.cc: тёмная панель, аккуратный акцент, без логотипов.
const COLORREF kBg     = RGB(0x14,0x14,0x14);
const COLORREF kPanel  = RGB(0x1b,0x1b,0x1b);
const COLORREF kBorder = RGB(0x2a,0x2a,0x2a);
const COLORREF kText   = RGB(0xd8,0xd8,0xd8);
const COLORREF kMuted  = RGB(0x7a,0x7a,0x7a);
const COLORREF kAccent = RGB(0x3a,0x7b,0xd5);
constexpr int kPad = 12;

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
    _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "%s\\zone\\%s",
                home->current.string, lang);
    return true;
}

void refreshList() {
    g_count = 0;
    if (g_list) SendMessageA(g_list, LB_RESETCONTENT, 0, 0);
    if (!resolveUserDir()) { InvalidateRect(g_hwnd, nullptr, TRUE); return; }
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.ff", g_dir);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { InvalidateRect(g_hwnd, nullptr, TRUE); return; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".ff") != 0) continue; // явный фильтр расширения
        size_t len = (size_t)(dot - fd.cFileName);
        if (len == 0 || len >= sizeof(g_entries[0].name)) continue;
        memcpy(g_entries[g_count].name, fd.cFileName, len);
        g_entries[g_count].name[len] = 0;
        if (g_list) SendMessageA(g_list, LB_ADDSTRING, 0,
                                 (LPARAM)g_entries[g_count].name);
        if (++g_count >= (int)_countof(g_entries)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

void tryLoadSelected() {
    int idx = g_list ? (int)SendMessageA(g_list, LB_GETCURSEL, 0, 0) : -1;
    if (idx < 0 || idx >= g_count) return;
    char err[192];
    if (!loadZoneByName(g_entries[idx].name, err, sizeof(err))) {
        // Ошибка — показываем в заголовке, загрузка не запускается,
        // предыдущая зона не выгружается (см. loadZoneByName).
        SetWindowTextA(g_hwnd, err);
        InvalidateRect(g_hwnd, nullptr, TRUE);
    } else {
        SetWindowTextA(g_hwnd, "LoadZone");
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

void drawPanel(HDC dc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &rc, bg); DeleteObject(bg);
    RECT panel = rc; InflateRect(&panel, -kPad, -kPad);
    HBRUSH pb = CreateSolidBrush(kPanel);
    FillRect(dc, &panel, pb); DeleteObject(pb);
    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBr  = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);
    SelectObject(dc, oldBr); SelectObject(dc, oldPen); DeleteObject(border);

    RECT accent = { panel.left, panel.top, panel.left + 3, panel.bottom };
    HBRUSH ab = CreateSolidBrush(kAccent);
    FillRect(dc, &accent, ab); DeleteObject(ab);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    HFONT f = CreateFontA(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, f);
    RECT title = { panel.left + 14, panel.top + 10, panel.right - 14, panel.top + 34 };
    DrawTextA(dc, "LOAD ZONE  -  user .ff", -1, &title,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(dc, kMuted);
    RECT hint = { panel.left + 14, panel.bottom - 26, panel.right - 14, panel.bottom - 8 };
    char hintText[128];
    _snprintf_s(hintText, sizeof(hintText), _TRUNCATE,
                "Enter = load   |   %s = toggle   |   Esc = close",
                g_waitingKey ? "press new key" : "F6/custom");
    DrawTextA(dc, hintText, -1, &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    if (g_count == 0) {
        SetTextColor(dc, kMuted);
        RECT empty = { panel.left + 18, panel.top + 70, panel.right - 18, panel.bottom - 40 };
        char msg[384];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "No user .ff files in:\n%s", g_dir[0] ? g_dir : "(fs_homepath unavailable)");
        DrawTextA(dc, msg, -1, &empty, DT_LEFT | DT_WORDBREAK);
    }
    SelectObject(dc, oldFont); DeleteObject(f);
}

void applyHotkey() {
    if (g_registered) { UnregisterHotKey(g_hwnd, kIdHotkey); g_registered = false; }
    g_registered = RegisterHotKey(g_hwnd, kIdHotkey, 0, g_cfg.toggleKey) != 0;
    if (!g_registered) OutputDebugStringA("LoadZone: hotkey registration failed.\n");
    saveConfig();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_list = CreateWindowExA(0, "LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            kPad + 8, 48, 360, 280, hwnd, (HMENU)kIdList, nullptr, nullptr);
        CreateWindowExA(0, "BUTTON", "Change key", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            kPad + 8, 336, 120, 26, hwnd, (HMENU)kIdKeyBtn, nullptr, nullptr);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        drawPanel(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kIdList && HIWORD(wp) == LBN_DBLCLK) tryLoadSelected();
        if (LOWORD(wp) == kIdKeyBtn) {
            g_waitingKey = true;
            SetWindowTextA(hwnd, "Press a new toggle key (Esc to cancel)");
            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_KEYDOWN:
        if (g_waitingKey) {
            UINT vk = (UINT)wp;
            g_waitingKey = false;
            if (vk == VK_ESCAPE) {
                SetWindowTextA(hwnd, "LoadZone");
            } else {
                g_cfg.toggleKey = vk;
                applyHotkey();
                SetWindowTextA(hwnd, "LoadZone");
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        if (wp == VK_RETURN) tryLoadSelected();
        if (wp == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_HOTKEY:
        if (wp == kIdHotkey) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else { refreshList(); ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_DESTROY:
        if (g_registered) { UnregisterHotKey(hwnd, kIdHotkey); g_registered = false; }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
} // namespace

void initZoneMenu(void* moduleHandle) {
    if (g_hwnd) return;
    g_self = (HMODULE)moduleHandle;
    loadConfig();

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_self;
    wc.lpszClassName = "LoadZoneMenuClassW";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        OutputDebugStringA("LoadZone: RegisterClassEx failed.\n");
        return;
    }
    g_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "LoadZoneMenuClassW",
        "LoadZone", WS_POPUP | WS_BORDER, 200, 200, 400, 380,
        nullptr, nullptr, g_self, nullptr);
    if (!g_hwnd) { OutputDebugStringA("LoadZone: menu window failed.\n"); return; }
    applyHotkey();
    refreshList();
}

void shutdownZoneMenu() {
    if (!g_hwnd) return;
    if (g_registered) { UnregisterHotKey(g_hwnd, kIdHotkey); g_registered = false; }
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr; g_list = nullptr;
}