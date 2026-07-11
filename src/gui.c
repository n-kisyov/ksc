#include "gui.h"
#include "ksc_private.h"
#include "database.h"
#include "startup.h"
#include "tray.h"
#include "resource.h"

static HINSTANCE g_hInst = NULL;
static HWND g_hListView = NULL;
static BOOL g_dark_mode = FALSE;
static HBRUSH g_hDarkBrush = NULL;
static HBRUSH g_hLvBrush = NULL;
static HICON g_hAppIcon = NULL;
static HWND g_hClickerWnd = NULL;
static HWND g_hTotalLabel = NULL;

typedef void (WINAPI *fnSetPreferredAppMode)(int mode);
typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND hWnd, BOOL allow);
typedef void (WINAPI *fnFlushMenuThemes)(void);

static fnSetPreferredAppMode    g_pSetPreferredAppMode = NULL;
static fnAllowDarkModeForWindow g_pAllowDarkModeForWindow = NULL;
static fnFlushMenuThemes        g_pFlushMenuThemes = NULL;

static HICON create_app_icon(void)
{
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, 32, 32);
    HBITMAP hbmMask = CreateBitmap(32, 32, 1, 1, NULL);

    SelectObject(hdcMem, hbmColor);

    HBRUSH hBrBg = CreateSolidBrush(RGB(31, 41, 55));
    RECT rc = {0, 0, 32, 32};
    FillRect(hdcMem, &rc, hBrBg);
    DeleteObject(hBrBg);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(226, 232, 240));
    HFONT hFont = CreateFont(23, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hOldFont = SelectObject(hdcMem, hFont);
    DrawText(hdcMem, "K", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdcMem, hOldFont);
    DeleteObject(hFont);

    HDC hdcMask = CreateCompatibleDC(hdcScreen);
    SelectObject(hdcMask, hbmMask);
    HBRUSH hBrMask = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdcMask, &rc, hBrMask);
    DeleteObject(hBrMask);

    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    DeleteDC(hdcMask);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    return hIcon;
}

HICON gui_get_app_icon(void)
{
    return g_hAppIcon;
}

void gui_init_dark_mode(void)
{
    HMODULE hUx = GetModuleHandle("uxtheme.dll");
    if (!hUx) return;

    g_pSetPreferredAppMode = (fnSetPreferredAppMode)
        GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    g_pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)
        GetProcAddress(hUx, MAKEINTRESOURCEA(133));
    g_pFlushMenuThemes = (fnFlushMenuThemes)
        GetProcAddress(hUx, MAKEINTRESOURCEA(136));

    if (g_pSetPreferredAppMode) {
        g_pSetPreferredAppMode(1);
    }
}

static int is_mouse_vk(int vk)
{
    return vk == VK_LBUTTON || vk == VK_RBUTTON ||
           vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
           vk == VK_XBUTTON2;
}

static void refresh_list_view(void)
{
    if (!g_hListView) return;

    SendMessage(g_hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hListView);

    KeyStat *stats = NULL;
    int count = db_get_stats(&stats);
    if (!stats || count == 0) {
        if (g_hTotalLabel)
            SetWindowText(g_hTotalLabel,
                "Total keypresses: 0 | Mouse clicks: 0");
        SendMessage(g_hListView, WM_SETREDRAW, TRUE, 0);
        return;
    }

    int64_t kbTotal = 0, mouseTotal = 0;
    for (int i = 0; i < count; i++) {
        if (is_mouse_vk(stats[i].key_code))
            mouseTotal += stats[i].count;
        else
            kbTotal += stats[i].count;
    }

    if (g_hTotalLabel) {
        char buf[128];
        sprintf(buf, "Total keypresses: %lld | Mouse clicks: %lld",
                (long long)kbTotal, (long long)mouseTotal);
        SetWindowText(g_hTotalLabel, buf);
    }

    LVITEM lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT;

    for (int i = 0; i < count; i++) {
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = stats[i].key_name;
        ListView_InsertItem(g_hListView, &lvi);

        char count_str[32];
        sprintf(count_str, "%lld", (long long)stats[i].count);
        ListView_SetItemText(g_hListView, i, 1, count_str);
    }

    db_free_stats(stats);
    SendMessage(g_hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hListView, NULL, TRUE);
}

static void update_theme(HWND hWnd)
{
    g_dark_mode = db_get_setting_int("dark_mode", 0);

    if (g_hDarkBrush) { DeleteObject(g_hDarkBrush); g_hDarkBrush = NULL; }
    if (g_hLvBrush)   { DeleteObject(g_hLvBrush);   g_hLvBrush = NULL;   }

    if (g_dark_mode) {
        g_hDarkBrush = CreateSolidBrush(RGB(30, 30, 30));
        g_hLvBrush   = CreateSolidBrush(RGB(37, 37, 38));

        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &useDark, sizeof(useDark));

        if (g_pAllowDarkModeForWindow) {
            g_pAllowDarkModeForWindow(hWnd, TRUE);
        }

        HMENU hMenu = GetMenu(hWnd);
        if (hMenu) {
            MENUINFO mi = { sizeof(mi) };
            mi.fMask = MIM_BACKGROUND;
            mi.hbrBack = g_hDarkBrush;
            SetMenuInfo(hMenu, &mi);
        }

        if (g_hListView) {
            if (g_pAllowDarkModeForWindow) {
                g_pAllowDarkModeForWindow(g_hListView, TRUE);
            }
            SetWindowTheme(g_hListView, L"DarkMode_Explorer", NULL);
            ListView_SetBkColor(g_hListView, RGB(37, 37, 38));
            ListView_SetTextBkColor(g_hListView, RGB(37, 37, 38));
            ListView_SetTextColor(g_hListView, RGB(212, 212, 212));
        }
    } else {
        g_hDarkBrush = NULL;
        g_hLvBrush   = NULL;

        BOOL useDark = FALSE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &useDark, sizeof(useDark));

        if (g_pAllowDarkModeForWindow) {
            g_pAllowDarkModeForWindow(hWnd, FALSE);
        }

        HMENU hMenu = GetMenu(hWnd);
        if (hMenu) {
            MENUINFO mi = { sizeof(mi) };
            mi.fMask = MIM_BACKGROUND;
            mi.hbrBack = NULL;
            SetMenuInfo(hMenu, &mi);
        }

        if (g_hListView) {
            if (g_pAllowDarkModeForWindow) {
                g_pAllowDarkModeForWindow(g_hListView, FALSE);
            }
            SetWindowTheme(g_hListView, L"Explorer", NULL);
            ListView_SetBkColor(g_hListView, RGB(255, 255, 255));
            ListView_SetTextBkColor(g_hListView, RGB(255, 255, 255));
            ListView_SetTextColor(g_hListView, RGB(0, 0, 0));
        }
    }

    if (g_pFlushMenuThemes) g_pFlushMenuThemes();

    if (g_hListView) InvalidateRect(g_hListView, NULL, TRUE);
    if (hWnd) {
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(hWnd, NULL, TRUE);
    }
}

static void update_auto_refresh(HWND hWnd)
{
    KillTimer(hWnd, ID_TIMER_REFRESH);
    if (db_get_setting_int("auto_refresh", 1)) {
        SetTimer(hWnd, ID_TIMER_REFRESH, 10000, NULL);
    }
}

