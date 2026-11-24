// Adapted from Rex109/CoD4QOL. See README and LICENSE.md.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "common.hpp"

namespace {
constexpr UINT kIdHotkey = 0x5A01;
constexpr wchar_t kIniName[] = L"loadzone.ini";

template <typename T, size_t N>
constexpr size_t countof(const T (&)[N]) { return N; }

struct Settings { UINT toggleKey; };
Settings g_cfg{ VK_F6 };
HWND     g_hwnd       = nullptr;
HMODULE  g_self       = nullptr;
bool     g_registered = false;
bool     g_waitingKey = false;
char     g_dir[MAX_PATH] = {};

struct Entry { char name[64]; };
Entry g_entries[256];
int   g_count  = 0;
int   g_sel    = -1;
int   g_hover  = -1;
int   g_scroll = 0;

// Геометрия панели (в клиентских координатах окна)
constexpr int kPad      = 12;
constexpr int kTitleH   = 28;
constexpr int kRowH     = 22;
constexpr int kFooterH  = 28;
constexpr int kListTop  = kPad + kTitleH + 4;
constexpr int kBtnW     = 110;
constexpr int kBtnH     = 24;
constexpr int kWinW     = 420;
constexpr int kWinH     = 360;

const COLORREF kBg     = RGB(0x10,0x10,0x10);
const COLORREF kPanel  = RGB(0x18,0x18,0x18);
const COLORREF kBorder = RGB(0x2a,0x2a,0x2a);
const COLORREF kText   = RGB(0xd8,0xd8,0xd8);
const COLORREF kMuted  = RGB(0x76,0x76,0x76);
const COLORREF kAccent = RGB(0x3a,0x7b,0xd5);
const COLORREF kSelBg  = RGB(0x22,0x2c,0x3a);
const COLORREF kHover  = RGB(0x1f,0x1f,0x1f);

void iniPath(wchar_t* out, size_t cap) {
    GetModuleFileNameW(g_self, out, (DWORD)cap);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
    wcscat_s(out, cap, L"\\");
    wcscat_s(out, cap, kIniName);
}
void loadConfig() {
    wchar_t path[MAX_PATH]; iniPath(path, countof(path));
    g_cfg.toggleKey = (UINT)GetPrivateProfileIntW(L"menu", L"toggle_key", VK_F6, path);
    if (g_cfg.toggleKey == 0 || g_cfg.toggleKey > 0xFE) g_cfg.toggleKey = VK_F6;
}
void saveConfig() {
    wchar_t path[MAX_PATH]; iniPath(path, countof(path));
    wchar_t val[16]; swprintf_s(val, L"%u", g_cfg.toggleKey);
    WritePrivateProfileStringW(L"menu", L"toggle_key", val, path);
}

bool resolveUserDir() {
    Dvar* home = findDvar("fs_homepath");
    const char* lang = (g_langPtr ? *g_langPtr : nullptr);
    if (!home || !home->current.string || !lang || !*lang) return false;
    // Только пользовательская подпапка: стандартные зоны игры лежат в zone\<lang>\*.ff
    _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE,
                "%s\\zone\\%s\\usermaps", home->current.string, lang);
    CreateDirectoryA(g_dir, nullptr);
    return true;
}

