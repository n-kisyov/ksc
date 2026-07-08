#include "gui.h"
#include "ksc_private.h"
#include "database.h"
#include "startup.h"
#include "tray.h"

static HINSTANCE g_hInst = NULL;
static HWND g_hListView = NULL;
static BOOL g_dark_mode = FALSE;
static HBRUSH g_hDarkBrush = NULL;
static HBRUSH g_hLvBrush = NULL;
static HICON g_hAppIcon = NULL;

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

static void refresh_list_view(void)
{
    if (!g_hListView) return;

    ListView_DeleteAllItems(g_hListView);

    KeyStat *stats = NULL;
    int count = db_get_stats(&stats);
    if (!stats || count == 0) return;

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
                SetWindowTheme(g_hListView, L"DarkMode_Explorer", NULL);
            } else {
                SetWindowTheme(g_hListView, L"", L"");
                ListView_SetBkColor(g_hListView, RGB(37, 37, 38));
                ListView_SetTextBkColor(g_hListView, RGB(37, 37, 38));
                ListView_SetTextColor(g_hListView, RGB(212, 212, 212));
            }
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

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hWnd, &rc);

        g_hListView = CreateWindow(WC_LISTVIEW, "",
                                    WS_CHILD | WS_VISIBLE |
                                    LVS_REPORT | LVS_SINGLESEL,
                                    0, 0, rc.right, rc.bottom,
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
        if (g_hListView) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(g_hListView, NULL, 0, 0,
                         rc.right, rc.bottom, SWP_NOZORDER);
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
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
            }
            return 0;
        case WM_RBUTTONUP:
            tray_show_menu(hWnd, TRAY_ID);
            return 0;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SHOW:
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
            } else {
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
            }
            break;
        case IDM_SETTINGS:
            show_settings(hWnd);
            break;
        case IDM_REFRESH:
            refresh_list_view();
            break;
        case IDM_QUIT:
            DestroyWindow(hWnd);
            break;
        case IDM_ABOUT:
            MessageBox(hWnd,
                "KSC - Keystroke Counter v1.0\n\n"
                "Counts every keystroke on your keyboard.\n"
                "Stores counts in a SQLite database.\n\n"
                "Built for Windows 11 with MinGW/GCC.",
                "About KSC", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (g_hDarkBrush) { DeleteObject(g_hDarkBrush); g_hDarkBrush = NULL; }
        if (g_hLvBrush)   { DeleteObject(g_hLvBrush);   g_hLvBrush = NULL;   }
        if (g_hAppIcon)   { DestroyIcon(g_hAppIcon);    g_hAppIcon = NULL;   }
        KillTimer(hWnd, ID_TIMER_REFRESH);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

HWND gui_create_main_window(HINSTANCE hInstance)
{
    g_hInst = hInstance;

    g_hAppIcon = create_app_icon();

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