static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);

        int y = 20;
        CreateWindow("BUTTON", "Start with Windows",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_STARTUP_CHK, g_hInst, NULL);
        if (startup_is_enabled())
            SendDlgItemMessage(hWnd, IDC_STARTUP_CHK, BM_SETCHECK, BST_CHECKED, 0);

        y += 28;
        CreateWindow("BUTTON", "Start minimized to tray",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_START_MINIMIZED_CHK, g_hInst, NULL);
        if (db_get_setting_int("start_minimized", 0))
            SendDlgItemMessage(hWnd, IDC_START_MINIMIZED_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        y += 28;
        CreateWindow("BUTTON", "Dark mode",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_DARK_MODE_CHK, g_hInst, NULL);
        if (db_get_setting_int("dark_mode", 0))
            SendDlgItemMessage(hWnd, IDC_DARK_MODE_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        y += 28;
        CreateWindow("BUTTON", "Auto-refresh stats (10s)",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_AUTO_REFRESH_CHK, g_hInst, NULL);
        if (db_get_setting_int("auto_refresh", 1))
            SendDlgItemMessage(hWnd, IDC_AUTO_REFRESH_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        y += 40;
        CreateWindow("BUTTON", "OK",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     170, y, 80, 25, hWnd,
                     (HMENU)IDOK, g_hInst, NULL);

        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark) {
            BOOL useDark = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &useDark, sizeof(useDark));
            if (g_pAllowDarkModeForWindow)
                g_pAllowDarkModeForWindow(hWnd, TRUE);
            SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        return 0;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark && g_hDarkBrush) {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(230, 230, 230));
            return (LRESULT)g_hDarkBrush;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_ERASEBKGND: {
        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark && g_hDarkBrush) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect((HDC)wParam, &rc, g_hDarkBrush);
            return TRUE;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            int startup = (SendDlgItemMessage(hWnd, IDC_STARTUP_CHK,
                            BM_GETCHECK, 0, 0) == BST_CHECKED);
            int minimized = (SendDlgItemMessage(hWnd, IDC_START_MINIMIZED_CHK,
                               BM_GETCHECK, 0, 0) == BST_CHECKED);
            int dark = (SendDlgItemMessage(hWnd, IDC_DARK_MODE_CHK,
                          BM_GETCHECK, 0, 0) == BST_CHECKED);
            int autoref = (SendDlgItemMessage(hWnd, IDC_AUTO_REFRESH_CHK,
                             BM_GETCHECK, 0, 0) == BST_CHECKED);

            startup_set_enabled(startup);
            db_set_setting_int("start_minimized", minimized);
            db_set_setting_int("dark_mode", dark);
            db_set_setting_int("auto_refresh", autoref);

            HWND hMain = (HWND)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            if (hMain) PostMessage(hMain, WM_THEME_CHANGED, 0, 0);

            DestroyWindow(hWnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_settings(HWND hParent)
{
    static BOOL registered = FALSE;

    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "KSC_Settings";
        RegisterClass(&wc);
        registered = TRUE;
    }

    HWND hDlg = CreateWindow("KSC_Settings", "KSC Settings",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                 CW_USEDEFAULT, CW_USEDEFAULT, 280, 220,
                 hParent, NULL, g_hInst, hParent);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

typedef struct {
    int vk;
    int x, y, w, h;
    const char *label;
} HeatKey;

static const HeatKey g_heatKeys[] = {
    {VK_ESCAPE,   10,  10, 40, 30, "Esc"},
    {VK_F1,       60,  10, 35, 30, "F1"},
    {VK_F2,      100,  10, 35, 30, "F2"},
    {VK_F3,      140,  10, 35, 30, "F3"},
    {VK_F4,      180,  10, 35, 30, "F4"},
    {VK_F5,      235,  10, 35, 30, "F5"},
    {VK_F6,      275,  10, 35, 30, "F6"},
    {VK_F7,      315,  10, 35, 30, "F7"},
    {VK_F8,      355,  10, 35, 30, "F8"},
    {VK_F9,      410,  10, 35, 30, "F9"},
    {VK_F10,     450,  10, 35, 30, "F10"},
    {VK_F11,     490,  10, 35, 30, "F11"},
    {VK_F12,     530,  10, 35, 30, "F12"},
    {VK_SNAPSHOT,585,  10, 40, 30, "PrtSc"},
    {VK_SCROLL,  630,  10, 40, 30, "ScrLk"},
    {VK_PAUSE,   675,  10, 40, 30, "Pause"},

    {VK_OEM_3,    10,  50, 41, 34, "`"},
    {'1',         56,  50, 41, 34, "1"},
    {'2',        102,  50, 41, 34, "2"},
    {'3',        148,  50, 41, 34, "3"},
    {'4',        194,  50, 41, 34, "4"},
    {'5',        240,  50, 41, 34, "5"},
    {'6',        286,  50, 41, 34, "6"},
    {'7',        332,  50, 41, 34, "7"},
    {'8',        378,  50, 41, 34, "8"},
    {'9',        424,  50, 41, 34, "9"},
    {'0',        470,  50, 41, 34, "0"},
    {VK_OEM_MINUS,516, 50, 41, 34, "-"},
    {VK_OEM_PLUS, 562, 50, 41, 34, "="},
    {VK_BACK,    609,  50, 72, 34, "Back"},

    {VK_TAB,      10,  90, 55, 34, "Tab"},
    {'Q',         70,  90, 41, 34, "Q"},
    {'W',        116,  90, 41, 34, "W"},
    {'E',        162,  90, 41, 34, "E"},
    {'R',        208,  90, 41, 34, "R"},
    {'T',        254,  90, 41, 34, "T"},
    {'Y',        300,  90, 41, 34, "Y"},
    {'U',        346,  90, 41, 34, "U"},
    {'I',        392,  90, 41, 34, "I"},
    {'O',        438,  90, 41, 34, "O"},
    {'P',        484,  90, 41, 34, "P"},
    {VK_OEM_4,   530,  90, 41, 34, "["},
    {VK_OEM_6,   576,  90, 41, 34, "]"},
    {VK_OEM_5,   623,  90, 58, 34, "\\"},

    {VK_CAPITAL,  10, 130, 66, 34, "Caps"},
    {'A',         81, 130, 41, 34, "A"},
    {'S',        127, 130, 41, 34, "S"},
    {'D',        173, 130, 41, 34, "D"},
    {'F',        219, 130, 41, 34, "F"},
    {'G',        265, 130, 41, 34, "G"},
    {'H',        311, 130, 41, 34, "H"},
    {'J',        357, 130, 41, 34, "J"},
    {'K',        403, 130, 41, 34, "K"},
    {'L',        449, 130, 41, 34, "L"},
    {VK_OEM_1,   495, 130, 41, 34, ";"},
    {VK_OEM_7,   541, 130, 41, 34, "'"},
    {VK_RETURN,  588, 130, 93, 74, "Enter"},

    {VK_LSHIFT,   10, 170, 88, 34, "Shift"},
    {'Z',        103, 170, 41, 34, "Z"},
    {'X',        149, 170, 41, 34, "X"},
    {'C',        195, 170, 41, 34, "C"},
    {'V',        241, 170, 41, 34, "V"},
    {'B',        287, 170, 41, 34, "B"},
    {'N',        333, 170, 41, 34, "N"},
    {'M',        379, 170, 41, 34, "M"},
    {VK_OEM_COMMA,425,170, 41, 34, ","},
    {VK_OEM_PERIOD,471,170,41, 34, "."},
    {VK_OEM_2,   517, 170, 41, 34, "/"},
    {VK_RSHIFT,  564, 170, 117, 34, "Shift"},

    {VK_LCONTROL, 10, 210, 55, 34, "Ctrl"},
    {VK_LWIN,     70, 210, 45, 34, "Win"},
    {VK_LMENU,   120, 210, 45, 34, "Alt"},
    {VK_SPACE,   172, 210, 280, 34, ""},
    {VK_RMENU,   458, 210, 45, 34, "Alt"},
    {VK_RWIN,    508, 210, 45, 34, "Win"},
    {VK_APPS,    558, 210, 45, 34, "Menu"},
    {VK_RCONTROL,608, 210, 55, 34, "Ctrl"},
};

#define HEAT_KEY_COUNT (sizeof(g_heatKeys) / sizeof(g_heatKeys[0]))

static COLORREF heat_color(int64_t count, int64_t max_count)
{
    if (max_count <= 0 || count <= 0) return RGB(50, 50, 70);
    if (count > max_count) count = max_count;
    double t = (double)count / (double)max_count;

    int r, g, b;
    if (t < 0.2) {
        double s = t / 0.2;
        r = (int)(35 + s * 0);
        g = (int)(35 + s * 65);
        b = (int)(75 + s * 145);
    } else if (t < 0.4) {
        double s = (t - 0.2) / 0.2;
        r = (int)(35 + s * 0);
        g = (int)(100 + s * 80);
        b = (int)(220 + s * -60);
    } else if (t < 0.6) {
        double s = (t - 0.4) / 0.2;
        r = (int)(35 + s * 55);
        g = (int)(180 + s * 40);
        b = (int)(160 + s * -145);
    } else if (t < 0.85) {
        double s = (t - 0.6) / 0.25;
        r = (int)(90 + s * 145);
        g = (int)(220 + s * -105);
        b = (int)(15 + s * -5);
    } else {
        double s = (t - 0.85) / 0.15;
        r = (int)(235 + s * 0);
        g = (int)(115 + s * -90);
        b = (int)(10 + s * 0);
    }

    if (r < 0) r = 0; if (r > 240) r = 240;
    if (g < 0) g = 0; if (g > 240) g = 240;
    if (b < 0) b = 0; if (b > 240) b = 240;

    return RGB(r, g, b);
}

static void draw_heatmap(HWND hWnd, HDC hdc, RECT *rcClient)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    COLORREF bgColor = dark ? RGB(40, 40, 45) : RGB(245, 245, 248);
    COLORREF borderColor = dark ? RGB(60, 60, 65) : RGB(200, 200, 205);
    COLORREF textCold = dark ? RGB(220, 220, 230) : RGB(30, 30, 40);
    COLORREF textHot  = RGB(250, 250, 255);

    HBRUSH hBg = CreateSolidBrush(bgColor);
    FillRect(hdc, rcClient, hBg);
    DeleteObject(hBg);

    int64_t counts[256] = {0};
    int64_t maxCount = 0;
    KeyStat *stats = NULL;
    int nStats = db_get_stats(&stats);
    if (stats && nStats > 0) {
        for (int i = 0; i < nStats; i++) {
            if (stats[i].key_code >= 0 && stats[i].key_code < 256) {
                counts[stats[i].key_code] = stats[i].count;
                if (stats[i].count > maxCount) maxCount = stats[i].count;
            }
        }
        db_free_stats(stats);
    }

    HFONT hFont = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hOldFont = SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN hOldPen = SelectObject(hdc, hPen);

    for (int i = 0; i < (int)HEAT_KEY_COUNT; i++) {
        const HeatKey *hk = &g_heatKeys[i];
        int vk = hk->vk;
        if (vk < 0 || vk >= 256) continue;
        int64_t cnt = counts[vk];

        COLORREF fill = heat_color(cnt, maxCount);
        HBRUSH hKeyBr = CreateSolidBrush(fill);
        SelectObject(hdc, hKeyBr);
        Rectangle(hdc, hk->x, hk->y, hk->x + hk->w, hk->y + hk->h);
        DeleteObject(hKeyBr);
        SelectObject(hdc, hPen);

        if (hk->label && hk->label[0]) {
            double t = (maxCount > 0) ? (double)cnt / (double)maxCount : 0.0;
            SetTextColor(hdc, (t > 0.6) ? textHot : textCold);
            RECT lr = {hk->x, hk->y, hk->x + hk->w, hk->y + hk->h};
            DrawText(hdc, hk->label, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);

    int legendY = 265;
    COLORREF legendSteps[] = {
        heat_color(0, 100),
        heat_color(20, 100),
        heat_color(40, 100),
        heat_color(60, 100),
        heat_color(80, 100),
        heat_color(100, 100),
    };
    int segW = 50;
    int legendX = rcClient->right / 2 - (6 * segW) / 2;

    SetTextColor(hdc, dark ? RGB(200, 200, 210) : RGB(80, 80, 90));
    HFONT hSmFont = CreateFont(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SelectObject(hdc, hSmFont);

    RECT ll = {legendX - 5, legendY + 14, legendX + 6 * segW + 5, legendY + 36};
    DrawText(hdc, "low", -1, &ll, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ll.left = legendX + 6 * segW - 35;
    DrawText(hdc, "high", -1, &ll, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < 6; i++) {
        HBRUSH hLBr = CreateSolidBrush(legendSteps[i]);
        SelectObject(hdc, hLBr);
        Rectangle(hdc, legendX + i * segW, legendY + 2,
                  legendX + (i + 1) * segW, legendY + 28);
        DeleteObject(hLBr);
        SelectObject(hdc, hPen);
    }

    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldFont);
    DeleteObject(hSmFont);
}

static LRESULT CALLBACK HeatmapWndProc(HWND hWnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hWnd, ID_TIMER_REFRESH, 10000, NULL);

        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark) {
            BOOL useDark = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &useDark, sizeof(useDark));
            if (g_pAllowDarkModeForWindow)
                g_pAllowDarkModeForWindow(hWnd, TRUE);
            SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        draw_heatmap(hWnd, hdc, &rc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hWnd, ID_TIMER_REFRESH);
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_heatmap(HWND hParent)
{
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = HeatmapWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_Heatmap";
        RegisterClass(&wc);
        registered = TRUE;
    }

    HWND hDlg = CreateWindow("KSC_Heatmap", "KSC - Key Heatmap",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                 CW_USEDEFAULT, CW_USEDEFAULT, 708, 330,
                 hParent, NULL, g_hInst, NULL);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

typedef struct {
    HWND hListView;
    HWND hDateFrom;
    HWND hDateTo;
    HWND hAppCombo;
    HWND hTotal;
} StatsWinData;

static void stats_refresh_range(HWND hListView, HWND hTotal,
                                 SYSTEMTIME *stFrom, SYSTEMTIME *stTo,
                                 const char *app)
{
    if (!hListView || !stFrom || !stTo) return;

    SendMessage(hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hListView);

    char from[16], to[16];
    sprintf(from, "%04d-%02d-%02d",
            stFrom->wYear, stFrom->wMonth, stFrom->wDay);
    sprintf(to, "%04d-%02d-%02d",
            stTo->wYear, stTo->wMonth, stTo->wDay);

    KeyStat *stats = NULL;
    int count = db_get_date_range_stats(from, to, app, 0, &stats);

    int64_t kbTotal = 0, mouseTotal = 0;
    if (stats && count > 0) {
        for (int i = 0; i < count; i++) {
            if (is_mouse_vk(stats[i].key_code))
                mouseTotal += stats[i].count;
            else
                kbTotal += stats[i].count;
        }
    }
    if (hTotal) {
        char buf[128];
        sprintf(buf, "Keys: %lld | Mouse: %lld",
                (long long)kbTotal, (long long)mouseTotal);
        SetWindowText(hTotal, buf);
    }

    if (!stats || count == 0) {
        SendMessage(hListView, WM_SETREDRAW, TRUE, 0);
        return;
    }

    LVITEM lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT;

    for (int i = 0; i < count; i++) {
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = stats[i].key_name;
        ListView_InsertItem(hListView, &lvi);

        char cnt[32];
        sprintf(cnt, "%lld", (long long)stats[i].count);
        ListView_SetItemText(hListView, i, 1, cnt);
    }

    db_free_stats(stats);
    SendMessage(hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hListView, NULL, TRUE);
}

static void populate_app_combo(HWND hCombo)
{
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"All apps");
    char **apps = NULL;
    int nApps = 0;
    if (db_get_distinct_apps(&apps, &nApps)) {
        for (int i = 0; i < nApps; i++) {
            SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)apps[i]);
        }
        db_free_apps(apps, nApps);
    }
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
}