void refreshList() {
    g_count = 0; g_sel = -1; g_hover = -1; g_scroll = 0;
    if (!resolveUserDir()) { InvalidateRect(g_hwnd, nullptr, TRUE); return; }
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.ff", g_dir);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { InvalidateRect(g_hwnd, nullptr, TRUE); return; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".ff") != 0) continue;
        size_t len = (size_t)(dot - fd.cFileName);
        if (len == 0 || len >= sizeof(g_entries[0].name)) continue;
        memcpy(g_entries[g_count].name, fd.cFileName, len);
        g_entries[g_count].name[len] = 0;
        if (++g_count >= (int)countof(g_entries)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (g_count > 0) g_sel = 0;
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

int listBottom() { return kWinH - kPad - kFooterH; }
int visibleRows() { return (listBottom() - kListTop) / kRowH; }
int maxScroll() { return g_count > visibleRows() ? g_count - visibleRows() : 0; }
RECT keyBtnRect() {
    RECT r = { kWinW - kPad - kBtnW, kWinH - kPad - kFooterH + 2,
               kWinW - kPad,             kWinH - kPad - kFooterH + 2 + kBtnH };
    return r;
}

void tryLoadSelected() {
    if (g_sel < 0 || g_sel >= g_count) return;
    char err[192];
    if (!loadZoneByName(g_entries[g_sel].name, err, sizeof(err))) {
        SetWindowTextA(g_hwnd, err);
        InvalidateRect(g_hwnd, nullptr, TRUE);
    } else {
        SetWindowTextA(g_hwnd, "LoadZone");
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

void drawPanel(HDC dc, const RECT& rc) {
    // фон
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &rc, bg); DeleteObject(bg);

    // панель
    RECT panel = { kPad, kPad, rc.right - kPad, rc.bottom - kPad };
    HBRUSH pb = CreateSolidBrush(kPanel);
    FillRect(dc, &panel, pb); DeleteObject(pb);

    // рамка
    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBr  = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);
    SelectObject(dc, oldBr); SelectObject(dc, oldPen); DeleteObject(border);

    // акцентная полоса слева
    RECT accent = { panel.left, panel.top, panel.left + 2, panel.bottom };
    HBRUSH ab = CreateSolidBrush(kAccent);
    FillRect(dc, &accent, ab); DeleteObject(ab);

    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontA(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, f);

    // заголовок
    SetTextColor(dc, kText);
    RECT title = { panel.left + 12, panel.top + 6, panel.right - 12, panel.top + kTitleH };
    DrawTextA(dc, "LOAD ZONE  \xE2\x80\xA2  usermaps", -1, &title,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // список
    int vis = visibleRows();
    if (g_count == 0) {
        SetTextColor(dc, kMuted);
        RECT empty = { panel.left + 14, kListTop + 8, panel.right - 14, listBottom() - 8 };
        char msg[384];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "No user .ff files in:\n%s", g_dir[0] ? g_dir : "(fs_homepath unavailable)");
        DrawTextA(dc, msg, -1, &empty, DT_LEFT | DT_WORDBREAK);
    } else {
        for (int i = 0; i < vis; ++i) {
            int idx = g_scroll + i;
            if (idx >= g_count) break;
            RECT row = { panel.left + 4, kListTop + i * kRowH,
                         panel.right - 4, kListTop + (i + 1) * kRowH - 2 };
            if (idx == g_sel) {
                HBRUSH sb = CreateSolidBrush(kSelBg); FillRect(dc, &row, sb); DeleteObject(sb);
                RECT bar = { row.left, row.top, row.left + 2, row.bottom };
                HBRUSH bb = CreateSolidBrush(kAccent); FillRect(dc, &bar, bb); DeleteObject(bb);
            } else if (idx == g_hover) {
                HBRUSH hb = CreateSolidBrush(kHover); FillRect(dc, &row, hb); DeleteObject(hb);
            }
            SetTextColor(dc, idx == g_sel ? kText : kMuted);
            RECT tx = row; tx.left += 10;
            DrawTextA(dc, g_entries[idx].name, -1, &tx,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        // индикатор скролла
        if (maxScroll() > 0) {
            int trackTop = kListTop, trackBot = listBottom();
            int trackH = trackBot - trackTop;
            int thumbH = trackH * vis / g_count;
            if (thumbH < 16) thumbH = 16;
            int thumbY = trackTop + (trackH - thumbH) * g_scroll / maxScroll();
            RECT thumb = { panel.right - 6, thumbY, panel.right - 3, thumbY + thumbH };
            HBRUSH tb = CreateSolidBrush(kBorder); FillRect(dc, &thumb, tb); DeleteObject(tb);
        }
    }

    // футер: подсказка + кнопка смены клавиши
    SetTextColor(dc, kMuted);
    RECT hint = { panel.left + 12, listBottom() + 4, keyBtnRect().left - 8, kWinH - kPad };
    char hintText[128];
    _snprintf_s(hintText, sizeof(hintText), _TRUNCATE,
                "Enter = load   |   %s = toggle   |   Esc = close",
                g_waitingKey ? "press new key" : "F6/custom");
    DrawTextA(dc, hintText, -1, &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT btn = keyBtnRect();
    HBRUSH bb = CreateSolidBrush(kBorder); FillRect(dc, &btn, bb); DeleteObject(bb);
    SetTextColor(dc, kText);
    DrawTextA(dc, g_waitingKey ? "press key..." : "Change key", -1, &btn,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);

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
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int y = GET_Y_LPARAM(lp);
        int idx = -1;
        if (y >= kListTop && y < listBottom()) {
            int row = (y - kListTop) / kRowH;
            if (row >= 0 && row < visibleRows()) {
                int cand = g_scroll + row;
                if (cand >= 0 && cand < g_count) idx = cand;
            }
        }
        if (idx != g_hover) { g_hover = idx; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT btn = keyBtnRect();
        if (x >= btn.left && x < btn.right && y >= btn.top && y < btn.bottom) {
            g_waitingKey = true;
            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (y >= kListTop && y < listBottom()) {
            int row = (y - kListTop) / kRowH;
            int cand = g_scroll + row;
            if (cand >= 0 && cand < g_count) {
                g_sel = cand;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        SetFocus(hwnd);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int y = GET_Y_LPARAM(lp);
        if (y >= kListTop && y < listBottom()) {
            int row = (y - kListTop) / kRowH;
            int cand = g_scroll + row;
            if (cand >= 0 && cand < g_count) { g_sel = cand; tryLoadSelected(); }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int step = delta > 0 ? -1 : 1;
        int ns = g_scroll + step;
        if (ns < 0) ns = 0;
        if (ns > maxScroll()) ns = maxScroll();
        if (ns != g_scroll) { g_scroll = ns; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_KEYDOWN:
        if (g_waitingKey) {
            UINT vk = (UINT)wp;
            g_waitingKey = false;
            if (vk != VK_ESCAPE) { g_cfg.toggleKey = vk; applyHotkey(); }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_UP) {
            if (g_sel > 0) { g_sel--; if (g_sel < g_scroll) g_scroll = g_sel;
                             InvalidateRect(hwnd, nullptr, FALSE); }
        } else if (wp == VK_DOWN) {
            if (g_sel + 1 < g_count) {
                g_sel++;
                if (g_sel >= g_scroll + visibleRows()) g_scroll = g_sel - visibleRows() + 1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (wp == VK_RETURN) {
            tryLoadSelected();
        } else if (wp == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    case WM_HOTKEY:
        if (wp == kIdHotkey) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else {
                refreshList();
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                SetFocus(hwnd);
            }
        }
        return 0;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
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
    wc.style = CS_DBLCLKS; // нужно для WM_LBUTTONDBLCLK
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        OutputDebugStringA("LoadZone: RegisterClassEx failed.\n");
        return;
    }
    g_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "LoadZoneMenuClassW",
        "LoadZone", WS_POPUP, 200, 200, kWinW, kWinH,
        nullptr, nullptr, g_self, nullptr);
    if (!g_hwnd) { OutputDebugStringA("LoadZone: menu window failed.\n"); return; }
    applyHotkey();
    refreshList();
}

void shutdownZoneMenu() {
    if (!g_hwnd) return;
    if (g_registered) { UnregisterHotKey(g_hwnd, kIdHotkey); g_registered = false; }
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
}