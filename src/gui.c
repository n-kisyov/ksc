#include "gui.h"
#include "ksc_private.h"
#include "database.h"
#include "startup.h"
#include "tray.h"

static HINSTANCE g_hInst = NULL;
static HWND g_hListView = NULL;

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

static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    (void)lParam;

    switch (msg) {
    case WM_CREATE: {
        CreateWindow("BUTTON", "Start with Windows",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, 20, 220, 25, hWnd,
                     (HMENU)IDC_STARTUP_CHK, g_hInst, NULL);

        if (startup_is_enabled())
            SendDlgItemMessage(hWnd, IDC_STARTUP_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        CreateWindow("BUTTON", "OK",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     160, 60, 80, 25, hWnd,
                     (HMENU)IDOK, g_hInst, NULL);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            int checked = (SendDlgItemMessage(hWnd, IDC_STARTUP_CHK,
                           BM_GETCHECK, 0, 0) == BST_CHECKED);
            startup_set_enabled(checked);
            MessageBox(hWnd,
                       checked ? "KSC will start with Windows."
                               : "KSC will not start with Windows.",
                       "KSC Settings", MB_OK | MB_ICONINFORMATION);
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
                              CW_USEDEFAULT, CW_USEDEFAULT, 280, 135,
                              hParent, NULL, g_hInst, NULL);
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
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMN lvc;
        memset(&lvc, 0, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = 300;
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

    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) {
            refresh_list_view();
        }
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
        KillTimer(hWnd, ID_TIMER_REFRESH);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

HWND gui_create_main_window(HINSTANCE hInstance)
{
    g_hInst = hInstance;

    WNDCLASS wc = {0};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "KSC_MainWindow";

    if (!RegisterClass(&wc)) return NULL;

    return CreateWindow("KSC_MainWindow", "KSC - Keystroke Counter",
                         WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, 680, 440,
                         NULL, NULL, hInstance, NULL);
}