static const char *get_selected_app(HWND hCombo)
{
    int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (sel <= 0) return NULL;
    static char buf[256];
    buf[0] = '\0';
    SendMessage(hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
    return buf[0] ? buf : NULL;
}

static void stats_apply_theme(HWND hWnd, HWND hListView)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    if (dark) {
        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &useDark, sizeof(useDark));
        if (g_pAllowDarkModeForWindow) {
            g_pAllowDarkModeForWindow(hWnd, TRUE);
            g_pAllowDarkModeForWindow(hListView, TRUE);
        }
        SetWindowTheme(hListView, L"DarkMode_Explorer", NULL);
        ListView_SetBkColor(hListView, RGB(37, 37, 38));
        ListView_SetTextBkColor(hListView, RGB(37, 37, 38));
        ListView_SetTextColor(hListView, RGB(212, 212, 212));
    } else {
        BOOL useDark = FALSE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &useDark, sizeof(useDark));
        if (g_pAllowDarkModeForWindow) {
            g_pAllowDarkModeForWindow(hWnd, FALSE);
            g_pAllowDarkModeForWindow(hListView, FALSE);
        }
        SetWindowTheme(hListView, L"Explorer", NULL);
        ListView_SetBkColor(hListView, RGB(255, 255, 255));
        ListView_SetTextBkColor(hListView, RGB(255, 255, 255));
        ListView_SetTextColor(hListView, RGB(0, 0, 0));
    }
    if (g_pFlushMenuThemes) g_pFlushMenuThemes();
    if (hListView) InvalidateRect(hListView, NULL, TRUE);
    if (hWnd) {
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(hWnd, NULL, TRUE);
    }
}

static void export_csv_file(HWND hParent, KeyStat *stats, int count,
                             const char *defaultName);

static LRESULT CALLBACK StatsWndProc(HWND hWnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        StatsWinData *data = malloc(sizeof(StatsWinData));
        if (!data) return -1;
        memset(data, 0, sizeof(*data));

        int yTop = 10;
        int dph = 22;

        CreateWindow(WC_STATIC, "From:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     10, yTop, 38, dph, hWnd, NULL, g_hInst, NULL);

        HWND hFrom = CreateWindow(DATETIMEPICK_CLASS, "",
                       WS_CHILD | WS_VISIBLE | DTS_SHORTDATECENTURYFORMAT,
                       50, yTop, 110, dph,
                       hWnd, (HMENU)IDC_DATE_FROM, g_hInst, NULL);
        data->hDateFrom = hFrom;

        CreateWindow(WC_STATIC, "To:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     170, yTop, 22, dph, hWnd, NULL, g_hInst, NULL);

        HWND hTo = CreateWindow(DATETIMEPICK_CLASS, "",
                     WS_CHILD | WS_VISIBLE | DTS_SHORTDATECENTURYFORMAT,
                     196, yTop, 110, dph,
                     hWnd, (HMENU)IDC_DATE_TO, g_hInst, NULL);
        data->hDateTo = hTo;

        CreateWindow(WC_STATIC, "App:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     316, yTop, 30, dph, hWnd, NULL, g_hInst, NULL);

        HWND hApp = CreateWindow(WC_COMBOBOX, "",
                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                     350, yTop, 120, 200,
                     hWnd, (HMENU)IDC_APP_COMBO, g_hInst, NULL);
        data->hAppCombo = hApp;
        populate_app_combo(hApp);

        CreateWindow(WC_BUTTON, "Refresh",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     480, yTop - 1, 60, dph + 2,
                     hWnd, (HMENU)IDC_STATS_REFRESH_BTN, g_hInst, NULL);

        CreateWindow(WC_BUTTON, "Export CSV",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     546, yTop - 1, 72, dph + 2,
                     hWnd, (HMENU)IDC_EXPORT_BTN, g_hInst, NULL);

        data->hTotal = CreateWindow(WC_STATIC, "Keys: 0 | Mouse: 0",
                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                         10, 40, 400, 20,
                         hWnd, NULL, g_hInst, NULL);

        SYSTEMTIME stEnd, stStart;
        GetLocalTime(&stEnd);
        stStart = stEnd;

        if (stStart.wDay > 1) {
            stStart.wDay--;
        } else {
            stStart.wMonth--;
            if (stStart.wMonth == 0) {
                stStart.wMonth = 12;
                stStart.wYear--;
            }
            SYSTEMTIME stTmp = stStart;
            stTmp.wDay = 28;
            FILETIME ft;
            SystemTimeToFileTime(&stTmp, &ft);
            FileTimeToSystemTime(&ft, &stTmp);
            stStart.wDay = stTmp.wDay;
        }
        SYSTEMTIME stMonthAgo = stEnd;
        if (stMonthAgo.wMonth > 1)
            stMonthAgo.wMonth--;
        else { stMonthAgo.wMonth = 12; stMonthAgo.wYear--; }

        SendMessage(hFrom, DTM_SETSYSTEMTIME, GDT_VALID,
                    (LPARAM)&stMonthAgo);
        SendMessage(hTo, DTM_SETSYSTEMTIME, GDT_VALID,
                    (LPARAM)&stEnd);

        HWND hLv = CreateWindow(WC_LISTVIEW, "",
                    WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                    0, 62, 10, 10,
                    hWnd, (HMENU)IDC_LISTVIEW, g_hInst, NULL);

        ListView_SetExtendedListViewStyle(hLv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMN lvc;
        memset(&lvc, 0, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = 280;
        lvc.pszText = "Key";
        lvc.iSubItem = 0;
        ListView_InsertColumn(hLv, 0, &lvc);
        lvc.cx = 100;
        lvc.pszText = "Count";
        lvc.iSubItem = 1;
        ListView_InsertColumn(hLv, 1, &lvc);

        data->hListView = hLv;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)data);

        RECT rc;
        GetClientRect(hWnd, &rc);
        SetWindowPos(hLv, NULL, 0, 62, rc.right, rc.bottom - 62,
                     SWP_NOZORDER);

        stats_apply_theme(hWnd, hLv);
        stats_refresh_range(hLv, data->hTotal, &stMonthAgo, &stEnd, NULL);
        SetTimer(hWnd, ID_TIMER_REFRESH, 10000, NULL);
        return 0;
    }

    case WM_SIZE: {
        StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d && d->hListView) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(d->hListView, NULL, 0, 62,
                         rc.right, rc.bottom - 62, SWP_NOZORDER);
        }
        return 0;
    }

    case WM_ERASEBKGND: {
        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark && g_hDarkBrush) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect((HDC)wParam, &rc, g_hDarkBrush);
            return TRUE;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark && g_hDarkBrush) {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(230, 230, 230));
            return (LRESULT)g_hDarkBrush;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_CTLCOLORLISTBOX: {
        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark && g_hDarkBrush) {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetBkColor(hdc, RGB(37, 37, 38));
            SetTextColor(hdc, RGB(230, 230, 230));
            return (LRESULT)g_hDarkBrush;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_NOTIFY: {
        NMHDR *nmh = (NMHDR *)lParam;
        if (nmh->idFrom == IDC_LISTVIEW && nmh->code == NM_CUSTOMDRAW) {
            BOOL dark = db_get_setting_int("dark_mode", 0);
            if (dark) {
                NMLVCUSTOMDRAW *lvcd = (NMLVCUSTOMDRAW *)lParam;
                switch (lvcd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    SetWindowLongPtr(hWnd, DWLP_MSGRESULT,
                                     CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                case CDDS_ITEMPREPAINT:
                    lvcd->clrText   = RGB(212, 212, 212);
                    lvcd->clrTextBk = RGB(37, 37, 38);
                    SetWindowLongPtr(hWnd, DWLP_MSGRESULT, CDRF_NEWFONT);
                    return TRUE;
                }
            }
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_STATS_REFRESH_BTN &&
            HIWORD(wParam) == BN_CLICKED) {
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) {
                SYSTEMTIME stFrom, stTo;
                LRESULT rFrom = SendMessage(d->hDateFrom,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stFrom);
                LRESULT rTo = SendMessage(d->hDateTo,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stTo);
                if (rFrom == GDT_VALID && rTo == GDT_VALID) {
                    const char *app = get_selected_app(d->hAppCombo);
                    stats_refresh_range(d->hListView, d->hTotal, &stFrom, &stTo, app);
                }
            }
        } else if (LOWORD(wParam) == IDC_APP_COMBO &&
                   HIWORD(wParam) == CBN_SELCHANGE) {
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) {
                SYSTEMTIME stFrom, stTo;
                LRESULT rFrom = SendMessage(d->hDateFrom,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stFrom);
                LRESULT rTo = SendMessage(d->hDateTo,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stTo);
                if (rFrom == GDT_VALID && rTo == GDT_VALID) {
                    const char *app = get_selected_app(d->hAppCombo);
                    stats_refresh_range(d->hListView, d->hTotal, &stFrom, &stTo, app);
                }
            }
        } else if (LOWORD(wParam) == IDC_EXPORT_BTN &&
                   HIWORD(wParam) == BN_CLICKED) {
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) {
                SYSTEMTIME stFrom, stTo;
                LRESULT rFrom = SendMessage(d->hDateFrom,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stFrom);
                LRESULT rTo = SendMessage(d->hDateTo,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stTo);
                if (rFrom == GDT_VALID && rTo == GDT_VALID) {
                    char from[16], to[16];
                    sprintf(from, "%04d-%02d-%02d",
                            stFrom.wYear, stFrom.wMonth, stFrom.wDay);
                    sprintf(to, "%04d-%02d-%02d",
                            stTo.wYear, stTo.wMonth, stTo.wDay);
                    const char *app = get_selected_app(d->hAppCombo);
                    KeyStat *stats = NULL;
                    int cnt = db_get_date_range_stats(from, to, app, 1, &stats);
                    if (stats && cnt > 0) {
                        export_csv_file(hWnd, stats, cnt,
                                        "ksc_stats_period.csv");
                        db_free_stats(stats);
                    } else {
                        MessageBox(hWnd, "No data in selected range.",
                                   "Export", MB_OK | MB_ICONINFORMATION);
                    }
                }
            }
        }
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) {
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) {
                SYSTEMTIME stFrom, stTo;
                LRESULT rFrom = SendMessage(d->hDateFrom,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stFrom);
                LRESULT rTo = SendMessage(d->hDateTo,
                    DTM_GETSYSTEMTIME, 0, (LPARAM)&stTo);
                if (rFrom == GDT_VALID && rTo == GDT_VALID) {
                    const char *app = get_selected_app(d->hAppCombo);
                    stats_refresh_range(d->hListView, d->hTotal,
                                       &stFrom, &stTo, app);
                }
            }
        }
        return 0;

    case WM_CLOSE:
        KillTimer(hWnd, ID_TIMER_REFRESH);
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d) free(d);
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_stats_window(HWND hParent)
{
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = StatsWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_Stats";
        RegisterClass(&wc);
        registered = TRUE;
    }

    HWND hDlg = CreateWindow("KSC_Stats", "KSC - Statistics",
                 WS_OVERLAPPEDWINDOW,
                 CW_USEDEFAULT, CW_USEDEFAULT, 710, 420,
                 hParent, NULL, g_hInst, NULL);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static void export_csv_file(HWND hParent, KeyStat *stats, int count,
                             const char *defaultName)
{
    OPENFILENAME ofn;
    char filePath[MAX_PATH] = "";
    if (defaultName && defaultName[0])
        strcpy(filePath, defaultName);

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hParent;
    ofn.lpstrFilter = "CSV Files (*.csv)\0*.csv\0\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileName(&ofn)) return;

    FILE *f = fopen(filePath, "w");
    if (!f) {
        MessageBox(hParent, "Failed to create file.", "Export Error",
                   MB_OK | MB_ICONERROR);
        return;
    }

    fprintf(f, "Key Code,Key Name,App,Count\n");
    for (int i = 0; i < count; i++) {
        fprintf(f, "%d,\"%s\",\"%s\",%lld\n",
                stats[i].key_code, stats[i].key_name,
                stats[i].app,
                (long long)stats[i].count);
    }
    fclose(f);

    MessageBox(hParent, "Exported successfully.", "Export",
               MB_OK | MB_ICONINFORMATION);
}

static void export_all_data(HWND hParent)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char to[16];
    sprintf(to, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    KeyStat *stats = NULL;
    int count = db_get_date_range_stats("2000-01-01", to, NULL, 1, &stats);
    if (!stats || count == 0) {
        KeyStat *allStats = NULL;
        int allCount = db_get_stats(&allStats);
        if (!allStats || allCount == 0) {
            MessageBox(hParent, "No data to export.", "Export",
                       MB_OK | MB_ICONINFORMATION);
            return;
        }
        export_csv_file(hParent, allStats, allCount, "ksc_stats.csv");
        db_free_stats(allStats);
        return;
    }
    export_csv_file(hParent, stats, count, "ksc_stats.csv");
    db_free_stats(stats);
}

static void format_shortcut(int packed, char *buf, int bufsize)
{
    int vk = packed & 0xFFFF;
    int mod = (packed >> 16) & 0xFFFF;
    int pos = 0;
    if (mod & MOD_CONTROL) pos += sprintf(buf + pos, "Ctrl+");
    if (mod & MOD_SHIFT)   pos += sprintf(buf + pos, "Shift+");
    if (mod & MOD_ALT)     pos += sprintf(buf + pos, "Alt+");
    if (mod & MOD_WIN)     pos += sprintf(buf + pos, "Win+");
    if (vk >= 'A' && vk <= 'Z')
        sprintf(buf + pos, "%c", (char)vk);
    else if (vk >= '0' && vk <= '9')
        sprintf(buf + pos, "%c", (char)vk);
    else if (vk >= VK_F1 && vk <= VK_F24)
        sprintf(buf + pos, "F%d", vk - VK_F1 + 1);
    else
        sprintf(buf + pos, "Key%d", vk);
}

typedef struct {
    HWND hMinEdit, hSecEdit, hMsEdit;
    HWND hOffsetEdit;
    HWND hBtnLeft, hBtnRight;
    HWND hContinuous, hLimited;
    HWND hLimitedCount;
    HWND hShortStart, hShortStop;
    HWND hBtnSetStart, hBtnSetStop;
    HWND hBtnStart, hBtnStop;
    HWND hStatus;
    HWND hMainWnd;
    volatile BOOL running;
    HANDLE hThread;
    int clickedSoFar;
    int capturing;
    int hotkeyStartId;
    int hotkeyStopId;
} ClickerData;

static int register_clicker_hotkey(HWND hMain, int id, int shortcut)
{
    if (shortcut <= 0) return 0;
    int vk = shortcut & 0xFFFF;
    int mod = (shortcut >> 16) & 0xFFFF;
    return RegisterHotKey(hMain, id, mod | MOD_NOREPEAT, vk);
}

static void unregister_clicker_hotkey(HWND hMain, int id)
{
    UnregisterHotKey(hMain, id);
}

static DWORD WINAPI ClickerThreadProc(LPVOID param)
{
    ClickerData *d = (ClickerData *)param;
    srand(GetTickCount());
    d->clickedSoFar = 0;

    HWND hDlg = GetParent(d->hBtnStart);
    int intervalMs = GetDlgItemInt(hDlg, IDC_CLICK_INTERVAL_MIN,
                                    NULL, FALSE) * 60000
                   + GetDlgItemInt(hDlg, IDC_CLICK_INTERVAL_SEC,
                                    NULL, FALSE) * 1000
                   + GetDlgItemInt(hDlg, IDC_CLICK_INTERVAL_MS,
                                    NULL, FALSE);
    int offsetMs = GetDlgItemInt(hDlg, IDC_CLICK_RANDOM_OFFSET,
                                  NULL, FALSE);
    BOOL isLeft = (SendMessage(d->hBtnLeft, BM_GETCHECK, 0, 0)
                   == BST_CHECKED);
    BOOL continuous = (SendMessage(d->hContinuous, BM_GETCHECK, 0, 0)
                       == BST_CHECKED);
    int limit = GetDlgItemInt(hDlg, IDC_CLICK_LIMITED_COUNT,
                               NULL, FALSE);

    if (intervalMs < 10) intervalMs = 10;

    while (d->running) {
        INPUT inp[2];
        memset(inp, 0, sizeof(inp));
        inp[0].type = INPUT_MOUSE;
        inp[1].type = INPUT_MOUSE;
        if (isLeft) {
            inp[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inp[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        } else {
            inp[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            inp[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        }
        SendInput(2, inp, sizeof(INPUT));

        d->clickedSoFar++;
        if (!continuous && d->clickedSoFar >= limit) {
            d->running = FALSE;
            break;
        }

        int delay = intervalMs;
        if (offsetMs > 0) {
            int r = (rand() % (2 * offsetMs + 1)) - offsetMs;
            delay += r;
        }
        if (delay < 10) delay = 10;
        Sleep(delay);
    }

    return 0;
}

static void apply_clicker_dark(HWND hWnd)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    if (dark) {
        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &useDark, sizeof(useDark));
        if (g_pAllowDarkModeForWindow)
            g_pAllowDarkModeForWindow(hWnd, TRUE);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK MouseClickerWndProc(HWND hWnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        ClickerData *d = calloc(1, sizeof(ClickerData));
        if (!d) return -1;
        d->hotkeyStartId = HOTKEY_ID_START_CLICK;
        d->hotkeyStopId  = HOTKEY_ID_STOP_CLICK;
        {
            CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
            d->hMainWnd = cs->hwndParent;
        }

        int y, xLabel = 10, xVal = 15, dy = 24, h = 22;
        (void)xLabel;

        /* Row: Click Interval */
        y = 10;
        CreateWindow(WC_STATIC, "Click Interval:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, NULL, g_hInst, NULL);
        y += dy;
        d->hMinEdit = CreateWindow(WC_EDIT, "0",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        15, y, 50, h, hWnd,
                        (HMENU)IDC_CLICK_INTERVAL_MIN, g_hInst, NULL);
        CreateWindow(WC_STATIC, "m",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     70, y + 2, 12, h, hWnd, NULL, g_hInst, NULL);
        d->hSecEdit = CreateWindow(WC_EDIT, "1",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        90, y, 50, h, hWnd,
                        (HMENU)IDC_CLICK_INTERVAL_SEC, g_hInst, NULL);
        CreateWindow(WC_STATIC, "s",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     145, y + 2, 12, h, hWnd, NULL, g_hInst, NULL);
        d->hMsEdit = CreateWindow(WC_EDIT, "0",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        165, y, 50, h, hWnd,
                        (HMENU)IDC_CLICK_INTERVAL_MS, g_hInst, NULL);
        CreateWindow(WC_STATIC, "ms",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     220, y + 2, 20, h, hWnd, NULL, g_hInst, NULL);

        /* Row: Random Offset */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Random Offset:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, NULL, g_hInst, NULL);
        y += dy;
        CreateWindow(WC_STATIC, " -+",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     18, y + 2, 22, h, hWnd, NULL, g_hInst, NULL);
        d->hOffsetEdit = CreateWindow(WC_EDIT, "0",
                           WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                           42, y, 55, h, hWnd,
                           (HMENU)IDC_CLICK_RANDOM_OFFSET, g_hInst, NULL);
        CreateWindow(WC_STATIC, "ms",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     102, y + 2, 20, h, hWnd, NULL, g_hInst, NULL);

        /* Row: Mouse Button */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Mouse Button:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 110, h, hWnd, NULL, g_hInst, NULL);
        y += dy;
        d->hBtnLeft = CreateWindow(WC_BUTTON, "Left",
                        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                        25, y, 70, h, hWnd,
                        (HMENU)IDC_CLICK_BTN_LEFT, g_hInst, NULL);
        d->hBtnRight = CreateWindow(WC_BUTTON, "Right",
                         WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                         120, y, 70, h, hWnd,
                         (HMENU)IDC_CLICK_BTN_RIGHT, g_hInst, NULL);

        /* Row: Mode */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Mode:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 60, h, hWnd, NULL, g_hInst, NULL);
        y += dy;
        d->hContinuous = CreateWindow(WC_BUTTON, "Continuous",
                           WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON |
                           WS_GROUP,
                           25, y, 130, h, hWnd,
                           (HMENU)IDC_CLICK_MODE_CONT, g_hInst, NULL);
        y += dy;
        d->hLimited = CreateWindow(WC_BUTTON, "Limited:",
                       WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                       25, y, 80, h, hWnd,
                       (HMENU)IDC_CLICK_MODE_LIMITED, g_hInst, NULL);
        d->hLimitedCount = CreateWindow(WC_EDIT, "50",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                              115, y, 55, h, hWnd,
                              (HMENU)IDC_CLICK_LIMITED_COUNT, g_hInst, NULL);
        CreateWindow(WC_STATIC, "clicks",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     175, y + 2, 50, h, hWnd, NULL, g_hInst, NULL);

        /* Row: Start Shortcut */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Start Shortcut:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, NULL, g_hInst, NULL);
        y += dy;
        d->hShortStart = CreateWindow(WC_STATIC, "Ctrl+Shift+S",
                           WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                           120, y, 170, h, hWnd,
                           (HMENU)IDC_CLICK_SHORT_START, g_hInst, NULL);
        d->hBtnSetStart = CreateWindow(WC_BUTTON, "Set",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            300, y, 55, h, hWnd,
                            (HMENU)IDC_CLICK_BTN_SET_START, g_hInst, NULL);

        /* Row: Stop Shortcut */
        y += dy + 6;
        CreateWindow(WC_STATIC, "Stop Shortcut:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, NULL, g_hInst, NULL);
        y += dy;
        d->hShortStop = CreateWindow(WC_STATIC, "Ctrl+Shift+X",
                          WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                          120, y, 170, h, hWnd,
                          (HMENU)IDC_CLICK_SHORT_STOP, g_hInst, NULL);
        d->hBtnSetStop = CreateWindow(WC_BUTTON, "Set",
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           300, y, 55, h, hWnd,
                           (HMENU)IDC_CLICK_BTN_SET_STOP, g_hInst, NULL);

        /* Status text */
        y += dy + 8;
        d->hStatus = CreateWindow(WC_STATIC, "Status: Idle",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       10, y, 400, h, hWnd,
                       (HMENU)IDC_CLICK_STATUS, g_hInst, NULL);

        /* Start / Stop buttons */
        y += dy + 4;
        d->hBtnStart = CreateWindow(WC_BUTTON, "Start",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        110, y, 85, 26, hWnd,
                        (HMENU)IDC_CLICK_BTN_START, g_hInst, NULL);
        d->hBtnStop = CreateWindow(WC_BUTTON, "Stop",
                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                       215, y, 85, 26, hWnd,
                       (HMENU)IDC_CLICK_BTN_STOP, g_hInst, NULL);

        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)d);

        /* Load saved settings */
        int intervalMs = db_get_setting_int("clicker_interval_ms", 1000);
        SetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_MIN,
                      intervalMs / 60000, FALSE);
        SetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_SEC,
                      (intervalMs % 60000) / 1000, FALSE);
        SetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_MS,
                      intervalMs % 1000, FALSE);
        SetDlgItemInt(hWnd, IDC_CLICK_RANDOM_OFFSET,
                      db_get_setting_int("clicker_random_offset", 0), FALSE);
        if (db_get_setting_int("clicker_left_button", 1))
            SendMessage(d->hBtnLeft, BM_SETCHECK, BST_CHECKED, 0);
        else
            SendMessage(d->hBtnRight, BM_SETCHECK, BST_CHECKED, 0);
        if (db_get_setting_int("clicker_continuous", 1))
            SendMessage(d->hContinuous, BM_SETCHECK, BST_CHECKED, 0);
        else {
            SendMessage(d->hLimited, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(d->hLimitedCount, TRUE);
        }
        SetDlgItemInt(hWnd, IDC_CLICK_LIMITED_COUNT,
                      db_get_setting_int("clicker_limited_count", 50), FALSE);

        int startSc = db_get_setting_int("clicker_start_shortcut",
                        (MOD_CONTROL | MOD_SHIFT) << 16 | 'S');
        int stopSc  = db_get_setting_int("clicker_stop_shortcut",
                        (MOD_CONTROL | MOD_SHIFT) << 16 | 'X');
        {
            char buf[64];
            format_shortcut(startSc, buf, sizeof(buf));
            SetWindowText(d->hShortStart, buf);
            format_shortcut(stopSc, buf, sizeof(buf));
            SetWindowText(d->hShortStop, buf);
        }

        if (!register_clicker_hotkey(d->hMainWnd, d->hotkeyStartId, startSc))
            SetWindowText(d->hStatus,
                "WARNING: Start hotkey in use by another app");
        if (!register_clicker_hotkey(d->hMainWnd, d->hotkeyStopId, stopSc))
            SetWindowText(d->hStatus,
                "WARNING: Stop hotkey in use by another app");

        apply_clicker_dark(hWnd);
        return 0;
    }

    case WM_ERASEBKGND: {
        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark && g_hDarkBrush) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect((HDC)wParam, &rc, g_hDarkBrush);
            return TRUE;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark && g_hDarkBrush) {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(230, 230, 230));
            return (LRESULT)g_hDarkBrush;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        ClickerData *d = (ClickerData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d && d->capturing) {
            int vk = (int)wParam;
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
                vk == VK_LWIN || vk == VK_RWIN)
                return 0;
            int mod = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
            if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN))
                & 0x8000) mod |= MOD_WIN;
            if (mod == 0) { d->capturing = 0; return 0; }

            int packed = (mod << 16) | vk;
            char buf[64];
            format_shortcut(packed, buf, sizeof(buf));

            if (d->capturing == 1) {
                SetWindowText(d->hShortStart, buf);
                db_set_setting_int("clicker_start_shortcut", packed);
                unregister_clicker_hotkey(d->hMainWnd, d->hotkeyStartId);
                if (!register_clicker_hotkey(d->hMainWnd,
                        d->hotkeyStartId, packed))
                    SetWindowText(d->hStatus,
                        "WARNING: Shortcut already in use by another app");
            } else {
                SetWindowText(d->hShortStop, buf);
                db_set_setting_int("clicker_stop_shortcut", packed);
                unregister_clicker_hotkey(d->hMainWnd, d->hotkeyStopId);
                if (!register_clicker_hotkey(d->hMainWnd,
                        d->hotkeyStopId, packed))
                    SetWindowText(d->hStatus,
                        "WARNING: Shortcut already in use by another app");
            }
            d->capturing = 0;
            return 0;
        }
        break;
    }

    case WM_CLICKER_CMD: {
        ClickerData *d = (ClickerData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (!d) return 0;
        int cmd = (int)wParam;
        if (cmd == 0 && !d->running) {
            /* Start */
            d->running = TRUE;
            d->hThread = CreateThread(NULL, 0, ClickerThreadProc,
                                       d, 0, NULL);
            SetWindowText(d->hStatus, "Status: Running...");
        } else if (cmd == 1 && d->running) {
            /* Stop */
            d->running = FALSE;
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
                d->hThread = NULL;
            }
            char buf[64];
            sprintf(buf, "Status: Stopped (%d clicks)", d->clickedSoFar);
            SetWindowText(d->hStatus, buf);
        } else if (cmd == 2) {
            /* Thread finished */
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
                d->hThread = NULL;
            }
            char buf[64];
            sprintf(buf, "Status: Done (%d clicks)", d->clickedSoFar);
            SetWindowText(d->hStatus, buf);
        }
        return 0;
    }

    case WM_COMMAND: {
        ClickerData *d = (ClickerData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (!d) break;
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (code == BN_CLICKED) {
            if (id == IDC_CLICK_BTN_SET_START) {
                d->capturing = 1;
                SetWindowText(d->hShortStart, "Press keys...");
                SetFocus(hWnd);
                return 0;
            }
            if (id == IDC_CLICK_BTN_SET_STOP) {
                d->capturing = 2;
                SetWindowText(d->hShortStop, "Press keys...");
                SetFocus(hWnd);
                return 0;
            }
            if (id == IDC_CLICK_BTN_START) {
                int intervalMs = GetDlgItemInt(hWnd,
                    IDC_CLICK_INTERVAL_MIN, NULL, FALSE) * 60000
                    + GetDlgItemInt(hWnd,
                    IDC_CLICK_INTERVAL_SEC, NULL, FALSE) * 1000
                    + GetDlgItemInt(hWnd,
                    IDC_CLICK_INTERVAL_MS, NULL, FALSE);
                if (intervalMs < 10) intervalMs = 10;
                BOOL isLeft = (SendMessage(d->hBtnLeft,
                                BM_GETCHECK, 0, 0) == BST_CHECKED);
                BOOL continuous = (SendMessage(d->hContinuous,
                                    BM_GETCHECK, 0, 0) == BST_CHECKED);

                db_set_setting_int("clicker_interval_ms", intervalMs);
                db_set_setting_int("clicker_random_offset",
                    GetDlgItemInt(hWnd, IDC_CLICK_RANDOM_OFFSET,
                                  NULL, FALSE));
                db_set_setting_int("clicker_left_button", isLeft ? 1 : 0);
                db_set_setting_int("clicker_continuous",
                                   continuous ? 1 : 0);
                db_set_setting_int("clicker_limited_count",
                    GetDlgItemInt(hWnd, IDC_CLICK_LIMITED_COUNT,
                                  NULL, FALSE));

                PostMessage(hWnd, WM_CLICKER_CMD, 0, 0);
                return 0;
            }
            if (id == IDC_CLICK_BTN_STOP) {
                PostMessage(hWnd, WM_CLICKER_CMD, 1, 0);
                return 0;
            }
            if (id == IDC_CLICK_MODE_CONT) {
                EnableWindow(d->hLimitedCount, FALSE);
                return 0;
            }
            if (id == IDC_CLICK_MODE_LIMITED) {
                EnableWindow(d->hLimitedCount, TRUE);
                return 0;
            }
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        ClickerData *d = (ClickerData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d) {
            d->running = FALSE;
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
            }
            unregister_clicker_hotkey(d->hMainWnd, d->hotkeyStartId);
            unregister_clicker_hotkey(d->hMainWnd, d->hotkeyStopId);
            free(d);
        }
        g_hClickerWnd = NULL;
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_mouse_clicker(HWND hParent)
{
    if (g_hClickerWnd && IsWindow(g_hClickerWnd)) {
        SetForegroundWindow(g_hClickerWnd);
        return;
    }
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = MouseClickerWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_MouseClicker";
        RegisterClass(&wc);
        registered = TRUE;
    }
    HWND hDlg = CreateWindow("KSC_MouseClicker", "ksc - Mouse Clicker",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                 CW_USEDEFAULT, CW_USEDEFAULT, 440, 470,
                 hParent, NULL, g_hInst, NULL);
    g_hClickerWnd = hDlg;
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hWnd, &rc);

        g_hTotalLabel = CreateWindow(WC_STATIC,
                         "Total keypresses: 0 | Mouse clicks: 0",
                         WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                         0, 0, rc.right, 24,
                         hWnd, NULL, g_hInst, NULL);

        g_hListView = CreateWindow(WC_LISTVIEW, "",
                                    WS_CHILD | WS_VISIBLE |
                                    LVS_REPORT | LVS_SINGLESEL,
                                    0, 24, rc.right, rc.bottom - 24,
                                    hWnd, (HMENU)IDC_LISTVIEW,
                                    g_hInst, NULL);

        ListView_SetExtendedListViewStyle(g_hListView,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMN lvc;
        memset(&lvc, 0, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = 340;
        lvc.pszText = "Key";
        lvc.iSubItem = 0;
        ListView_InsertColumn(g_hListView, 0, &lvc);

        lvc.cx = 120;
        lvc.pszText = "Count";
        lvc.iSubItem = 1;
        ListView_InsertColumn(g_hListView, 1, &lvc);

        SetTimer(hWnd, ID_TIMER_REFRESH, 1000, NULL);

        HMENU hMenu = CreateMenu();
        HMENU hFileMenu = CreatePopupMenu();
        AppendMenu(hFileMenu, MF_STRING, IDM_SETTINGS, "Settings");
        AppendMenu(hFileMenu, MF_STRING, IDM_HEATMAP, "Key Heatmap");
        AppendMenu(hFileMenu, MF_STRING, IDM_STATS, "Stats");
        AppendMenu(hFileMenu, MF_STRING, IDM_EXPORT_CSV, "Export Data...");
        AppendMenu(hFileMenu, MF_STRING, IDM_MOUSE_CLICKER, "Mouse Clicker");
        AppendMenu(hFileMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(hFileMenu, MF_STRING, IDM_QUIT, "Quit");
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, "File");

        HMENU hViewMenu = CreatePopupMenu();
        AppendMenu(hViewMenu, MF_STRING, IDM_REFRESH, "Refresh\tF5");
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, "View");

        HMENU hHelpMenu = CreatePopupMenu();
        AppendMenu(hHelpMenu, MF_STRING, IDM_ABOUT, "About");
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hHelpMenu, "Help");

        SetMenu(hWnd, hMenu);

        update_theme(hWnd);
        update_auto_refresh(hWnd);
        refresh_list_view();
        return 0;
    }

    case WM_SIZE:
        if (g_hTotalLabel) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(g_hTotalLabel, NULL, 0, 0,
                         rc.right, 24, SWP_NOZORDER);
        }
        if (g_hListView) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(g_hListView, NULL, 0, 24,
                         rc.right, rc.bottom - 24, SWP_NOZORDER);
        }
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_HIDE);
        }
        return 0;

    case WM_ERASEBKGND:
        if (g_dark_mode && g_hDarkBrush) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect((HDC)wParam, &rc, g_hDarkBrush);
            return TRUE;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        if (g_dark_mode && g_hDarkBrush) {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(230, 230, 230));
            return (LRESULT)g_hDarkBrush;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_NOTIFY: {
        NMHDR *nmh = (NMHDR *)lParam;
        if (nmh->idFrom == IDC_LISTVIEW && nmh->code == NM_CUSTOMDRAW) {
            if (g_dark_mode) {
                NMLVCUSTOMDRAW *lvcd = (NMLVCUSTOMDRAW *)lParam;
                switch (lvcd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    SetWindowLongPtr(hWnd, DWLP_MSGRESULT,
                                     CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                case CDDS_ITEMPREPAINT:
                    lvcd->clrText   = RGB(212, 212, 212);
                    lvcd->clrTextBk = RGB(37, 37, 38);
                    SetWindowLongPtr(hWnd, DWLP_MSGRESULT, CDRF_NEWFONT);
                    return TRUE;
                }
            }
        }
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) {
            refresh_list_view();
        }
        return 0;

    case WM_THEME_CHANGED:
        update_theme(hWnd);
        update_auto_refresh(hWnd);
        return 0;

    case WM_TRAYICON:
        switch (lParam) {
        case WM_LBUTTONDBLCLK:
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
            } else {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            return 0;
        case WM_RBUTTONUP:
            tray_show_menu(hWnd, TRAY_ID);
            return 0;
        }
        return 0;

    case WM_HOTKEY:
        if (g_hClickerWnd && IsWindow(g_hClickerWnd)) {
            if (wParam == HOTKEY_ID_START_CLICK) {
                PostMessage(g_hClickerWnd, WM_CLICKER_CMD, 0, 0);
            } else if (wParam == HOTKEY_ID_STOP_CLICK) {
                PostMessage(g_hClickerWnd, WM_CLICKER_CMD, 1, 0);
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SHOW:
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
            } else {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            break;
        case IDM_SETTINGS:
            show_settings(hWnd);
            break;
        case IDM_HEATMAP:
            show_heatmap(hWnd);
            break;
        case IDM_STATS:
            show_stats_window(hWnd);
            break;
        case IDM_EXPORT_CSV:
            export_all_data(hWnd);
            break;
        case IDM_MOUSE_CLICKER:
            show_mouse_clicker(hWnd);
            break;
        case IDM_REFRESH:
            refresh_list_view();
            break;
        case IDM_QUIT:
            DestroyWindow(hWnd);
            break;
        case IDM_ABOUT:
            MessageBox(hWnd,
                "ksc - Keystroke Counter\n"
                "Version 0.9.5rc1\n\n"
                "Counts every keystroke on your keyboard.\n"
                "Tracks per-application usage.\n"
                "Stores counts in a SQLite database.\n\n"
                "Built for Windows 11 with MinGW/GCC.\n"
                "bbounce.org",
                "About ksc", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (g_hDarkBrush) { DeleteObject(g_hDarkBrush); g_hDarkBrush = NULL; }
        if (g_hLvBrush)   { DeleteObject(g_hLvBrush);   g_hLvBrush = NULL;   }
        KillTimer(hWnd, ID_TIMER_REFRESH);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

HWND gui_create_main_window(HINSTANCE hInstance)
{
    g_hInst = hInstance;

    g_hAppIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN));
    if (!g_hAppIcon) {
        g_hAppIcon = create_app_icon();
    }

    WNDCLASS wc = {0};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = g_hAppIcon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "KSC_MainWindow";

    if (!RegisterClass(&wc)) return NULL;

    return CreateWindow("KSC_MainWindow", "KSC - Keystroke Counter",
                         WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, 720, 460,
                         NULL, NULL, hInstance, NULL);
}
