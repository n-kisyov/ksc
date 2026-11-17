#include "gui.h"
#include "ksc_private.h"
#include "database.h"
#include "keylogdb.h"
#include "keyhook.h"
#include "startup.h"
#include "tray.h"
#include "cloudsync.h"
#include "ssh_sync.h"
#include "telegram.h"
#include "zip_util.h"
#include "resource.h"

static HINSTANCE g_hInst = nullptr;
static HWND g_hListView = nullptr;
static bool g_dark_mode = false;
static HBRUSH g_hDarkBrush = nullptr;
static HBRUSH g_hLvBrush = nullptr;
static HICON g_hAppIcon = nullptr;
static HWND g_hClickerWnd = nullptr;
static HWND g_hTotalLabel = nullptr;

static void format_shortcut(int packed, char *buf, int bufsize);
static void show_stats_window(HWND hParent);

typedef void (WINAPI *fnSetPreferredAppMode)(int mode);
typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND hWnd, BOOL allow);
typedef void (WINAPI *fnFlushMenuThemes)(void);

static fnSetPreferredAppMode    g_pSetPreferredAppMode = nullptr;
static fnAllowDarkModeForWindow g_pAllowDarkModeForWindow = nullptr;
static fnFlushMenuThemes        g_pFlushMenuThemes = nullptr;

typedef struct {
    int sortCol;
    bool sortAsc;
    bool userSorted;
} ListViewSortState;

typedef struct {
    HWND hLv;
    int col;
    bool asc;
    bool numeric;
} LvSortCtx;

static int CALLBACK listview_compare_proc(LPARAM lParam1, LPARAM lParam2,
                                          LPARAM lParamSort)
{
    const LvSortCtx *ctx = (const LvSortCtx *)lParamSort;
    char b1[260] = "", b2[260] = "";
    ListView_GetItemText(ctx->hLv, (int)lParam1, ctx->col, b1, sizeof(b1));
    ListView_GetItemText(ctx->hLv, (int)lParam2, ctx->col, b2, sizeof(b2));

    int cmp = 0;
    if (ctx->numeric) {
        long long v1 = _strtoi64(b1, nullptr, 10);
        long long v2 = _strtoi64(b2, nullptr, 10);
        cmp = (v1 < v2) ? -1 : (v1 > v2 ? 1 : 0);
    } else {
        cmp = _stricmp(b1, b2);
    }
    return ctx->asc ? cmp : -cmp;
}

static void listview_update_header_arrows(HWND hLv, const ListViewSortState *st)
{
    if (!hLv) return;
    HWND hHdr = ListView_GetHeader(hLv);
    if (!hHdr) return;
    int colCount = Header_GetItemCount(hHdr);
    for (int c = 0; c < colCount; c++) {
        HDITEM hdi = { .mask = HDI_FORMAT };
        if (!Header_GetItem(hHdr, c, &hdi)) continue;
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (st && st->userSorted && c == st->sortCol) {
            hdi.fmt |= st->sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(hHdr, c, &hdi);
    }
}

static void listview_clear_sort_arrows(HWND hLv)
{
    listview_update_header_arrows(hLv, nullptr);
}

static void listview_apply_sort(HWND hLv, const ListViewSortState *st,
                                bool isNumericCol)
{
    if (!hLv || !st || !st->userSorted) return;
    LvSortCtx ctx = {
        .hLv = hLv,
        .col = st->sortCol,
        .asc = st->sortAsc,
        .numeric = isNumericCol
    };
    ListView_SortItemsEx(hLv, listview_compare_proc, (LPARAM)&ctx);
    listview_update_header_arrows(hLv, st);
}

static void listview_toggle_sort(HWND hLv, ListViewSortState *st,
                                 int col, bool isNumericCol)
{
    if (!hLv || !st) return;
    if (st->userSorted && st->sortCol == col) {
        st->sortAsc = !st->sortAsc;
    } else {
        st->sortCol = col;
        st->sortAsc = !isNumericCol;
        st->userSorted = true;
    }
    listview_apply_sort(hLv, st, isNumericCol);
}

typedef struct {
    int topIdx;
    char selectedText[260];
    bool hasSelection;
} LvViewState;

static void listview_save_view_state(HWND hLv, LvViewState *vs)
{
    vs->topIdx = ListView_GetTopIndex(hLv);
    vs->selectedText[0] = '\0';
    vs->hasSelection = false;
    int sel = ListView_GetNextItem(hLv, -1, LVNI_SELECTED);
    if (sel >= 0) {
        ListView_GetItemText(hLv, sel, 0, vs->selectedText,
                             sizeof(vs->selectedText));
        vs->hasSelection = (vs->selectedText[0] != '\0');
    }
}

static void listview_restore_view_state(HWND hLv, const LvViewState *vs)
{
    int n = ListView_GetItemCount(hLv);
    if (n <= 0) return;
    if (vs->hasSelection) {
        for (int i = 0; i < n; i++) {
            char buf[260] = "";
            ListView_GetItemText(hLv, i, 0, buf, sizeof(buf));
            if (_stricmp(buf, vs->selectedText) == 0) {
                ListView_SetItemState(hLv, i, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }
    if (vs->topIdx > 0 && vs->topIdx < n) {
        RECT rcItem = {};
        if (ListView_GetItemRect(hLv, 0, &rcItem, LVIR_BOUNDS)) {
            int itemH = rcItem.bottom - rcItem.top;
            if (itemH > 0) {
                ListView_Scroll(hLv, 0, vs->topIdx * itemH);
            }
        }
    }
}

static void refresh_app_combo_preserve_sel(HWND hCombo, bool isKeylog)
{
    if (!hCombo) return;
    char prevSel[256] = "";
    int cur = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (cur > 0) {
        LRESULT len = SendMessage(hCombo, CB_GETLBTEXTLEN, cur, 0);
        if (len >= 0 && (size_t)len < sizeof(prevSel)) {
            SendMessage(hCombo, CB_GETLBTEXT, cur, (LPARAM)prevSel);
        }
    }

    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"All apps");
    char **apps = nullptr;
    int nApps = 0;
    int ok = isKeylog ? keylog_get_apps(&apps, &nApps)
                      : db_get_distinct_apps(&apps, &nApps);
    if (ok) {
        for (int i = 0; i < nApps; i++)
            SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)apps[i]);
        if (isKeylog) keylog_free_apps(apps, nApps);
        else db_free_apps(apps, nApps);
    }

    int restoreIdx = 0;
    if (prevSel[0]) {
        LRESULT idx = SendMessage(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                  (LPARAM)prevSel);
        if (idx != CB_ERR) restoreIdx = (int)idx;
    }
    SendMessage(hCombo, CB_SETCURSEL, restoreIdx, 0);
}

static bool focus_existing_window(const char *className)
{
    HWND existing = FindWindow(className, nullptr);
    if (!existing) return false;
    if (!IsWindowVisible(existing) || IsIconic(existing))
        ShowWindow(existing, SW_RESTORE);
    SetForegroundWindow(existing);
    return true;
}

static void get_date_days_ago(int days_back, SYSTEMTIME *outSt)
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    FILETIME ft = {};
    if (SystemTimeToFileTime(&now, &ft)) {
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        ULONGLONG delta = (ULONGLONG)days_back * 24ULL * 60ULL * 60ULL * 10000000ULL;
        if (uli.QuadPart > delta) uli.QuadPart -= delta;
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        if (FileTimeToSystemTime(&ft, outSt)) return;
    }
    *outSt = now;
}

static void format_date_str(const SYSTEMTIME *st, char *buf, size_t sz)
{
    snprintf(buf, sz, "%04d-%02d-%02d", st->wYear, st->wMonth, st->wDay);
}

static void write_csv_escaped(FILE *f, const char *s)
{
    fputc('"', f);
    if (s) {
        for (const char *p = s; *p; p++) {
            if (*p == '"') fputc('"', f);
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

static bool handle_dark_erasebkgnd(HWND hWnd, HDC hdc)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    if (dark && g_hDarkBrush) {
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_hDarkBrush);
        return true;
    }
    return false;
}

static LRESULT handle_dark_ctlcolor(HWND hWnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    if (dark && g_hDarkBrush) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(230, 230, 230));
        if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(37, 37, 38));
            return (LRESULT)(g_hLvBrush ? g_hLvBrush : g_hDarkBrush);
        }
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)g_hDarkBrush;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void apply_listview_theme(HWND hWnd, HWND hListView)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &useDark, sizeof(useDark));
    if (g_pAllowDarkModeForWindow) {
        g_pAllowDarkModeForWindow(hWnd, useDark);
        if (hListView) g_pAllowDarkModeForWindow(hListView, useDark);
    }
    if (hListView) {
        SetWindowTheme(hListView, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        ListView_SetBkColor(hListView, dark ? RGB(37, 37, 38) : RGB(255, 255, 255));
        ListView_SetTextBkColor(hListView, dark ? RGB(37, 37, 38) : RGB(255, 255, 255));
        ListView_SetTextColor(hListView, dark ? RGB(212, 212, 212) : RGB(0, 0, 0));
    }
    if (g_pFlushMenuThemes) g_pFlushMenuThemes();
    if (hListView) InvalidateRect(hListView, nullptr, TRUE);
    if (hWnd) {
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(hWnd, nullptr, TRUE);
    }
}

static bool handle_dark_lv_customdraw(int ctlId, LPARAM lParam,
                                      LRESULT *outRes)
{
    NMHDR *nmh = (NMHDR *)lParam;
    if ((int)nmh->idFrom != ctlId || nmh->code != NM_CUSTOMDRAW)
        return false;
    if (!db_get_setting_int("dark_mode", 0))
        return false;

    NMLVCUSTOMDRAW *lvcd = (NMLVCUSTOMDRAW *)lParam;
    switch (lvcd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        *outRes = CDRF_NOTIFYITEMDRAW;
        return true;
    case CDDS_ITEMPREPAINT:
        lvcd->clrText   = RGB(212, 212, 212);
        lvcd->clrTextBk = RGB(37, 37, 38);
        *outRes = CDRF_NEWFONT;
        return true;
    }
    return false;
}

static HICON create_app_icon(void)
{
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, 32, 32);
    HBITMAP hbmMask = CreateBitmap(32, 32, 1, 1, nullptr);

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
    ReleaseDC(nullptr, hdcScreen);

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

    g_pSetPreferredAppMode = (fnSetPreferredAppMode)(void *)
        GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    g_pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)(void *)
        GetProcAddress(hUx, MAKEINTRESOURCEA(133));
    g_pFlushMenuThemes = (fnFlushMenuThemes)(void *)
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

static ListViewSortState g_mainSort = {1, false, false};
static int64_t g_cachedKbTotal = 0;
static int64_t g_cachedMouseTotal = 0;

static void update_main_status_bar(void)
{
    if (!g_hTotalLabel) return;
    int64_t todayTotal = db_get_today_count();
    int wpm = 0, peakWpm = 0;
    keyhook_get_wpm(&wpm, &peakWpm);
    const char *prefix = keyhook_is_paused() ? "[PAUSED]  " : "";
    char buf[224];
    snprintf(buf, sizeof(buf),
        "%sTotal: %lld keys | %lld clicks  |  Today: %lld  |  Speed: %d WPM (Peak: %d)",
        prefix,
        (long long)g_cachedKbTotal, (long long)g_cachedMouseTotal,
        (long long)todayTotal, wpm, peakWpm);
    SetWindowText(g_hTotalLabel, buf);
}

static void refresh_list_view(void)
{
    if (!g_hListView) return;

    LvViewState vs;
    listview_save_view_state(g_hListView, &vs);

    SendMessage(g_hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hListView);

    KeyStat *stats = nullptr;
    int count = db_get_stats(&stats);
    if (!stats || count == 0) {
        g_cachedKbTotal = 0;
        g_cachedMouseTotal = 0;
        update_main_status_bar();
        SendMessage(g_hListView, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hListView, nullptr, TRUE);
        return;
    }

    int64_t kbTotal = 0, mouseTotal = 0;
    for (int i = 0; i < count; i++) {
        if (is_mouse_vk(stats[i].key_code))
            mouseTotal += stats[i].count;
        else
            kbTotal += stats[i].count;
    }
    g_cachedKbTotal = kbTotal;
    g_cachedMouseTotal = mouseTotal;
    update_main_status_bar();

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
    listview_apply_sort(g_hListView, &g_mainSort, g_mainSort.sortCol == 1);
    listview_restore_view_state(g_hListView, &vs);
    SendMessage(g_hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hListView, nullptr, TRUE);
}

static BOOL CALLBACK EnumThemeWindowsProc(HWND hwnd, LPARAM lParam)
{
    if (hwnd != (HWND)lParam) {
        PostMessage(hwnd, WM_THEME_CHANGED, 0, 0);
    }
    return TRUE;
}

static void handle_theme_changed(HWND hWnd)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    if (g_pSetPreferredAppMode) {
        g_pSetPreferredAppMode(dark ? 2 : 3);
    }
    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &useDark, sizeof(useDark));
    if (g_pAllowDarkModeForWindow)
        g_pAllowDarkModeForWindow(hWnd, useDark);
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hWnd, nullptr, TRUE);
}

static void update_theme(HWND hWnd)
{
    g_dark_mode = db_get_setting_int("dark_mode", 0) != 0;
    if (g_pSetPreferredAppMode) {
        g_pSetPreferredAppMode(g_dark_mode ? 2 : 3);
    }

    if (g_hDarkBrush) { DeleteObject(g_hDarkBrush); g_hDarkBrush = nullptr; }
    if (g_hLvBrush)   { DeleteObject(g_hLvBrush);   g_hLvBrush = nullptr;   }

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
            MENUINFO mi = {
                .cbSize = sizeof(mi),
                .fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS,
                .hbrBack = g_hDarkBrush
            };
            SetMenuInfo(hMenu, &mi);
            DrawMenuBar(hWnd);
        }

        if (g_hListView) {
            if (g_pAllowDarkModeForWindow) {
                g_pAllowDarkModeForWindow(g_hListView, TRUE);
            }
            SetWindowTheme(g_hListView, L"DarkMode_Explorer", nullptr);
            ListView_SetBkColor(g_hListView, RGB(37, 37, 38));
            ListView_SetTextBkColor(g_hListView, RGB(37, 37, 38));
            ListView_SetTextColor(g_hListView, RGB(212, 212, 212));
        }
    } else {
        g_hDarkBrush = nullptr;
        g_hLvBrush   = nullptr;

        BOOL useDark = FALSE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &useDark, sizeof(useDark));

        if (g_pAllowDarkModeForWindow) {
            g_pAllowDarkModeForWindow(hWnd, FALSE);
        }

        HMENU hMenu = GetMenu(hWnd);
        if (hMenu) {
            MENUINFO mi = {
                .cbSize = sizeof(mi),
                .fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS,
                .hbrBack = nullptr
            };
            SetMenuInfo(hMenu, &mi);
            DrawMenuBar(hWnd);
        }

        if (g_hListView) {
            if (g_pAllowDarkModeForWindow) {
                g_pAllowDarkModeForWindow(g_hListView, FALSE);
            }
            SetWindowTheme(g_hListView, L"Explorer", nullptr);
            ListView_SetBkColor(g_hListView, RGB(255, 255, 255));
            ListView_SetTextBkColor(g_hListView, RGB(255, 255, 255));
            ListView_SetTextColor(g_hListView, RGB(0, 0, 0));
        }
    }

    if (g_pFlushMenuThemes) g_pFlushMenuThemes();

    if (g_hListView) InvalidateRect(g_hListView, nullptr, TRUE);
    if (hWnd) {
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(hWnd, nullptr, TRUE);
    }
}

static void update_auto_refresh(HWND hWnd)
{
    KillTimer(hWnd, ID_TIMER_REFRESH);
    if (db_get_setting_int("auto_refresh", 1)) {
        SetTimer(hWnd, ID_TIMER_REFRESH, 10000, nullptr);
    }
}

static int g_setCapturing = 0;

static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        g_setCapturing = 0;

        int y = 20;
        CreateWindow("BUTTON", "Start with Windows",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_STARTUP_CHK, g_hInst, nullptr);
        if (startup_is_enabled())
            SendDlgItemMessage(hWnd, IDC_STARTUP_CHK, BM_SETCHECK, BST_CHECKED, 0);

        y += 28;
        CreateWindow("BUTTON", "Start minimized to tray",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_START_MINIMIZED_CHK, g_hInst, nullptr);
        if (db_get_setting_int("start_minimized", 0))
            SendDlgItemMessage(hWnd, IDC_START_MINIMIZED_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        y += 28;
        CreateWindow("BUTTON", "Dark mode",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_DARK_MODE_CHK, g_hInst, nullptr);
        if (db_get_setting_int("dark_mode", 0))
            SendDlgItemMessage(hWnd, IDC_DARK_MODE_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        y += 28;
        CreateWindow("BUTTON", "Auto-refresh stats (10s)",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_AUTO_REFRESH_CHK, g_hInst, nullptr);
        if (db_get_setting_int("auto_refresh", 1))
            SendDlgItemMessage(hWnd, IDC_AUTO_REFRESH_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        y += 28;
        CreateWindow("BUTTON", "Enable Keylogger",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 220, 22, hWnd,
                     (HMENU)IDC_KEYLOGGER_CHK, g_hInst, nullptr);
        if (db_get_setting_int("keylogger_enabled", 0))
            SendDlgItemMessage(hWnd, IDC_KEYLOGGER_CHK,
                               BM_SETCHECK, BST_CHECKED, 0);

        y += 30;
        CreateWindow("BUTTON", "Delete Keylogger Logs",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     20, y, 160, 22, hWnd,
                     (HMENU)IDC_DELETE_KEYLOG_BTN, g_hInst, nullptr);
        if (db_get_setting_int("keylogger_enabled", 0))
            EnableWindow(GetDlgItem(hWnd, IDC_DELETE_KEYLOG_BTN), FALSE);

        y += 30;
        CreateWindow("BUTTON", "Reset All Statistics",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     20, y, 160, 22, hWnd,
                     (HMENU)IDC_RESET_STATS_BTN, g_hInst, nullptr);

        y += 28;
        CreateWindow(WC_STATIC, "Show KSC Shortcut:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                     20, y, 140, 22, hWnd, nullptr, g_hInst, nullptr);
        y += 24;
        {
            char buf[64];
            format_shortcut(db_get_setting_int("show_ksc_shortcut",
                (MOD_CONTROL | MOD_SHIFT) << 16 | 'K'), buf, sizeof(buf));
            CreateWindow(WC_STATIC, buf,
                         WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                         20, y, 140, 22, hWnd,
                         (HMENU)IDC_HOTKEY_SHOW_LBL, g_hInst, nullptr);
        }
        CreateWindow("BUTTON", "Set",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     168, y, 50, 22, hWnd,
                     (HMENU)IDC_HOTKEY_SET_SHOW, g_hInst, nullptr);

        y += 30;
        CreateWindow(WC_STATIC, "\x1a\x1a\x1a Telegram Notifications \x1a\x1a\x1a",
                     WS_CHILD | WS_VISIBLE | SS_CENTER,
                     10, y, 300, 22, hWnd, nullptr, g_hInst, nullptr);
        y += 26;
        CreateWindow(WC_STATIC, "Bot Token:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     10, y, 85, 22, hWnd, nullptr, g_hInst, nullptr);
        CreateWindow(WC_EDIT, "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     97, y, 260, 22, hWnd,
                     (HMENU)IDC_TELEGRAM_TOKEN, g_hInst, nullptr);
        {
            char tok[256];
            extern void db_get_setting_str(const char *k,
                const char *d, char *o, int s);
            db_get_setting_str("tg_bot_token", "", tok, sizeof(tok));
            SetDlgItemText(hWnd, IDC_TELEGRAM_TOKEN, tok);
        }

        y += 26;
        CreateWindow(WC_STATIC, "Group Chat ID:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     10, y, 85, 22, hWnd, nullptr, g_hInst, nullptr);
        CreateWindow(WC_EDIT, "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     97, y, 260, 22, hWnd,
                     (HMENU)IDC_TELEGRAM_CHAT_ID, g_hInst, nullptr);
        {
            char cid[256];
            extern void db_get_setting_str(const char *k,
                const char *d, char *o, int s);
            db_get_setting_str("tg_chat_id", "", cid, sizeof(cid));
            SetDlgItemText(hWnd, IDC_TELEGRAM_CHAT_ID, cid);
        }

        y += 28;
        CreateWindow(WC_BUTTON, "Save Credentials",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     20, y, 130, 22, hWnd,
                     (HMENU)IDC_TELEGRAM_SAVE, g_hInst, nullptr);

        y += 26;
        CreateWindow("BUTTON", "Enable Telegram",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, y, 140, 22, hWnd,
                     (HMENU)IDC_TELEGRAM_ENABLE, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "(cloud sync status)",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     165, y + 3, 130, 18, hWnd, nullptr, g_hInst, nullptr);
        {
            int en = db_get_setting_int("tg_enabled", 0);
            if (en)
                SendDlgItemMessage(hWnd, IDC_TELEGRAM_ENABLE,
                                   BM_SETCHECK, BST_CHECKED, 0);
            /* disable if no token or chat_id */
            char t1[256], t2[256];
            extern void db_get_setting_str(const char *k,
                const char *d, char *o, int s);
            db_get_setting_str("tg_bot_token", "", t1, sizeof(t1));
            db_get_setting_str("tg_chat_id", "", t2, sizeof(t2));
            if (!t1[0] || !t2[0])
                EnableWindow(GetDlgItem(hWnd, IDC_TELEGRAM_ENABLE), FALSE);
        }

        y += 26;
        CreateWindow(WC_STATIC, "Notify:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     20, y, 50, 22, hWnd, nullptr, g_hInst, nullptr);
        CreateWindow("BUTTON", "Success + failures",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON |
                     WS_GROUP,
                     75, y, 150, 22, hWnd,
                     (HMENU)IDC_TELEGRAM_SUCCESS, g_hInst, nullptr);
        y += 22;
        CreateWindow("BUTTON", "Failures only",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                     75, y, 150, 22, hWnd,
                     (HMENU)IDC_TELEGRAM_FAILURES, g_hInst, nullptr);
        {
            int mode = db_get_setting_int("tg_notify_mode", 0);
            if (mode == 0)
                SendDlgItemMessage(hWnd, IDC_TELEGRAM_SUCCESS,
                                   BM_SETCHECK, BST_CHECKED, 0);
            else
                SendDlgItemMessage(hWnd, IDC_TELEGRAM_FAILURES,
                                   BM_SETCHECK, BST_CHECKED, 0);
        }

        y += 30;
        CreateWindow(WC_BUTTON, "Test Message",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     20, y, 110, 24, hWnd,
                     (HMENU)IDC_TELEGRAM_TEST, g_hInst, nullptr);

        y += 36;
        CreateWindow("BUTTON", "OK",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     170, y, 80, 25, hWnd,
                     (HMENU)IDOK, g_hInst, nullptr);

        BOOL dark = db_get_setting_int("dark_mode", 0);
        if (dark) {
            BOOL useDark = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &useDark, sizeof(useDark));
            if (g_pAllowDarkModeForWindow)
                g_pAllowDarkModeForWindow(hWnd, TRUE);
            SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        return 0;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return handle_dark_ctlcolor(hWnd, msg, wParam, lParam);

    case WM_ERASEBKGND:
        if (handle_dark_erasebkgnd(hWnd, (HDC)wParam)) return TRUE;
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_setCapturing) {
            int vk = (int)wParam;
            if (vk == VK_CONTROL || vk == VK_SHIFT ||
                vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN)
                return 0;
            int mod = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
            if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN))
                & 0x8000) mod |= MOD_WIN;
            if (mod == 0 || vk == VK_ESCAPE) {
                char buf[64];
                int cur = db_get_setting_int("show_ksc_shortcut",
                    (MOD_CONTROL | MOD_SHIFT) << 16 | 'K');
                format_shortcut(cur, buf, sizeof(buf));
                SetDlgItemText(hWnd, IDC_HOTKEY_SHOW_LBL, buf);
                g_setCapturing = 0;
                return 0;
            }
            int packed = (mod << 16) | vk;
            db_set_setting_int("show_ksc_shortcut", packed);
            {
                char buf[64];
                format_shortcut(packed, buf, sizeof(buf));
                SetDlgItemText(hWnd, IDC_HOTKEY_SHOW_LBL, buf);
            }
            {
                HWND hMain = (HWND)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                UnregisterHotKey(hMain, HOTKEY_ID_SHOW_KSC);
                RegisterHotKey(hMain, HOTKEY_ID_SHOW_KSC,
                               mod | MOD_NOREPEAT, vk);
            }
            g_setCapturing = 0;
            return 0;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_KEYLOGGER_CHK &&
            HIWORD(wParam) == BN_CLICKED) {
            int checked = (SendDlgItemMessage(hWnd, IDC_KEYLOGGER_CHK,
                            BM_GETCHECK, 0, 0) == BST_CHECKED);
            db_set_setting_int("keylogger_enabled", checked);
            keyhook_set_keylogger_enabled(checked);
            if (checked) keylog_open();
            else { keylog_flush_events(); keylog_close(); }
            EnableWindow(GetDlgItem(hWnd, IDC_DELETE_KEYLOG_BTN),
                         checked ? FALSE : TRUE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_DELETE_KEYLOG_BTN &&
            HIWORD(wParam) == BN_CLICKED) {
            keylog_delete_db();
            int enabled = (SendDlgItemMessage(hWnd, IDC_KEYLOGGER_CHK,
                            BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (enabled) keylog_open();
            MessageBox(hWnd, "Keylogger database deleted.",
                       "Keylogger", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESET_STATS_BTN &&
            HIWORD(wParam) == BN_CLICKED) {
            int res = MessageBox(hWnd,
                "This will delete all keystroke and mouse click data.\n"
                "Settings will be kept.\n\nContinue?",
                "Reset Statistics", MB_YESNO | MB_ICONWARNING);
            if (res == IDYES) {
                db_reset_stats();
                keyhook_reset_wpm();
                HWND hMain = (HWND)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                if (hMain) {
                    PostMessage(hMain, WM_THEME_CHANGED, 0, 0);
                    PostMessage(hMain, WM_TIMER, ID_TIMER_REFRESH, 0);
                }
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_HOTKEY_SET_SHOW &&
            HIWORD(wParam) == BN_CLICKED) {
            g_setCapturing = 1;
            SetDlgItemText(hWnd, IDC_HOTKEY_SHOW_LBL, "Press keys...");
            SetFocus(hWnd);
            return 0;
        }
        if ((LOWORD(wParam) == IDC_TELEGRAM_TOKEN ||
             LOWORD(wParam) == IDC_TELEGRAM_CHAT_ID) &&
            HIWORD(wParam) == EN_CHANGE) {
            char buf[256];
            extern void db_set_setting_str(const char *k,
                const char *v);
            if (LOWORD(wParam) == IDC_TELEGRAM_TOKEN) {
                GetDlgItemText(hWnd, IDC_TELEGRAM_TOKEN,
                               buf, sizeof(buf));
                db_set_setting_str("tg_bot_token", buf);
            } else {
                GetDlgItemText(hWnd, IDC_TELEGRAM_CHAT_ID,
                               buf, sizeof(buf));
                db_set_setting_str("tg_chat_id", buf);
            }
            /* update enable/disable */
            char tok[256], cid[256];
            GetDlgItemText(hWnd, IDC_TELEGRAM_TOKEN,
                           tok, sizeof(tok));
            GetDlgItemText(hWnd, IDC_TELEGRAM_CHAT_ID,
                           cid, sizeof(cid));
            HWND hChk = GetDlgItem(hWnd, IDC_TELEGRAM_ENABLE);
            if (tok[0] && cid[0])
                EnableWindow(hChk, TRUE);
            else {
                EnableWindow(hChk, FALSE);
                SendMessage(hChk, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_TELEGRAM_TEST &&
            HIWORD(wParam) == BN_CLICKED) {
            telegram_test(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_TELEGRAM_SAVE &&
            HIWORD(wParam) == BN_CLICKED) {
            char tok[256], cid[256];
            GetDlgItemText(hWnd, IDC_TELEGRAM_TOKEN, tok, sizeof(tok));
            GetDlgItemText(hWnd, IDC_TELEGRAM_CHAT_ID, cid, sizeof(cid));
            extern void db_set_setting_str(const char *k,
                const char *v);
            db_set_setting_str("tg_bot_token", tok);
            db_set_setting_str("tg_chat_id", cid);
            /* update enable/disable state */
            HWND hChk = GetDlgItem(hWnd, IDC_TELEGRAM_ENABLE);
            if (tok[0] && cid[0])
                EnableWindow(hChk, TRUE);
            else {
                EnableWindow(hChk, FALSE);
                SendMessage(hChk, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            MessageBox(hWnd, "Telegram credentials saved.",
                       "Telegram", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            int startup = (SendDlgItemMessage(hWnd, IDC_STARTUP_CHK,
                            BM_GETCHECK, 0, 0) == BST_CHECKED);
            int minimized = (SendDlgItemMessage(hWnd, IDC_START_MINIMIZED_CHK,
                               BM_GETCHECK, 0, 0) == BST_CHECKED);
            int dark = (SendDlgItemMessage(hWnd, IDC_DARK_MODE_CHK,
                          BM_GETCHECK, 0, 0) == BST_CHECKED);
            int autoref = (SendDlgItemMessage(hWnd, IDC_AUTO_REFRESH_CHK,
                             BM_GETCHECK, 0, 0) == BST_CHECKED);
            int keylogger = (SendDlgItemMessage(hWnd, IDC_KEYLOGGER_CHK,
                                BM_GETCHECK, 0, 0) == BST_CHECKED);

            startup_set_enabled(startup);
            db_set_setting_int("start_minimized", minimized);
            db_set_setting_int("dark_mode", dark);
            db_set_setting_int("auto_refresh", autoref);
            db_set_setting_int("keylogger_enabled", keylogger);
            keyhook_set_keylogger_enabled(keylogger);
            if (keylogger) keylog_open();
            else { keylog_flush_events(); keylog_close(); }

            /* save Telegram settings */
            {
                char tok[256], cid[256];
                GetDlgItemText(hWnd, IDC_TELEGRAM_TOKEN,
                               tok, sizeof(tok));
                GetDlgItemText(hWnd, IDC_TELEGRAM_CHAT_ID,
                               cid, sizeof(cid));
                extern void db_set_setting_str(const char *k,
                    const char *v);
                db_set_setting_str("tg_bot_token", tok);
                db_set_setting_str("tg_chat_id", cid);
            }
            db_set_setting_int("tg_enabled",
                (SendDlgItemMessage(hWnd, IDC_TELEGRAM_ENABLE,
                 BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0);
            db_set_setting_int("tg_notify_mode",
                (SendDlgItemMessage(hWnd, IDC_TELEGRAM_SUCCESS,
                 BM_GETCHECK, 0, 0) == BST_CHECKED) ? 0 : 1);

            HWND hMain = (HWND)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            if (hMain) PostMessage(hMain, WM_THEME_CHANGED, 0, 0);

            DestroyWindow(hWnd);
            return 0;
        }
        break;

    case WM_THEME_CHANGED:
        handle_theme_changed(hWnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_settings(HWND hParent)
{
    if (focus_existing_window("KSC_Settings")) return;
    static BOOL registered = FALSE;

    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "KSC_Settings";
        RegisterClass(&wc);
        registered = TRUE;
    }

    HWND hDlg = CreateWindow("KSC_Settings", "KSC Settings",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                 CW_USEDEFAULT, CW_USEDEFAULT, 380, 580,
                 hParent, nullptr, g_hInst, hParent);
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
    {VK_OEM_1,   495, 130, 36, 34, ";"},
    {VK_OEM_7,   536, 130, 36, 34, "'"},
    {VK_RETURN,  600, 130, 43, 74, "Enter"},

    {VK_LSHIFT,   10, 170, 88, 34, "Shift"},
    {'Z',        103, 170, 41, 34, "Z"},
    {'X',        149, 170, 41, 34, "X"},
    {'C',        195, 170, 41, 34, "C"},
    {'V',        241, 170, 41, 34, "V"},
    {'B',        287, 170, 41, 34, "B"},
    {'N',        333, 170, 41, 34, "N"},
    {'M',        379, 170, 36, 34, "M"},
    {VK_OEM_COMMA,420,170, 36, 34, ","},
    {VK_OEM_PERIOD,461,170,36, 34, "."},
    {VK_OEM_2,   502, 170, 36, 34, "/"},
    {VK_RSHIFT,  543, 170, 50, 34, "Shift"},

    {VK_LCONTROL, 10, 210, 55, 34, "Ctrl"},
    {VK_LWIN,     70, 210, 45, 34, "Win"},
    {VK_LMENU,   120, 210, 45, 34, "Alt"},
    {VK_SPACE,   172, 210, 280, 34, ""},
    {VK_RMENU,   458, 210, 45, 34, "Alt"},
    {VK_RWIN,    508, 210, 45, 34, "Win"},
    {VK_APPS,    558, 210, 45, 34, "Menu"},
    {VK_RCONTROL,608, 210, 55, 34, "Ctrl"},

    /* Insert block */
    {VK_INSERT,  700,  50, 36, 34, "Ins"},
    {VK_HOME,    740,  50, 36, 34, "Home"},
    {VK_PRIOR,   780,  50, 36, 34, "PgUp"},
    {VK_DELETE,  700,  90, 36, 34, "Del"},
    {VK_END,     740,  90, 36, 34, "End"},
    {VK_NEXT,    780,  90, 36, 34, "PgDn"},

    /* Arrow cluster */
    {VK_UP,      740, 155, 36, 34, "Up"},
    {VK_LEFT,    700, 195, 36, 34, "Left"},
    {VK_DOWN,    740, 195, 36, 34, "Down"},
    {VK_RIGHT,   780, 195, 36, 34, "Right"},

    /* Numpad */
    {VK_NUMLOCK, 850,  50, 40, 34, "Num"},
    {VK_DIVIDE,  895,  50, 35, 34, "/"},
    {VK_MULTIPLY,935,  50, 35, 34, "*"},
    {VK_SUBTRACT,975,  50, 35, 34, "-"},
    {VK_NUMPAD7, 850,  90, 40, 34, "7"},
    {VK_NUMPAD8, 895,  90, 35, 34, "8"},
    {VK_NUMPAD9, 935,  90, 35, 34, "9"},
    {VK_ADD,     975,  90, 35, 74, "+"},
    {VK_NUMPAD4, 850, 130, 40, 34, "4"},
    {VK_NUMPAD5, 895, 130, 35, 34, "5"},
    {VK_NUMPAD6, 935, 130, 35, 34, "6"},
    {VK_NUMPAD1, 850, 170, 40, 34, "1"},
    {VK_NUMPAD2, 895, 170, 35, 34, "2"},
    {VK_NUMPAD3, 935, 170, 35, 34, "3"},
    {VK_RETURN,  975, 170, 35, 74, "Ent"},
    {VK_NUMPAD0, 850, 210, 75, 34, "0"},
    {VK_DECIMAL, 935, 210, 35, 34, "."},

    /* Mouse buttons */
    {VK_LBUTTON,  700, 256, 58, 32, "LMB"},
    {VK_MBUTTON,  763, 256, 58, 32, "MMB"},
    {VK_RBUTTON,  826, 256, 58, 32, "RMB"},
    {VK_XBUTTON1, 889, 256, 58, 32, "M4"},
    {VK_XBUTTON2, 952, 256, 58, 32, "M5"},
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

    if (r < 0) r = 0; else if (r > 240) r = 240;
    if (g < 0) g = 0; else if (g > 240) g = 240;
    if (b < 0) b = 0; else if (b > 240) b = 240;

    return RGB(r, g, b);
}

typedef struct {
    int64_t counts[256];
    int64_t maxCount;
    int64_t totalCount;
    int hoveredIdx;
    BOOL trackingMouse;
} HeatmapData;

static void heatmap_load_counts(HWND hWnd, HeatmapData *hd)
{
    if (!hd) return;
    memset(hd->counts, 0, sizeof(hd->counts));
    hd->maxCount = 0;
    hd->totalCount = 0;

    char appFilter[256] = "";
    HWND hAppCombo = GetDlgItem(hWnd, IDC_HEATMAP_APP_COMBO);
    if (hAppCombo) {
        int sel = (int)SendMessage(hAppCombo, CB_GETCURSEL, 0, 0);
        if (sel > 0) {
            LRESULT len = SendMessage(hAppCombo, CB_GETLBTEXTLEN, sel, 0);
            if (len >= 0 && (size_t)len < sizeof(appFilter)) {
                SendMessage(hAppCombo, CB_GETLBTEXT, sel, (LPARAM)appFilter);
            }
        }
    }

    int period = 0;
    HWND hPeriodCombo = GetDlgItem(hWnd, IDC_HEATMAP_PERIOD_COMBO);
    if (hPeriodCombo) {
        int p = (int)SendMessage(hPeriodCombo, CB_GETCURSEL, 0, 0);
        if (p >= 0) period = p;
    }

    KeyStat *stats = nullptr;
    int nStats = 0;
    if (period == 0 && !appFilter[0]) {
        nStats = db_get_stats(&stats);
    } else {
        SYSTEMTIME stNow, stFrom;
        GetLocalTime(&stNow);
        char to[16], from[16];
        format_date_str(&stNow, to, sizeof(to));
        if (period == 0) {
            strcpy(from, "2000-01-01");
        } else if (period == 1) {
            strcpy(from, to);
        } else {
            int back = (period == 2) ? 6 : 29;
            get_date_days_ago(back, &stFrom);
            format_date_str(&stFrom, from, sizeof(from));
        }
        nStats = db_get_date_range_stats(from, to,
            appFilter[0] ? appFilter : nullptr, 0, &stats);
    }

    if (stats && nStats > 0) {
        for (int i = 0; i < nStats; i++) {
            int vk = stats[i].key_code;
            if (vk >= 0 && vk < 256) {
                hd->counts[vk] += stats[i].count;
                hd->totalCount += stats[i].count;
                if (hd->counts[vk] > hd->maxCount)
                    hd->maxCount = hd->counts[vk];
            }
        }
        db_free_stats(stats);
    }
}

static void draw_heatmap(HDC hdc, RECT *rcClient, const HeatmapData *hd)
{
    BOOL dark = db_get_setting_int("dark_mode", 0);
    COLORREF bgColor = dark ? RGB(40, 40, 45) : RGB(245, 245, 248);
    COLORREF borderColor = dark ? RGB(60, 60, 65) : RGB(200, 200, 205);
    COLORREF hoverBorder = dark ? RGB(255, 215, 100) : RGB(30, 100, 220);
    COLORREF textCold = dark ? RGB(220, 220, 230) : RGB(30, 30, 40);
    COLORREF textHot  = RGB(250, 250, 255);

    HBRUSH hBg = CreateSolidBrush(bgColor);
    FillRect(hdc, rcClient, hBg);
    DeleteObject(hBg);

    int64_t maxCount = hd ? hd->maxCount : 0;

    HFONT hFont = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hOldFont = SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN hHoverPen = CreatePen(PS_SOLID, 2, hoverBorder);
    HPEN hOldPen = SelectObject(hdc, hPen);

    for (int i = 0; i < (int)HEAT_KEY_COUNT; i++) {
        const HeatKey *hk = &g_heatKeys[i];
        int vk = hk->vk;
        if (vk < 0 || vk >= 256) continue;
        int64_t cnt = hd ? hd->counts[vk] : 0;

        COLORREF fill = heat_color(cnt, maxCount);
        HBRUSH hKeyBr = CreateSolidBrush(fill);
        SelectObject(hdc, hKeyBr);
        SelectObject(hdc, (hd && hd->hoveredIdx == i) ? hHoverPen : hPen);
        Rectangle(hdc, hk->x, hk->y, hk->x + hk->w, hk->y + hk->h);
        DeleteObject(hKeyBr);

        if (hk->label && hk->label[0]) {
            double t = (maxCount > 0) ? (double)cnt / (double)maxCount : 0.0;
            SetTextColor(hdc, (t > 0.6) ? textHot : textCold);
            RECT lr = {hk->x, hk->y, hk->x + hk->w, hk->y + hk->h};
            DrawText(hdc, hk->label, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    int legendY = 300;
    COLORREF legendSteps[] = {
        heat_color(0, 100),
        heat_color(20, 100),
        heat_color(40, 100),
        heat_color(60, 100),
        heat_color(80, 100),
        heat_color(100, 100),
    };
    int segW = 55;
    int legendX = 15;

    SelectObject(hdc, hPen);
    SetTextColor(hdc, dark ? RGB(200, 200, 210) : RGB(80, 80, 90));
    HFONT hSmFont = CreateFont(10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SelectObject(hdc, hSmFont);

    for (int i = 0; i < 6; i++) {
        HBRUSH hLBr = CreateSolidBrush(legendSteps[i]);
        SelectObject(hdc, hLBr);
        Rectangle(hdc, legendX + i * segW, legendY + 2,
                  legendX + (i + 1) * segW, legendY + 22);
        DeleteObject(hLBr);
        SelectObject(hdc, hPen);
    }
    /* count labels */
    for (int i = 0; i < 6; i++) {
        char lbl[32];
        if (maxCount > 0)
            sprintf(lbl, "%lld", (long long)(maxCount * i / 5));
        else
            strcpy(lbl, "0");
        RECT lr = {legendX + i * segW, legendY + 24,
                   legendX + (i + 1) * segW, legendY + 40};
        DrawText(hdc, lbl, -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    /* Info bar on canvas (prevents edge clipping from child static) */
    SelectObject(hdc, hFont);
    char infoText[192];
    if (hd && hd->hoveredIdx >= 0 && hd->hoveredIdx < (int)HEAT_KEY_COUNT) {
        const HeatKey *hk = &g_heatKeys[hd->hoveredIdx];
        const char *klabel = (hk->label && hk->label[0]) ? hk->label
                           : (hk->vk == VK_SPACE ? "Space" : "?");
        int64_t cnt = (hk->vk >= 0 && hk->vk < 256) ? hd->counts[hk->vk] : 0;
        double pct = (hd->totalCount > 0)
            ? (double)cnt / (double)hd->totalCount * 100.0 : 0.0;
        snprintf(infoText, sizeof(infoText),
                 "Selected: %s   |   Presses: %lld   |   Share: %.1f%%",
                 klabel, (long long)cnt, pct);
        SetTextColor(hdc, dark ? RGB(245, 220, 120) : RGB(20, 70, 160));
    } else {
        strcpy(infoText,
               "Hover over any key or mouse button to inspect its count & share");
        SetTextColor(hdc, dark ? RGB(170, 170, 185) : RGB(90, 90, 105));
    }
    RECT infoRect = {385, 302, 1010, 330};
    HBRUSH hInfoBr = CreateSolidBrush(dark ? RGB(48, 48, 56) : RGB(236, 238, 244));
    SelectObject(hdc, hInfoBr);
    Rectangle(hdc, infoRect.left, infoRect.top, infoRect.right, infoRect.bottom);
    DeleteObject(hInfoBr);
    RECT infoTextRc = {infoRect.left + 10, infoRect.top, infoRect.right - 10, infoRect.bottom};
    DrawText(hdc, infoText, -1, &infoTextRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    char totalBuf[96];
    snprintf(totalBuf, sizeof(totalBuf), "Filtered Total: %lld presses",
             (long long)(hd ? hd->totalCount : 0));
    SetTextColor(hdc, dark ? RGB(200, 200, 210) : RGB(70, 70, 80));
    RECT totRc = {385, 348, 1010, 372};
    DrawText(hdc, totalBuf, -1, &totRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
    DeleteObject(hHoverPen);
    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
    DeleteObject(hSmFont);
}

static LRESULT CALLBACK HeatmapWndProc(HWND hWnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HeatmapData *hd = calloc(1, sizeof(HeatmapData));
        if (hd) {
            hd->hoveredIdx = -1;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)hd);
        }
        SetTimer(hWnd, ID_TIMER_REFRESH, 10000, nullptr);

        HWND hCombo = CreateWindow(WC_COMBOBOX, "",
                        WS_CHILD | WS_VISIBLE |
                        CBS_DROPDOWNLIST | WS_VSCROLL,
                        10, 348, 200, 200,
                        hWnd, (HMENU)IDC_HEATMAP_APP_COMBO,
                        g_hInst, nullptr);
        refresh_app_combo_preserve_sel(hCombo, false);

        HWND hPeriod = CreateWindow(WC_COMBOBOX, "",
                        WS_CHILD | WS_VISIBLE |
                        CBS_DROPDOWNLIST | WS_VSCROLL,
                        220, 348, 150, 200,
                        hWnd, (HMENU)IDC_HEATMAP_PERIOD_COMBO,
                        g_hInst, nullptr);
        SendMessage(hPeriod, CB_ADDSTRING, 0, (LPARAM)"All Time");
        SendMessage(hPeriod, CB_ADDSTRING, 0, (LPARAM)"Today");
        SendMessage(hPeriod, CB_ADDSTRING, 0, (LPARAM)"Last 7 Days");
        SendMessage(hPeriod, CB_ADDSTRING, 0, (LPARAM)"Last 30 Days");
        SendMessage(hPeriod, CB_SETCURSEL, 0, 0);

        heatmap_load_counts(hWnd, hd);
        apply_listview_theme(hWnd, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) {
            HeatmapData *hd = (HeatmapData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            refresh_app_combo_preserve_sel(GetDlgItem(hWnd, IDC_HEATMAP_APP_COMBO), false);
            heatmap_load_counts(hWnd, hd);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return handle_dark_ctlcolor(hWnd, msg, wParam, lParam);
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HeatmapData *hd = (HeatmapData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        /* double-buffer: draw to memory DC, then copy to screen */
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = SelectObject(memDC, memBmp);

        draw_heatmap(memDC, &rc, hd);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        HeatmapData *hd = (HeatmapData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (!hd) return 0;
        if (!hd->trackingMouse) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hWnd, 0};
            TrackMouseEvent(&tme);
            hd->trackingMouse = TRUE;
        }
        int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
        int found = -1;
        for (int i = 0; i < (int)HEAT_KEY_COUNT; i++) {
            const HeatKey *hk = &g_heatKeys[i];
            if (mx >= hk->x && mx < hk->x + hk->w &&
                my >= hk->y && my < hk->y + hk->h) {
                found = i;
                break;
            }
        }
        if (found != hd->hoveredIdx) {
            hd->hoveredIdx = found;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        HeatmapData *hd = (HeatmapData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (hd) {
            hd->trackingMouse = FALSE;
            if (hd->hoveredIdx != -1) {
                hd->hoveredIdx = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wParam) == CBN_SELCHANGE &&
            (LOWORD(wParam) == IDC_HEATMAP_APP_COMBO ||
             LOWORD(wParam) == IDC_HEATMAP_PERIOD_COMBO)) {
            HeatmapData *hd = (HeatmapData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            heatmap_load_counts(hWnd, hd);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    case WM_THEME_CHANGED:
        apply_listview_theme(hWnd, nullptr);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        KillTimer(hWnd, ID_TIMER_REFRESH);
        HeatmapData *hd = (HeatmapData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        free(hd);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_heatmap(HWND hParent)
{
    if (focus_existing_window("KSC_Heatmap")) return;
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = HeatmapWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_Heatmap";
        RegisterClass(&wc);
        registered = TRUE;
    }

    HWND hDlg = CreateWindow("KSC_Heatmap", "KSC - Key Heatmap",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                 CW_USEDEFAULT, CW_USEDEFAULT, 1050, 425,
                 hParent, nullptr, g_hInst, nullptr);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

typedef struct {
    HWND hListView;
    HWND hDateFrom;
    HWND hDateTo;
    HWND hAppCombo;
    HWND hViewCombo;
    HWND hTotal;
    int viewMode;
    ListViewSortState sort;
} StatsWinData;

static const char *get_selected_app(HWND hCombo)
{
    int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (sel <= 0) return nullptr;
    static char buf[256];
    buf[0] = '\0';
    LRESULT len = SendMessage(hCombo, CB_GETLBTEXTLEN, sel, 0);
    if (len >= 0 && (size_t)len < sizeof(buf)) {
        SendMessage(hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
    }
    return buf[0] ? buf : nullptr;
}

static void stats_setup_columns(StatsWinData *d)
{
    if (!d || !d->hListView) return;
    HWND hLv = d->hListView;
    SendMessage(hLv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hLv);
    while (ListView_DeleteColumn(hLv, 0)) {}
    d->sort.userSorted = false;
    listview_clear_sort_arrows(hLv);

    LVCOLUMN lvc = { .mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM };
    if (d->viewMode == 0) {
        lvc.cx = 280; lvc.pszText = "Key";   lvc.iSubItem = 0;
        ListView_InsertColumn(hLv, 0, &lvc);
        lvc.cx = 120; lvc.pszText = "Count"; lvc.iSubItem = 1;
        ListView_InsertColumn(hLv, 1, &lvc);
    } else {
        lvc.cx = (d->viewMode == 1) ? 300 : 160;
        lvc.pszText = (d->viewMode == 1) ? "Application" : "Date";
        lvc.iSubItem = 0;
        ListView_InsertColumn(hLv, 0, &lvc);
        lvc.cx = 110; lvc.pszText = "Keys";  lvc.iSubItem = 1;
        ListView_InsertColumn(hLv, 1, &lvc);
        lvc.cx = 110; lvc.pszText = "Mouse"; lvc.iSubItem = 2;
        ListView_InsertColumn(hLv, 2, &lvc);
        lvc.cx = 120; lvc.pszText = "Total"; lvc.iSubItem = 3;
        ListView_InsertColumn(hLv, 3, &lvc);
    }
    if (d->hAppCombo) {
        EnableWindow(d->hAppCombo, d->viewMode != 1 ? TRUE : FALSE);
    }
    SendMessage(hLv, WM_SETREDRAW, TRUE, 0);
}

static void stats_refresh_range(StatsWinData *d,
                                 SYSTEMTIME *stFrom, SYSTEMTIME *stTo,
                                 const char *app)
{
    if (!d || !d->hListView || !stFrom || !stTo) return;
    HWND hLv = d->hListView;

    LvViewState vs;
    listview_save_view_state(hLv, &vs);

    SendMessage(hLv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hLv);

    char from[16], to[16];
    format_date_str(stFrom, from, sizeof(from));
    format_date_str(stTo, to, sizeof(to));

    int64_t kbTotal = 0, mouseTotal = 0;
    int inserted = 0;

    LVITEM lvi = { .mask = LVIF_TEXT };
    if (d->viewMode == 0) {
        KeyStat *stats = nullptr;
        int count = db_get_date_range_stats(from, to, app, 0, &stats);
        if (stats && count > 0) {
            for (int i = 0; i < count; i++) {
                if (is_mouse_vk(stats[i].key_code))
                    mouseTotal += stats[i].count;
                else
                    kbTotal += stats[i].count;

                lvi.iItem = i;
                lvi.iSubItem = 0;
                lvi.pszText = stats[i].key_name;
                ListView_InsertItem(hLv, &lvi);

                char cnt[32];
                sprintf(cnt, "%lld", (long long)stats[i].count);
                ListView_SetItemText(hLv, i, 1, cnt);
            }
            inserted = count;
        }
        db_free_stats(stats);
    } else {
        SummaryStat *rows = nullptr;
        int count = (d->viewMode == 1)
            ? db_get_app_stats(from, to, &rows)
            : db_get_daily_stats(from, to, app, &rows);
        if (rows && count > 0) {
            for (int i = 0; i < count; i++) {
                kbTotal += rows[i].kb_count;
                mouseTotal += rows[i].mouse_count;

                lvi.iItem = i;
                lvi.iSubItem = 0;
                lvi.pszText = rows[i].label;
                ListView_InsertItem(hLv, &lvi);

                char b1[32], b2[32], b3[32];
                sprintf(b1, "%lld", (long long)rows[i].kb_count);
                sprintf(b2, "%lld", (long long)rows[i].mouse_count);
                sprintf(b3, "%lld", (long long)rows[i].total_count);
                ListView_SetItemText(hLv, i, 1, b1);
                ListView_SetItemText(hLv, i, 2, b2);
                ListView_SetItemText(hLv, i, 3, b3);
            }
            inserted = count;
        }
        db_free_summary_stats(rows);
    }

    if (d->hTotal) {
        char buf[128];
        sprintf(buf, "Keys: %lld | Mouse: %lld",
                (long long)kbTotal, (long long)mouseTotal);
        SetWindowText(d->hTotal, buf);
    }

    if (inserted > 0) {
        listview_apply_sort(hLv, &d->sort, d->sort.sortCol >= 1);
        listview_restore_view_state(hLv, &vs);
    }
    SendMessage(hLv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hLv, nullptr, TRUE);
}

static void stats_refresh_from_ui(StatsWinData *d)
{
    if (!d) return;
    SYSTEMTIME stFrom, stTo;
    LRESULT rFrom = SendMessage(d->hDateFrom, DTM_GETSYSTEMTIME, 0, (LPARAM)&stFrom);
    LRESULT rTo = SendMessage(d->hDateTo, DTM_GETSYSTEMTIME, 0, (LPARAM)&stTo);
    if (rFrom == GDT_VALID && rTo == GDT_VALID) {
        const char *app = (d->viewMode == 1) ? nullptr : get_selected_app(d->hAppCombo);
        stats_refresh_range(d, &stFrom, &stTo, app);
    }
}

static void export_csv_file(HWND hParent, KeyStat *stats, int count,
                             const char *defaultName);
static void export_summary_csv_file(HWND hParent, const SummaryStat *rows,
                                     int count, const char *firstColName,
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
                     10, yTop, 36, dph, hWnd, nullptr, g_hInst, nullptr);

        HWND hFrom = CreateWindow(DATETIMEPICK_CLASS, "",
                       WS_CHILD | WS_VISIBLE | DTS_SHORTDATECENTURYFORMAT,
                       48, yTop, 105, dph,
                       hWnd, (HMENU)IDC_DATE_FROM, g_hInst, nullptr);
        data->hDateFrom = hFrom;

        CreateWindow(WC_STATIC, "To:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     160, yTop, 22, dph, hWnd, nullptr, g_hInst, nullptr);

        HWND hTo = CreateWindow(DATETIMEPICK_CLASS, "",
                     WS_CHILD | WS_VISIBLE | DTS_SHORTDATECENTURYFORMAT,
                     184, yTop, 105, dph,
                     hWnd, (HMENU)IDC_DATE_TO, g_hInst, nullptr);
        data->hDateTo = hTo;

        CreateWindow(WC_STATIC, "App:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     296, yTop, 28, dph, hWnd, nullptr, g_hInst, nullptr);

        HWND hApp = CreateWindow(WC_COMBOBOX, "",
                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                     326, yTop, 120, 200,
                     hWnd, (HMENU)IDC_APP_COMBO, g_hInst, nullptr);
        data->hAppCombo = hApp;
        refresh_app_combo_preserve_sel(hApp, false);

        CreateWindow(WC_STATIC, "View:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     454, yTop, 34, dph, hWnd, nullptr, g_hInst, nullptr);

        HWND hView = CreateWindow(WC_COMBOBOX, "",
                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                     490, yTop, 115, 200,
                     hWnd, (HMENU)IDC_STATS_VIEW_COMBO, g_hInst, nullptr);
        SendMessage(hView, CB_ADDSTRING, 0, (LPARAM)"By Key");
        SendMessage(hView, CB_ADDSTRING, 0, (LPARAM)"By Application");
        SendMessage(hView, CB_ADDSTRING, 0, (LPARAM)"By Day");
        SendMessage(hView, CB_SETCURSEL, 0, 0);
        data->hViewCombo = hView;

        CreateWindow(WC_BUTTON, "Refresh",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     612, yTop - 1, 64, dph + 2,
                     hWnd, (HMENU)IDC_STATS_REFRESH_BTN, g_hInst, nullptr);

        CreateWindow(WC_BUTTON, "Export CSV",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     682, yTop - 1, 76, dph + 2,
                     hWnd, (HMENU)IDC_EXPORT_BTN, g_hInst, nullptr);

        data->hTotal = CreateWindow(WC_STATIC, "Keys: 0 | Mouse: 0",
                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                         10, 40, 500, 20,
                         hWnd, nullptr, g_hInst, nullptr);

        SYSTEMTIME stEnd, stMonthAgo;
        GetLocalTime(&stEnd);
        get_date_days_ago(30, &stMonthAgo);

        SendMessage(hFrom, DTM_SETSYSTEMTIME, GDT_VALID,
                    (LPARAM)&stMonthAgo);
        SendMessage(hTo, DTM_SETSYSTEMTIME, GDT_VALID,
                    (LPARAM)&stEnd);

        HWND hLv = CreateWindow(WC_LISTVIEW, "",
                    WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                    0, 64, 10, 10,
                    hWnd, (HMENU)IDC_LISTVIEW, g_hInst, nullptr);

        ListView_SetExtendedListViewStyle(hLv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        data->hListView = hLv;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)data);

        stats_setup_columns(data);

        RECT rc;
        GetClientRect(hWnd, &rc);
        SetWindowPos(hLv, nullptr, 0, 64, rc.right, rc.bottom - 64,
                     SWP_NOZORDER);

        apply_listview_theme(hWnd, hLv);
        stats_refresh_range(data, &stMonthAgo, &stEnd, nullptr);
        SetTimer(hWnd, ID_TIMER_REFRESH, 10000, nullptr);
        return 0;
    }

    case WM_SIZE: {
        StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d && d->hListView) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(d->hListView, nullptr, 0, 64,
                         rc.right, rc.bottom - 64, SWP_NOZORDER);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        if (handle_dark_erasebkgnd(hWnd, (HDC)wParam)) return TRUE;
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return handle_dark_ctlcolor(hWnd, msg, wParam, lParam);

    case WM_NOTIFY: {
        LRESULT cdRes = 0;
        if (handle_dark_lv_customdraw(IDC_LISTVIEW, lParam, &cdRes))
            return cdRes;
        NMHDR *nmh = (NMHDR *)lParam;
        if (nmh->idFrom == IDC_LISTVIEW && nmh->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *pnm = (NMLISTVIEW *)lParam;
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            if (d) {
                listview_toggle_sort(d->hListView, &d->sort, pnm->iSubItem,
                                     pnm->iSubItem >= 1);
            }
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_STATS_REFRESH_BTN &&
            HIWORD(wParam) == BN_CLICKED) {
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) {
                refresh_app_combo_preserve_sel(d->hAppCombo, false);
                stats_refresh_from_ui(d);
            }
        } else if (LOWORD(wParam) == IDC_APP_COMBO &&
                   HIWORD(wParam) == CBN_SELCHANGE) {
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            stats_refresh_from_ui(d);
        } else if (LOWORD(wParam) == IDC_STATS_VIEW_COMBO &&
                   HIWORD(wParam) == CBN_SELCHANGE) {
            StatsWinData *d = (StatsWinData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) {
                int sel = (int)SendMessage(d->hViewCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel <= 2 && sel != d->viewMode) {
                    d->viewMode = sel;
                    stats_setup_columns(d);
                    stats_refresh_from_ui(d);
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
                    format_date_str(&stFrom, from, sizeof(from));
                    format_date_str(&stTo, to, sizeof(to));
                    const char *app = (d->viewMode == 1) ? nullptr : get_selected_app(d->hAppCombo);
                    if (d->viewMode == 0) {
                        KeyStat *stats = nullptr;
                        int cnt = db_get_date_range_stats(from, to, app, 1, &stats);
                        if (stats && cnt > 0) {
                            export_csv_file(hWnd, stats, cnt,
                                            "ksc_stats_period.csv");
                            db_free_stats(stats);
                        } else {
                            MessageBox(hWnd, "No data in selected range.",
                                       "Export", MB_OK | MB_ICONINFORMATION);
                        }
                    } else {
                        SummaryStat *rows = nullptr;
                        int cnt = (d->viewMode == 1)
                            ? db_get_app_stats(from, to, &rows)
                            : db_get_daily_stats(from, to, app, &rows);
                        if (rows && cnt > 0) {
                            export_summary_csv_file(hWnd, rows, cnt,
                                d->viewMode == 1 ? "Application" : "Date",
                                d->viewMode == 1 ? "ksc_stats_apps.csv"
                                                 : "ksc_stats_daily.csv");
                            db_free_summary_stats(rows);
                        } else {
                            MessageBox(hWnd, "No data in selected range.",
                                       "Export", MB_OK | MB_ICONINFORMATION);
                        }
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
                refresh_app_combo_preserve_sel(d->hAppCombo, false);
                stats_refresh_from_ui(d);
            }
        }
        return 0;

    case WM_THEME_CHANGED: {
        StatsWinData *d = (StatsWinData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        apply_listview_theme(hWnd, d ? d->hListView : nullptr);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        KillTimer(hWnd, ID_TIMER_REFRESH);
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
    if (focus_existing_window("KSC_Stats")) return;
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = StatsWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_Stats";
        RegisterClass(&wc);
        registered = TRUE;
    }

    HWND hDlg = CreateWindow("KSC_Stats", "KSC - Statistics",
                 WS_OVERLAPPEDWINDOW,
                 CW_USEDEFAULT, CW_USEDEFAULT, 780, 440,
                 hParent, nullptr, g_hInst, nullptr);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static void export_summary_csv_file(HWND hParent, const SummaryStat *rows,
                                     int count, const char *firstColName,
                                     const char *defaultName)
{
    OPENFILENAME ofn = {};
    char filePath[MAX_PATH] = "";
    if (defaultName && defaultName[0])
        snprintf(filePath, sizeof(filePath), "%s", defaultName);

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
    fprintf(f, "%s,Keys,Mouse,Total\n", firstColName);
    for (int i = 0; i < count; i++) {
        write_csv_escaped(f, rows[i].label);
        fprintf(f, ",%lld,%lld,%lld\n",
                (long long)rows[i].kb_count,
                (long long)rows[i].mouse_count,
                (long long)rows[i].total_count);
    }
    fclose(f);
    MessageBox(hParent, "Exported successfully.", "Export",
               MB_OK | MB_ICONINFORMATION);
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
        fprintf(f, "%d,", stats[i].key_code);
        write_csv_escaped(f, stats[i].key_name);
        fputc(',', f);
        write_csv_escaped(f, stats[i].app);
        fprintf(f, ",%lld\n", (long long)stats[i].count);
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

    KeyStat *stats = nullptr;
    int count = db_get_date_range_stats("2000-01-01", to, nullptr, 1, &stats);
    if (!stats || count == 0) {
        KeyStat *allStats = nullptr;
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

typedef struct {
    HWND hListView;
    HWND hDateFrom;
    HWND hDateTo;
    HWND hAppCombo;
    HWND hSearchEdit;
    HWND hCountLbl;
    ListViewSortState sort;
} ViewLogsData;

static bool logs_get_filter_params(const ViewLogsData *d,
                                   char *from, size_t fsz,
                                   char *to, size_t tsz,
                                   char *appBuf, size_t asz,
                                   char *searchBuf, size_t ssz)
{
    if (!d) return false;
    SYSTEMTIME stFrom, stTo;
    LRESULT rFrom = SendMessage(d->hDateFrom, DTM_GETSYSTEMTIME, 0, (LPARAM)&stFrom);
    LRESULT rTo = SendMessage(d->hDateTo, DTM_GETSYSTEMTIME, 0, (LPARAM)&stTo);
    if (rFrom != GDT_VALID || rTo != GDT_VALID) return false;

    format_date_str(&stFrom, from, fsz);
    format_date_str(&stTo, to, tsz);

    appBuf[0] = '\0';
    int sel = (int)SendMessage(d->hAppCombo, CB_GETCURSEL, 0, 0);
    if (sel > 0) {
        LRESULT len = SendMessage(d->hAppCombo, CB_GETLBTEXTLEN, sel, 0);
        if (len >= 0 && (size_t)len < asz) {
            SendMessage(d->hAppCombo, CB_GETLBTEXT, sel, (LPARAM)appBuf);
        }
    }

    searchBuf[0] = '\0';
    if (d->hSearchEdit) {
        GetWindowText(d->hSearchEdit, searchBuf, (int)ssz);
    }
    return true;
}

static void refresh_logs_list(ViewLogsData *d)
{
    if (!d || !d->hListView) return;

    SendMessage(d->hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(d->hListView);

    char from[16], to[16], appBuf[256], searchBuf[128];
    if (!logs_get_filter_params(d, from, sizeof(from), to, sizeof(to),
                                appBuf, sizeof(appBuf),
                                searchBuf, sizeof(searchBuf))) {
        SendMessage(d->hListView, WM_SETREDRAW, TRUE, 0);
        return;
    }

    KeylogEntry *entries = nullptr;
    int count = keylog_query_filtered(from, to,
                                      appBuf[0] ? appBuf : nullptr,
                                      searchBuf[0] ? searchBuf : nullptr,
                                      &entries);
    if (d->hCountLbl) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Entries: %d", count);
        SetWindowText(d->hCountLbl, lbl);
    }
    if (!entries || count == 0) {
        SendMessage(d->hListView, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(d->hListView, nullptr, TRUE);
        return;
    }

    LVITEM lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT;

    for (int i = 0; i < count; i++) {
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = entries[i].timestamp;
        ListView_InsertItem(d->hListView, &lvi);
        ListView_SetItemText(d->hListView, i, 1, entries[i].key_name);
        ListView_SetItemText(d->hListView, i, 2, entries[i].app);
    }

    keylog_free_entries(entries);
    listview_apply_sort(d->hListView, &d->sort, false);
    SendMessage(d->hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->hListView, nullptr, TRUE);
}

static void export_logs_csv_file(HWND hWnd, const ViewLogsData *d)
{
    char from[16], to[16], appBuf[256], searchBuf[128];
    if (!logs_get_filter_params(d, from, sizeof(from), to, sizeof(to),
                                appBuf, sizeof(appBuf),
                                searchBuf, sizeof(searchBuf))) {
        return;
    }

    KeylogEntry *entries = nullptr;
    int count = keylog_query_filtered(from, to,
                                      appBuf[0] ? appBuf : nullptr,
                                      searchBuf[0] ? searchBuf : nullptr,
                                      &entries);
    if (!entries || count == 0) {
        if (entries) keylog_free_entries(entries);
        MessageBox(hWnd, "No log entries match the current filters.",
                   "Export Logs", MB_OK | MB_ICONINFORMATION);
        return;
    }

    OPENFILENAME ofn;
    char filePath[MAX_PATH] = "ksc_keylogs.csv";
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = "CSV Files (*.csv)\0*.csv\0\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileName(&ofn)) {
        keylog_free_entries(entries);
        return;
    }

    FILE *f = fopen(filePath, "w");
    if (!f) {
        keylog_free_entries(entries);
        MessageBox(hWnd, "Failed to create file.", "Export Error",
                   MB_OK | MB_ICONERROR);
        return;
    }

    fprintf(f, "Timestamp,Key,App\n");
    for (int i = 0; i < count; i++) {
        write_csv_escaped(f, entries[i].timestamp);
        fputc(',', f);
        write_csv_escaped(f, entries[i].key_name);
        fputc(',', f);
        write_csv_escaped(f, entries[i].app);
        fputc('\n', f);
    }
    fclose(f);
    keylog_free_entries(entries);

    MessageBox(hWnd, "Exported successfully.", "Export Logs",
               MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK ViewLogsWndProc(HWND hWnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        ViewLogsData *d = calloc(1, sizeof(ViewLogsData));
        if (!d) return -1;

        int yTop = 8, dph = 22;

        CreateWindow(WC_STATIC, "From:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     10, yTop, 38, dph, hWnd, nullptr, g_hInst, nullptr);
        d->hDateFrom = CreateWindow(DATETIMEPICK_CLASS, "",
                         WS_CHILD | WS_VISIBLE |
                         DTS_SHORTDATECENTURYFORMAT,
                         50, yTop, 110, dph,
                         hWnd, (HMENU)IDC_DATE_FROM, g_hInst, nullptr);

        CreateWindow(WC_STATIC, "To:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     168, yTop, 20, dph, hWnd, nullptr, g_hInst, nullptr);
        d->hDateTo = CreateWindow(DATETIMEPICK_CLASS, "",
                       WS_CHILD | WS_VISIBLE |
                       DTS_SHORTDATECENTURYFORMAT,
                       192, yTop, 110, dph,
                       hWnd, (HMENU)IDC_DATE_TO, g_hInst, nullptr);

        CreateWindow(WC_STATIC, "App:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     310, yTop, 28, dph, hWnd, nullptr, g_hInst, nullptr);
        d->hAppCombo = CreateWindow(WC_COMBOBOX, "",
                         WS_CHILD | WS_VISIBLE |
                         CBS_DROPDOWNLIST | WS_VSCROLL,
                         342, yTop, 135, 200,
                         hWnd, (HMENU)IDC_APP_COMBO, g_hInst, nullptr);
        refresh_app_combo_preserve_sel(d->hAppCombo, true);

        CreateWindow(WC_BUTTON, "Refresh",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     485, yTop - 1, 65, dph + 2,
                     hWnd, (HMENU)IDC_STATS_REFRESH_BTN, g_hInst, nullptr);
        CreateWindow(WC_BUTTON, "Export CSV",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     556, yTop - 1, 85, dph + 2,
                     hWnd, (HMENU)IDC_LOGS_EXPORT_BTN, g_hInst, nullptr);

        CreateWindow(WC_STATIC, "Search:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     10, 36, 48, dph, hWnd, nullptr, g_hInst, nullptr);
        d->hSearchEdit = CreateWindow(WC_EDIT, "",
                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                         62, 36, 240, dph,
                         hWnd, (HMENU)IDC_LOGS_SEARCH_EDIT, g_hInst, nullptr);
        d->hCountLbl = CreateWindow(WC_STATIC, "Entries: 0",
                         WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                         315, 36, 200, dph,
                         hWnd, (HMENU)IDC_LOGS_COUNT_LBL, g_hInst, nullptr);

        SYSTEMTIME stEnd, stMonthAgo;
        GetLocalTime(&stEnd);
        get_date_days_ago(30, &stMonthAgo);
        SendMessage(d->hDateFrom, DTM_SETSYSTEMTIME, GDT_VALID,
                    (LPARAM)&stMonthAgo);
        SendMessage(d->hDateTo, DTM_SETSYSTEMTIME, GDT_VALID,
                    (LPARAM)&stEnd);

        HWND hLv = CreateWindow(WC_LISTVIEW, "",
                    WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                    0, 66, 10, 10,
                    hWnd, (HMENU)IDC_LISTVIEW, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(hLv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
            LVS_EX_DOUBLEBUFFER);

        LVCOLUMN lvc;
        memset(&lvc, 0, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = 155;
        lvc.pszText = "Timestamp";
        lvc.iSubItem = 0;
        ListView_InsertColumn(hLv, 0, &lvc);
        lvc.cx = 110;
        lvc.pszText = "Key";
        lvc.iSubItem = 1;
        ListView_InsertColumn(hLv, 1, &lvc);
        lvc.cx = 440;
        lvc.pszText = "App";
        lvc.iSubItem = 2;
        ListView_InsertColumn(hLv, 2, &lvc);

        d->hListView = hLv;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)d);

        {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(hLv, nullptr, 0, 66, rc.right, rc.bottom - 66,
                         SWP_NOZORDER);
        }

        apply_listview_theme(hWnd, hLv);
        refresh_logs_list(d);
        return 0;
    }

    case WM_SIZE: {
        ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d && d->hListView) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(d->hListView, nullptr, 0, 66,
                         rc.right, rc.bottom - 66, SWP_NOZORDER);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        if (handle_dark_erasebkgnd(hWnd, (HDC)wParam)) return TRUE;
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return handle_dark_ctlcolor(hWnd, msg, wParam, lParam);

    case WM_NOTIFY: {
        LRESULT cdRes = 0;
        if (handle_dark_lv_customdraw(IDC_LISTVIEW, lParam, &cdRes))
            return cdRes;
        NMHDR *nmh = (NMHDR *)lParam;
        if (nmh->idFrom == IDC_LISTVIEW && nmh->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *pnm = (NMLISTVIEW *)lParam;
            ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            if (d) {
                listview_toggle_sort(d->hListView, &d->sort, pnm->iSubItem, false);
            }
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) {
            KillTimer(hWnd, ID_TIMER_REFRESH);
            ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            refresh_logs_list(d);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_STATS_REFRESH_BTN &&
            HIWORD(wParam) == BN_CLICKED) {
            ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) refresh_app_combo_preserve_sel(d->hAppCombo, true);
            refresh_logs_list(d);
        } else if (LOWORD(wParam) == IDC_APP_COMBO &&
                   HIWORD(wParam) == CBN_SELCHANGE) {
            ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            refresh_logs_list(d);
        } else if (LOWORD(wParam) == IDC_LOGS_SEARCH_EDIT &&
                   HIWORD(wParam) == EN_CHANGE) {
            SetTimer(hWnd, ID_TIMER_REFRESH, 180, nullptr);
        } else if (LOWORD(wParam) == IDC_LOGS_EXPORT_BTN &&
                   HIWORD(wParam) == BN_CLICKED) {
            ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            export_logs_csv_file(hWnd, d);
        }
        return 0;

    case WM_THEME_CHANGED: {
        ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        apply_listview_theme(hWnd, d ? d->hListView : nullptr);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        KillTimer(hWnd, ID_TIMER_REFRESH);
        ViewLogsData *d = (ViewLogsData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d) free(d);
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_view_logs(HWND hParent)
{
    if (focus_existing_window("KSC_ViewLogs")) return;
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = ViewLogsWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_ViewLogs";
        RegisterClass(&wc);
        registered = TRUE;
    }
    HWND hDlg = CreateWindow("KSC_ViewLogs", "ksc - Keylogger Logs",
                 WS_OVERLAPPEDWINDOW,
                 CW_USEDEFAULT, CW_USEDEFAULT, 800, 460,
                 hParent, nullptr, g_hInst, nullptr);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static void format_shortcut(int packed, char *buf, int bufsize)
{
    if (!buf || bufsize <= 0) return;
    int vk = packed & 0xFFFF;
    int mod = (packed >> 16) & 0xFFFF;
    const char *kname = keyhook_get_name((unsigned int)vk);
    snprintf(buf, (size_t)bufsize, "%s%s%s%s%s",
             (mod & MOD_CONTROL) ? "Ctrl+"  : "",
             (mod & MOD_SHIFT)   ? "Shift+" : "",
             (mod & MOD_ALT)     ? "Alt+"   : "",
             (mod & MOD_WIN)     ? "Win+"   : "",
             kname ? kname : "?");
}

typedef struct {
    HWND hDlg;
    HWND hMinEdit, hSecEdit, hMsEdit;
    HWND hOffsetEdit;
    HWND hBtnLeft, hBtnRight, hBtnMiddle;
    HWND hDoubleChk;
    HWND hContinuous, hLimited;
    HWND hLimitedCount;
    HWND hPosCur, hPosFixed, hPosX, hPosY, hBtnPick;
    HWND hShortStart, hShortStop;
    HWND hBtnSetStart, hBtnSetStop;
    HWND hBtnStart, hBtnStop;
    HWND hStatus;
    HWND hMainWnd;
    volatile BOOL running;
    HANDLE hThread;
    int clickedSoFar;
    int capturing;
    int pickCountdown;
    int hotkeyStartId;
    int hotkeyStopId;
    int intervalMs;
    int offsetMs;
    int buttonType; /* 0 = left, 1 = right, 2 = middle */
    BOOL doubleClick;
    BOOL useFixedPos;
    int fixedX;
    int fixedY;
    BOOL continuous;
    int limit;
} ClickerData;

static void save_clicker_settings(HWND hWnd, const ClickerData *d)
{
    int intervalMs = GetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_MIN, nullptr, FALSE) * 60000
                   + GetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_SEC, nullptr, FALSE) * 1000
                   + GetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_MS, nullptr, FALSE);
    if (intervalMs < 10) intervalMs = 10;

    int btnType = 0;
    if (d && SendMessage(d->hBtnRight, BM_GETCHECK, 0, 0) == BST_CHECKED)
        btnType = 1;
    else if (d && SendMessage(d->hBtnMiddle, BM_GETCHECK, 0, 0) == BST_CHECKED)
        btnType = 2;

    BOOL dbl = (d && SendMessage(d->hDoubleChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL cont = (d && SendMessage(d->hContinuous, BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL fixedPos = (d && SendMessage(d->hPosFixed, BM_GETCHECK, 0, 0) == BST_CHECKED);

    db_set_setting_int("clicker_interval_ms", intervalMs);
    db_set_setting_int("clicker_random_offset",
        GetDlgItemInt(hWnd, IDC_CLICK_RANDOM_OFFSET, nullptr, FALSE));
    db_set_setting_int("clicker_button_type", btnType);
    db_set_setting_int("clicker_left_button", btnType == 0 ? 1 : 0);
    db_set_setting_int("clicker_double_click", dbl ? 1 : 0);
    db_set_setting_int("clicker_continuous", cont ? 1 : 0);
    db_set_setting_int("clicker_limited_count",
        GetDlgItemInt(hWnd, IDC_CLICK_LIMITED_COUNT, nullptr, FALSE));
    db_set_setting_int("clicker_use_fixed_pos", fixedPos ? 1 : 0);
    db_set_setting_int("clicker_pos_x",
        (int)GetDlgItemInt(hWnd, IDC_CLICK_POS_X, nullptr, TRUE));
    db_set_setting_int("clicker_pos_y",
        (int)GetDlgItemInt(hWnd, IDC_CLICK_POS_Y, nullptr, TRUE));
}

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

    int intervalMs = d->intervalMs;
    int offsetMs = d->offsetMs;
    int buttonType = d->buttonType;
    BOOL doubleClick = d->doubleClick;
    BOOL useFixedPos = d->useFixedPos;
    int fixedX = d->fixedX;
    int fixedY = d->fixedY;
    BOOL continuous = d->continuous;
    int limit = d->limit;
    if (intervalMs < 10) intervalMs = 10;

    BOOL finishedLimit = FALSE;
    while (d->running) {
        if (useFixedPos) {
            SetCursorPos(fixedX, fixedY);
        }

        INPUT inp[2];
        memset(inp, 0, sizeof(inp));
        inp[0].type = INPUT_MOUSE;
        inp[1].type = INPUT_MOUSE;
        inp[0].mi.dwExtraInfo = KSC_INJECTED_MAGIC;
        inp[1].mi.dwExtraInfo = KSC_INJECTED_MAGIC;
        if (buttonType == 1) {
            inp[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            inp[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        } else if (buttonType == 2) {
            inp[0].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
            inp[1].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        } else {
            inp[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inp[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        }
        SendInput(2, inp, sizeof(INPUT));
        if (doubleClick && d->running) {
            Sleep(30);
            if (d->running) SendInput(2, inp, sizeof(INPUT));
        }

        d->clickedSoFar++;
        if (!continuous && d->clickedSoFar >= limit) {
            d->running = FALSE;
            finishedLimit = TRUE;
            break;
        }

        int delay = intervalMs;
        if (offsetMs > 0) {
            int r = (rand() % (2 * offsetMs + 1)) - offsetMs;
            delay += r;
        }
        if (delay < 10) delay = 10;
        /* sleep in chunks for responsive stop */
        while (delay > 0 && d->running) {
            int chunk = delay > 50 ? 50 : delay;
            Sleep(chunk);
            delay -= chunk;
        }
    }

    if (finishedLimit && d->hDlg && IsWindow(d->hDlg))
        PostMessage(d->hDlg, WM_CLICKER_CMD, 2, 0);

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
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
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
        d->hDlg = hWnd;
        d->hotkeyStartId = HOTKEY_ID_START_CLICK;
        d->hotkeyStopId  = HOTKEY_ID_STOP_CLICK;
        {
            CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
            d->hMainWnd = cs->hwndParent;
        }

        int y, xLabel = 10, dy = 24, h = 22;
        (void)xLabel;

        /* Row: Click Interval */
        y = 10;
        CreateWindow(WC_STATIC, "Click Interval:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        y += dy;
        d->hMinEdit = CreateWindow(WC_EDIT, "0",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        15, y, 50, h, hWnd,
                        (HMENU)IDC_CLICK_INTERVAL_MIN, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "m",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     70, y + 2, 12, h, hWnd, nullptr, g_hInst, nullptr);
        d->hSecEdit = CreateWindow(WC_EDIT, "1",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        90, y, 50, h, hWnd,
                        (HMENU)IDC_CLICK_INTERVAL_SEC, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "s",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     145, y + 2, 12, h, hWnd, nullptr, g_hInst, nullptr);
        d->hMsEdit = CreateWindow(WC_EDIT, "0",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        165, y, 50, h, hWnd,
                        (HMENU)IDC_CLICK_INTERVAL_MS, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "ms",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     220, y + 2, 20, h, hWnd, nullptr, g_hInst, nullptr);

        /* Row: Random Offset */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Random Offset:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        y += dy;
        CreateWindow(WC_STATIC, " -+",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     18, y + 2, 22, h, hWnd, nullptr, g_hInst, nullptr);
        d->hOffsetEdit = CreateWindow(WC_EDIT, "0",
                           WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                           42, y, 55, h, hWnd,
                           (HMENU)IDC_CLICK_RANDOM_OFFSET, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "ms",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     102, y + 2, 20, h, hWnd, nullptr, g_hInst, nullptr);

        /* Row: Mouse Button */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Mouse Button:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 110, h, hWnd, nullptr, g_hInst, nullptr);
        y += dy;
        d->hBtnLeft = CreateWindow(WC_BUTTON, "Left",
                        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                        25, y, 65, h, hWnd,
                        (HMENU)IDC_CLICK_BTN_LEFT, g_hInst, nullptr);
        d->hBtnRight = CreateWindow(WC_BUTTON, "Right",
                         WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                         100, y, 70, h, hWnd,
                         (HMENU)IDC_CLICK_BTN_RIGHT, g_hInst, nullptr);
        d->hBtnMiddle = CreateWindow(WC_BUTTON, "Middle",
                          WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                          180, y, 75, h, hWnd,
                          (HMENU)IDC_CLICK_BTN_MIDDLE, g_hInst, nullptr);
        d->hDoubleChk = CreateWindow(WC_BUTTON, "Double click",
                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                          270, y, 120, h, hWnd,
                          (HMENU)IDC_CLICK_DOUBLE_CHK, g_hInst, nullptr);

        /* Row: Mode */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Mode:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 60, h, hWnd, nullptr, g_hInst, nullptr);
        y += dy;
        d->hContinuous = CreateWindow(WC_BUTTON, "Continuous",
                           WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON |
                           WS_GROUP,
                           25, y, 130, h, hWnd,
                           (HMENU)IDC_CLICK_MODE_CONT, g_hInst, nullptr);
        y += dy;
        d->hLimited = CreateWindow(WC_BUTTON, "Limited:",
                       WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                       25, y, 80, h, hWnd,
                       (HMENU)IDC_CLICK_MODE_LIMITED, g_hInst, nullptr);
        d->hLimitedCount = CreateWindow(WC_EDIT, "50",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                              115, y, 55, h, hWnd,
                              (HMENU)IDC_CLICK_LIMITED_COUNT, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "clicks",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     175, y + 2, 50, h, hWnd, nullptr, g_hInst, nullptr);

        /* Row: Position */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Click Position:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 110, h, hWnd, nullptr, g_hInst, nullptr);
        y += dy;
        d->hPosCur = CreateWindow(WC_BUTTON, "Cursor",
                       WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                       25, y, 70, h, hWnd,
                       (HMENU)IDC_CLICK_POS_CUR, g_hInst, nullptr);
        d->hPosFixed = CreateWindow(WC_BUTTON, "Fixed:",
                         WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                         100, y, 62, h, hWnd,
                         (HMENU)IDC_CLICK_POS_FIXED, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "X:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     165, y + 2, 16, h, hWnd, nullptr, g_hInst, nullptr);
        d->hPosX = CreateWindow(WC_EDIT, "0",
                     WS_CHILD | WS_VISIBLE | WS_BORDER,
                     182, y, 52, h, hWnd,
                     (HMENU)IDC_CLICK_POS_X, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "Y:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     240, y + 2, 16, h, hWnd, nullptr, g_hInst, nullptr);
        d->hPosY = CreateWindow(WC_EDIT, "0",
                     WS_CHILD | WS_VISIBLE | WS_BORDER,
                     257, y, 52, h, hWnd,
                     (HMENU)IDC_CLICK_POS_Y, g_hInst, nullptr);
        d->hBtnPick = CreateWindow(WC_BUTTON, "Pick (3s)",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        318, y, 80, h, hWnd,
                        (HMENU)IDC_CLICK_BTN_PICK, g_hInst, nullptr);

        /* Row: Start Shortcut */
        y += dy + 8;
        CreateWindow(WC_STATIC, "Start Shortcut:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        y += dy;
        d->hShortStart = CreateWindow(WC_STATIC, "Ctrl+Shift+S",
                           WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                           120, y, 170, h, hWnd,
                           (HMENU)IDC_CLICK_SHORT_START, g_hInst, nullptr);
        d->hBtnSetStart = CreateWindow(WC_BUTTON, "Set",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            300, y, 55, h, hWnd,
                            (HMENU)IDC_CLICK_BTN_SET_START, g_hInst, nullptr);

        /* Row: Stop Shortcut */
        y += dy + 6;
        CreateWindow(WC_STATIC, "Stop Shortcut:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     xLabel, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        y += dy;
        d->hShortStop = CreateWindow(WC_STATIC, "Ctrl+Shift+X",
                          WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                          120, y, 170, h, hWnd,
                          (HMENU)IDC_CLICK_SHORT_STOP, g_hInst, nullptr);
        d->hBtnSetStop = CreateWindow(WC_BUTTON, "Set",
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           300, y, 55, h, hWnd,
                           (HMENU)IDC_CLICK_BTN_SET_STOP, g_hInst, nullptr);

        /* Status text */
        y += dy + 8;
        d->hStatus = CreateWindow(WC_STATIC, "Status: Idle",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       10, y, 400, h, hWnd,
                       (HMENU)IDC_CLICK_STATUS, g_hInst, nullptr);

        /* Start / Stop buttons */
        y += dy + 4;
        d->hBtnStart = CreateWindow(WC_BUTTON, "Start",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        110, y, 85, 26, hWnd,
                        (HMENU)IDC_CLICK_BTN_START, g_hInst, nullptr);
        d->hBtnStop = CreateWindow(WC_BUTTON, "Stop",
                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                       215, y, 85, 26, hWnd,
                       (HMENU)IDC_CLICK_BTN_STOP, g_hInst, nullptr);

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
        int btnType = db_get_setting_int("clicker_button_type", -1);
        if (btnType < 0)
            btnType = db_get_setting_int("clicker_left_button", 1) ? 0 : 1;
        if (btnType == 1)
            SendMessage(d->hBtnRight, BM_SETCHECK, BST_CHECKED, 0);
        else if (btnType == 2)
            SendMessage(d->hBtnMiddle, BM_SETCHECK, BST_CHECKED, 0);
        else
            SendMessage(d->hBtnLeft, BM_SETCHECK, BST_CHECKED, 0);

        if (db_get_setting_int("clicker_double_click", 0))
            SendMessage(d->hDoubleChk, BM_SETCHECK, BST_CHECKED, 0);

        if (db_get_setting_int("clicker_continuous", 1)) {
            SendMessage(d->hContinuous, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(d->hLimitedCount, FALSE);
        } else {
            SendMessage(d->hLimited, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(d->hLimitedCount, TRUE);
        }
        SetDlgItemInt(hWnd, IDC_CLICK_LIMITED_COUNT,
                      db_get_setting_int("clicker_limited_count", 50), FALSE);

        BOOL fixedPos = db_get_setting_int("clicker_use_fixed_pos", 0) != 0;
        if (fixedPos) {
            SendMessage(d->hPosFixed, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(d->hPosX, TRUE);
            EnableWindow(d->hPosY, TRUE);
        } else {
            SendMessage(d->hPosCur, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(d->hPosX, FALSE);
            EnableWindow(d->hPosY, FALSE);
        }
        SetDlgItemInt(hWnd, IDC_CLICK_POS_X,
                      (UINT)db_get_setting_int("clicker_pos_x", 0), TRUE);
        SetDlgItemInt(hWnd, IDC_CLICK_POS_Y,
                      (UINT)db_get_setting_int("clicker_pos_y", 0), TRUE);

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

        apply_clicker_dark(hWnd);
        return 0;
    }

    case WM_ERASEBKGND:
        if (handle_dark_erasebkgnd(hWnd, (HDC)wParam)) return TRUE;
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return handle_dark_ctlcolor(hWnd, msg, wParam, lParam);

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
            if (mod == 0 || vk == VK_ESCAPE) {
                char buf[64];
                if (d->capturing == 1) {
                    int sc = db_get_setting_int("clicker_start_shortcut",
                        (MOD_CONTROL | MOD_SHIFT) << 16 | 'S');
                    format_shortcut(sc, buf, sizeof(buf));
                    SetWindowText(d->hShortStart, buf);
                } else {
                    int sc = db_get_setting_int("clicker_stop_shortcut",
                        (MOD_CONTROL | MOD_SHIFT) << 16 | 'X');
                    format_shortcut(sc, buf, sizeof(buf));
                    SetWindowText(d->hShortStop, buf);
                }
                d->capturing = 0;
                return 0;
            }

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
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 1000);
                CloseHandle(d->hThread);
                d->hThread = nullptr;
            }
            int intervalMs = GetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_MIN,
                                           nullptr, FALSE) * 60000
                           + GetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_SEC,
                                           nullptr, FALSE) * 1000
                           + GetDlgItemInt(hWnd, IDC_CLICK_INTERVAL_MS,
                                           nullptr, FALSE);
            if (intervalMs < 10) intervalMs = 10;
            d->intervalMs = intervalMs;
            d->offsetMs = GetDlgItemInt(hWnd, IDC_CLICK_RANDOM_OFFSET,
                                        nullptr, FALSE);
            if (SendMessage(d->hBtnRight, BM_GETCHECK, 0, 0) == BST_CHECKED)
                d->buttonType = 1;
            else if (SendMessage(d->hBtnMiddle, BM_GETCHECK, 0, 0) == BST_CHECKED)
                d->buttonType = 2;
            else
                d->buttonType = 0;
            d->doubleClick = (SendMessage(d->hDoubleChk, BM_GETCHECK, 0, 0)
                              == BST_CHECKED);
            d->useFixedPos = (SendMessage(d->hPosFixed, BM_GETCHECK, 0, 0)
                              == BST_CHECKED);
            d->fixedX = (int)GetDlgItemInt(hWnd, IDC_CLICK_POS_X, nullptr, TRUE);
            d->fixedY = (int)GetDlgItemInt(hWnd, IDC_CLICK_POS_Y, nullptr, TRUE);
            d->continuous = (SendMessage(d->hContinuous, BM_GETCHECK, 0, 0)
                             == BST_CHECKED);
            d->limit = GetDlgItemInt(hWnd, IDC_CLICK_LIMITED_COUNT,
                                     nullptr, FALSE);
            d->running = TRUE;
            d->hThread = CreateThread(nullptr, 0, ClickerThreadProc,
                                       d, 0, nullptr);
            SetWindowText(d->hStatus, "Status: Running...");
        } else if (cmd == 1 && d->running) {
            /* Stop */
            d->running = FALSE;
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
                d->hThread = nullptr;
            }
            char buf[64];
            sprintf(buf, "Status: Stopped (%d clicks)", d->clickedSoFar);
            SetWindowText(d->hStatus, buf);
        } else if (cmd == 2) {
            /* Thread finished */
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
                d->hThread = nullptr;
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
                save_clicker_settings(hWnd, d);
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
            if (id == IDC_CLICK_POS_CUR) {
                EnableWindow(d->hPosX, FALSE);
                EnableWindow(d->hPosY, FALSE);
                return 0;
            }
            if (id == IDC_CLICK_POS_FIXED) {
                EnableWindow(d->hPosX, TRUE);
                EnableWindow(d->hPosY, TRUE);
                return 0;
            }
            if (id == IDC_CLICK_BTN_PICK) {
                d->pickCountdown = 3;
                SetWindowText(d->hBtnPick, "Pick (3)...");
                SetTimer(hWnd, ID_TIMER_REFRESH, 1000, nullptr);
                return 0;
            }
        }
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) {
            ClickerData *d = (ClickerData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (!d) return 0;
            d->pickCountdown--;
            if (d->pickCountdown > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "Pick (%d)...", d->pickCountdown);
                SetWindowText(d->hBtnPick, buf);
            } else {
                KillTimer(hWnd, ID_TIMER_REFRESH);
                POINT pt = {};
                GetCursorPos(&pt);
                SetDlgItemInt(hWnd, IDC_CLICK_POS_X, (UINT)pt.x, TRUE);
                SetDlgItemInt(hWnd, IDC_CLICK_POS_Y, (UINT)pt.y, TRUE);
                SendMessage(d->hPosCur, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessage(d->hPosFixed, BM_SETCHECK, BST_CHECKED, 0);
                EnableWindow(d->hPosX, TRUE);
                EnableWindow(d->hPosY, TRUE);
                SetWindowText(d->hBtnPick, "Pick (3s)");
                char st[64];
                snprintf(st, sizeof(st), "Status: Picked (%ld, %ld)",
                         pt.x, pt.y);
                SetWindowText(d->hStatus, st);
            }
            return 0;
        }
        break;

    case WM_THEME_CHANGED:
        handle_theme_changed(hWnd);
        return 0;

    case WM_CLOSE: {
        ClickerData *d = (ClickerData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        save_clicker_settings(hWnd, d);
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }

    case WM_DESTROY: {
        KillTimer(hWnd, ID_TIMER_REFRESH);
        ClickerData *d = (ClickerData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d) {
            d->running = FALSE;
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
            }
            free(d);
        }
        g_hClickerWnd = nullptr;
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

typedef struct {
    HWND hStatus;
    HWND hLoginBtn;
    HWND hCompressChk;
    HWND hKeepCombo;
    HWND hListView;
    int  loggedIn;
    ListViewSortState sort;
} CloudBakData;

static void refresh_cloud_history(HWND hListView,
                                  const ListViewSortState *sortState)
{
    if (!hListView) return;
    SendMessage(hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hListView);

    CloudSyncEntry *entries = nullptr;
    int count = 0;
    if (!cloudsync_load_history(&entries, &count)) {
        SendMessage(hListView, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hListView, nullptr, TRUE);
        return;
    }

    LVITEM lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT;

    for (int i = 0; i < count; i++) {
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = entries[i].ts;
        ListView_InsertItem(hListView, &lvi);
        ListView_SetItemText(hListView, i, 1, entries[i].files);
        char szbuf[32];
        sprintf(szbuf, "%d KB", entries[i].size / 1024);
        ListView_SetItemText(hListView, i, 2, szbuf);
        ListView_SetItemText(hListView, i, 3, entries[i].status);
    }

    cloudsync_free_history(entries);
    if (sortState)
        listview_apply_sort(hListView, sortState, sortState->sortCol == 2);
    SendMessage(hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hListView, nullptr, TRUE);
}

static LRESULT CALLBACK CloudBackupWndProc(HWND hWnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CloudBakData *d = calloc(1, sizeof(CloudBakData));
        if (!d) return -1;
        d->loggedIn = cloudsync_is_logged_in();

        int y = 10, h = 22;

        d->hStatus = CreateWindow(WC_STATIC, "",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       10, y, 500, h, hWnd,
                       (HMENU)IDC_CLOUD_STATUS, g_hInst, nullptr);
        {
            char buf[256];
            cloudsync_get_email(buf, sizeof(buf));
            char label[320];
            sprintf(label, "Status: %s", buf);
            SetWindowText(d->hStatus, label);
        }

        y += 24;
        d->hLoginBtn = CreateWindow(WC_BUTTON,
                         d->loggedIn ? "Logout" : "Login",
                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         10, y, 80, h, hWnd,
                         (HMENU)IDC_CLOUD_LOGIN_BTN, g_hInst, nullptr);

        y += 34;
        CreateWindow(WC_STATIC, "Schedule:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 60, h, hWnd, nullptr, g_hInst, nullptr);
        y += 24;

        int sched = cloudsync_get_schedule();
        int xp = 20;
        CreateWindow(WC_BUTTON, "Off",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                     xp, y, 50, h, hWnd,
                     (HMENU)IDC_CLOUD_SCHED_OFF, g_hInst, nullptr);
        if (sched == 0) SendMessage(GetDlgItem(hWnd, IDC_CLOUD_SCHED_OFF),
                                     BM_SETCHECK, BST_CHECKED, 0);
        xp += 58;
        CreateWindow(WC_BUTTON, "5 min",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                     xp, y, 62, h, hWnd,
                     (HMENU)IDC_CLOUD_SCHED_5M, g_hInst, nullptr);
        if (sched == 1) SendMessage(GetDlgItem(hWnd, IDC_CLOUD_SCHED_5M),
                                     BM_SETCHECK, BST_CHECKED, 0);
        xp += 70;
        CreateWindow(WC_BUTTON, "15 min",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                     xp, y, 65, h, hWnd,
                     (HMENU)IDC_CLOUD_SCHED_15M, g_hInst, nullptr);
        if (sched == 2) SendMessage(GetDlgItem(hWnd, IDC_CLOUD_SCHED_15M),
                                     BM_SETCHECK, BST_CHECKED, 0);
        xp += 73;
        CreateWindow(WC_BUTTON, "30 min",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                     xp, y, 65, h, hWnd,
                     (HMENU)IDC_CLOUD_SCHED_30M, g_hInst, nullptr);
        if (sched == 3) SendMessage(GetDlgItem(hWnd, IDC_CLOUD_SCHED_30M),
                                     BM_SETCHECK, BST_CHECKED, 0);
        xp += 73;
        CreateWindow(WC_BUTTON, "1 hr",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                     xp, y, 55, h, hWnd,
                     (HMENU)IDC_CLOUD_SCHED_1H, g_hInst, nullptr);
        if (sched == 4) SendMessage(GetDlgItem(hWnd, IDC_CLOUD_SCHED_1H),
                                     BM_SETCHECK, BST_CHECKED, 0);
        xp += 63;
        CreateWindow(WC_BUTTON, "12 hr",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                     xp, y, 58, h, hWnd,
                     (HMENU)IDC_CLOUD_SCHED_12H, g_hInst, nullptr);
        if (sched == 5) SendMessage(GetDlgItem(hWnd, IDC_CLOUD_SCHED_12H),
                                     BM_SETCHECK, BST_CHECKED, 0);
        xp += 66;
        CreateWindow(WC_BUTTON, "Daily",
                     WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                     xp, y, 55, h, hWnd,
                     (HMENU)IDC_CLOUD_SCHED_DAILY, g_hInst, nullptr);
        if (sched == 6) SendMessage(GetDlgItem(hWnd, IDC_CLOUD_SCHED_DAILY),
                                     BM_SETCHECK, BST_CHECKED, 0);

        y += 32;
        CreateWindow(WC_BUTTON, "Backup Now",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     10, y, 100, 26, hWnd,
                     (HMENU)IDC_CLOUD_BACKUP_NOW, g_hInst, nullptr);
        d->hCompressChk = CreateWindow("BUTTON",
                     "Compress (.zip) before upload",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     118, y + 2, 225, 22, hWnd,
                     (HMENU)IDC_CLOUD_COMPRESS_CHK, g_hInst, nullptr);
        if (cloudsync_get_compress())
            SendMessage(d->hCompressChk, BM_SETCHECK, BST_CHECKED, 0);

        CreateWindow(WC_STATIC, "Keep last:",
                     WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                     350, y + 2, 66, 22, hWnd, nullptr, g_hInst, nullptr);
        d->hKeepCombo = CreateWindow(WC_COMBOBOX, "",
                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                     418, y + 2, 115, 160, hWnd,
                     (HMENU)IDC_CLOUD_KEEP_COMBO, g_hInst, nullptr);
        SendMessage(d->hKeepCombo, CB_ADDSTRING, 0, (LPARAM)"All backups");
        SendMessage(d->hKeepCombo, CB_ADDSTRING, 0, (LPARAM)"10 backups");
        SendMessage(d->hKeepCombo, CB_ADDSTRING, 0, (LPARAM)"30 backups");
        SendMessage(d->hKeepCombo, CB_ADDSTRING, 0, (LPARAM)"50 backups");
        {
            int kc = cloudsync_get_keep_count();
            int selIdx = (kc == 10) ? 1 : (kc == 30) ? 2 : (kc == 50) ? 3 : 0;
            SendMessage(d->hKeepCombo, CB_SETCURSEL, selIdx, 0);
        }

        y += 36;
        CreateWindow(WC_STATIC, "\x1a\x1a\x1a SSH Target \x1a\x1a\x1a",
                     WS_CHILD | WS_VISIBLE | SS_CENTER,
                     10, y, 620, h, hWnd, nullptr, g_hInst, nullptr);
        y += 26;
        {
            CreateWindow(WC_STATIC, "Host:",
                         WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                         10, y, 40, h, hWnd, nullptr, g_hInst, nullptr);
            CreateWindow(WC_EDIT, "",
                         WS_CHILD | WS_VISIBLE | WS_BORDER,
                         52, y, 180, h, hWnd,
                         (HMENU)IDC_SSH_HOST, g_hInst, nullptr);
            CreateWindow(WC_STATIC, "Port:",
                         WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                         242, y, 32, h, hWnd, nullptr, g_hInst, nullptr);
            CreateWindow(WC_EDIT, "22",
                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                         278, y, 55, h, hWnd,
                         (HMENU)IDC_SSH_PORT, g_hInst, nullptr);
        }
        y += 26;
        {
            CreateWindow(WC_STATIC, "User:",
                         WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                         10, y, 40, h, hWnd, nullptr, g_hInst, nullptr);
            CreateWindow(WC_EDIT, "",
                         WS_CHILD | WS_VISIBLE | WS_BORDER,
                         52, y, 140, h, hWnd,
                         (HMENU)IDC_SSH_USER, g_hInst, nullptr);
            CreateWindow(WC_STATIC, "Pass:",
                         WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                         202, y, 36, h, hWnd, nullptr, g_hInst, nullptr);
            CreateWindow(WC_EDIT, "",
                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD,
                         242, y, 140, h, hWnd,
                         (HMENU)IDC_SSH_PASS, g_hInst, nullptr);
        }
        y += 30;
        CreateWindow(WC_BUTTON, "Test Connection",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     10, y, 110, h, hWnd,
                     (HMENU)IDC_SSH_TEST, g_hInst, nullptr);
        CreateWindow(WC_BUTTON, "Save SSH Config",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     130, y, 120, h, hWnd,
                     (HMENU)IDC_SSH_SAVE, g_hInst, nullptr);

        ssh_sync_load_config(
            GetDlgItem(hWnd, IDC_SSH_HOST),
            GetDlgItem(hWnd, IDC_SSH_PORT),
            GetDlgItem(hWnd, IDC_SSH_USER),
            GetDlgItem(hWnd, IDC_SSH_PASS));

        y += 34;
        CreateWindow(WC_STATIC, "Sync History:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 120, h, hWnd, nullptr, g_hInst, nullptr);
        y += 20;

        HWND hLv = CreateWindow(WC_LISTVIEW, "",
                    WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                    0, y, 10, 10,
                    hWnd, (HMENU)IDC_CLOUD_LISTVIEW, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(hLv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMN lvc;
        memset(&lvc, 0, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = 170; lvc.pszText = "Timestamp"; ListView_InsertColumn(hLv, 0, &lvc);
        lvc.cx = 220; lvc.pszText = "Files";     ListView_InsertColumn(hLv, 1, &lvc);
        lvc.cx = 70;  lvc.pszText = "Size";      ListView_InsertColumn(hLv, 2, &lvc);
        lvc.cx = 70;  lvc.pszText = "Status";    ListView_InsertColumn(hLv, 3, &lvc);

        d->hListView = hLv;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)d);

        {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(hLv, nullptr, 0, y, rc.right,
                         rc.bottom - y, SWP_NOZORDER);
        }

        apply_listview_theme(hWnd, hLv);
        refresh_cloud_history(hLv, &d->sort);
        return 0;
    }

    case WM_SIZE: {
        CloudBakData *d = (CloudBakData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d && d->hListView) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            int y = 296;
            SetWindowPos(d->hListView, nullptr, 0, y,
                         rc.right, rc.bottom - y, SWP_NOZORDER);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        if (handle_dark_erasebkgnd(hWnd, (HDC)wParam)) return TRUE;
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return handle_dark_ctlcolor(hWnd, msg, wParam, lParam);

    case WM_NOTIFY: {
        LRESULT cdRes = 0;
        if (handle_dark_lv_customdraw(IDC_CLOUD_LISTVIEW, lParam, &cdRes))
            return cdRes;
        NMHDR *nmh = (NMHDR *)lParam;
        if (nmh->idFrom == IDC_CLOUD_LISTVIEW && nmh->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *pnm = (NMLISTVIEW *)lParam;
            CloudBakData *d = (CloudBakData *)GetWindowLongPtr(
                hWnd, GWLP_USERDATA);
            if (d) {
                listview_toggle_sort(d->hListView, &d->sort, pnm->iSubItem,
                                     pnm->iSubItem == 2);
            }
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        CloudBakData *d = (CloudBakData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (!d) break;
        WORD id = LOWORD(wParam);

        if (id == IDC_CLOUD_LOGIN_BTN &&
            HIWORD(wParam) == BN_CLICKED) {
            if (d->loggedIn) {
                cloudsync_logout();
                d->loggedIn = 0;
                SetWindowText(d->hStatus,
                    "Status: Not logged in");
                SetWindowText(d->hLoginBtn, "Login");
            } else {
                EnableWindow(d->hLoginBtn, FALSE);
                SetWindowText(d->hStatus,
                    "Status: Opening browser...");
                ShowWindow(hWnd, SW_MINIMIZE);
                cloudsync_login(hWnd);
                if (!IsWindow(hWnd)) return 0;
                EnableWindow(d->hLoginBtn, TRUE);
                ShowWindow(hWnd, SW_RESTORE);
                d->loggedIn = cloudsync_is_logged_in();
                char buf[256];
                cloudsync_get_email(buf, sizeof(buf));
                char label[320];
                sprintf(label, "Status: %s", buf);
                SetWindowText(d->hStatus, label);
                SetWindowText(d->hLoginBtn,
                    d->loggedIn ? "Logout" : "Login");
            }
            return 0;
        }

        if (id == IDC_CLOUD_COMPRESS_CHK &&
            HIWORD(wParam) == BN_CLICKED) {
            int chk = (SendMessage(d->hCompressChk, BM_GETCHECK, 0, 0)
                       == BST_CHECKED) ? 1 : 0;
            cloudsync_set_compress(chk);
            return 0;
        }

        if (id == IDC_CLOUD_KEEP_COMBO &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessage(d->hKeepCombo, CB_GETCURSEL, 0, 0);
            int vals[] = {0, 10, 30, 50};
            cloudsync_set_keep_count((sel >= 0 && sel <= 3) ? vals[sel] : 0);
            return 0;
        }

        if (id >= IDC_CLOUD_SCHED_OFF &&
            id <= IDC_CLOUD_SCHED_DAILY &&
            HIWORD(wParam) == BN_CLICKED) {
            int val = id - IDC_CLOUD_SCHED_OFF;
            cloudsync_set_schedule(val);

            /* restart cloud timer in main window */
            HWND hMain = GetWindow(hWnd, GW_OWNER);
            if (hMain) {
                KillTimer(hMain, ID_TIMER_CLOUD_SYNC);
                DWORD intervals[] = {
                    0, 300000, 900000, 1800000, 3600000, 43200000, 86400000
                };
                if (val > 0 && val <= 6)
                    SetTimer(hMain, ID_TIMER_CLOUD_SYNC,
                             intervals[val], nullptr);
            }
            return 0;
        }

        if (id == IDC_SSH_TEST &&
            HIWORD(wParam) == BN_CLICKED) {
            ssh_sync_save_config(
                GetDlgItem(hWnd, IDC_SSH_HOST),
                GetDlgItem(hWnd, IDC_SSH_PORT),
                GetDlgItem(hWnd, IDC_SSH_USER),
                GetDlgItem(hWnd, IDC_SSH_PASS));
            ssh_sync_test(hWnd);
            return 0;
        }

        if (id == IDC_SSH_SAVE &&
            HIWORD(wParam) == BN_CLICKED) {
            ssh_sync_save_config(
                GetDlgItem(hWnd, IDC_SSH_HOST),
                GetDlgItem(hWnd, IDC_SSH_PORT),
                GetDlgItem(hWnd, IDC_SSH_USER),
                GetDlgItem(hWnd, IDC_SSH_PASS));
            return 0;
        }

        if (id == IDC_CLOUD_BACKUP_NOW &&
            HIWORD(wParam) == BN_CLICKED) {
            if (!d->loggedIn && !ssh_sync_is_configured()) {
                MessageBox(hWnd,
                    "No backup target configured.\n"
                    "Login to Google Drive or set up SSH.",
                    "Cloud Backup", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            SetWindowText(d->hStatus,
                "Status: Backing up...");
            cloudsync_backup_trigger(hWnd);
            return 0;
        }
        break;
    }

    case WM_CLOUD_SYNC: {
        CloudBakData *d = (CloudBakData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d) {
            if (wParam >= 100) {
                char buf[256];
                cloudsync_get_email(buf, sizeof(buf));
                const char *suffix = "sync complete";
                if (lParam < 0) suffix = "nothing to back up";
                else if (lParam == 0) suffix = "sync failed";
                char label[320];
                sprintf(label, "Status: %s (%s)", buf, suffix);
                SetWindowText(d->hStatus, label);
                refresh_cloud_history(d->hListView, &d->sort);
            } else {
                char buf[320];
                sprintf(buf,
                    "Status: Uploading... %lld%%",
                    (long long)wParam);
                SetWindowText(d->hStatus, buf);
            }
        }
        return 0;
    }

    case WM_THEME_CHANGED: {
        CloudBakData *d = (CloudBakData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        apply_listview_theme(hWnd, d ? d->hListView : nullptr);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        CloudBakData *d = (CloudBakData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d) free(d);
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_cloud_backup(HWND hParent)
{
    if (focus_existing_window("KSC_CloudBackup")) return;
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = CloudBackupWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_CloudBackup";
        RegisterClass(&wc);
        registered = TRUE;
    }
    HWND hDlg = CreateWindow("KSC_CloudBackup", "ksc - Cloud Backup",
                 WS_OVERLAPPEDWINDOW,
                 CW_USEDEFAULT, CW_USEDEFAULT, 640, 680,
                 hParent, nullptr, g_hInst, nullptr);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static void show_mouse_clicker(HWND hParent)
{
    if (g_hClickerWnd && IsWindow(g_hClickerWnd)) {
        if (!IsWindowVisible(g_hClickerWnd))
            ShowWindow(g_hClickerWnd, SW_RESTORE);
        SetForegroundWindow(g_hClickerWnd);
        return;
    }
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = MouseClickerWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_MouseClicker";
        RegisterClass(&wc);
        registered = TRUE;
    }
    HWND hDlg = CreateWindow("KSC_MouseClicker", "ksc - Mouse Clicker",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                 CW_USEDEFAULT, CW_USEDEFAULT, 440, 540,
                 hParent, nullptr, g_hInst, nullptr);
    g_hClickerWnd = hDlg;
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

typedef struct {
    HWND hDlg;
    HWND hSeqLbl;
    HWND hMinEdit, hSecEdit, hMsEdit;
    HWND hOffsetEdit;
    HWND hContinuous, hLimited;
    HWND hLimitedCount;
    HWND hShortStart, hShortStop;
    HWND hBtnSetStart, hBtnSetStop;
    HWND hBtnStart, hBtnStop;
    HWND hStatus;
    HWND hMainWnd;
    HWND hRecordBtn;
    HWND hReadableLbl;
    volatile BOOL running;
    HANDLE hThread;
    int cyclesSoFar;
    int capturing;
    int keyCount;
    int hotkeyStartId;
    int hotkeyStopId;
    /* cached settings for the thread (no UI reads from thread) */
    char cachedSeq[2048];
    int  intervalMs;
    int  offsetMs;
    int  isContinuous;
    int  limitCount;
} KbSimData;

/* manual strtok replacement for thread safety */
static char *parse_next(char *str, char **ctx)
{
    if (str) *ctx = str;
    if (!*ctx || !**ctx) return nullptr;
    while (**ctx == ' ') (*ctx)++;
    char *start = *ctx;
    while (**ctx && **ctx != ',') (*ctx)++;
    if (**ctx == ',') { **ctx = '\0'; (*ctx)++; }
    return start;
}

static void update_readable_label(KbSimData *d)
{
    char hexText[2048], readable[2048] = "";
    GetWindowText(d->hSeqLbl, hexText, sizeof(hexText));
    if (hexText[0] == '\0') {
        SetWindowText(d->hReadableLbl, "");
        d->keyCount = 0;
        return;
    }
    char *ctx = nullptr;
    char *tok = parse_next(hexText, &ctx);
    int cnt = 0;
    while (tok && cnt < 128) {
        while (*tok == ' ') tok++;
        if (strncmp(tok, "0x", 2) == 0 || strncmp(tok, "0X", 2) == 0) {
            int packed = (int)strtol(tok, nullptr, 16);
            char tmp[64];
            format_shortcut(packed, tmp, sizeof(tmp));
            size_t need = (cnt > 0 ? 2 : 0) + strlen(tmp) + 1;
            if (strlen(readable) + need >= sizeof(readable)) break;
            if (cnt > 0) strcat(readable, ", ");
            strcat(readable, tmp);
            cnt++;
        }
        tok = parse_next(nullptr, &ctx);
    }
    d->keyCount = cnt;
    SetWindowText(d->hReadableLbl, readable);
}

static void register_kbsim_hotkey(HWND hMain, int id, int shortcut)
{
    if (shortcut <= 0) return;
    int vk = shortcut & 0xFFFF;
    int mod = (shortcut >> 16) & 0xFFFF;
    RegisterHotKey(hMain, id, mod | MOD_NOREPEAT, vk);
}

static void unregister_kbsim_hotkey(HWND hMain, int id)
{
    UnregisterHotKey(hMain, id);
}

static bool is_extended_vk(int vk)
{
    switch (vk) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_DIVIDE:
    case VK_NUMLOCK:
    case VK_SNAPSHOT:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
        return true;
    default:
        return false;
    }
}

static DWORD WINAPI KbSimThreadProc(LPVOID param)
{
    KbSimData *d = (KbSimData *)param;
    srand(GetTickCount());
    d->cyclesSoFar = 0;

    /* copy cached values locally so the struct can be reused */
    int intervalMs = d->intervalMs;
    int offsetMs   = d->offsetMs;
    int continuous = d->isContinuous;
    int limit      = d->limitCount;
    int keys[128];
    int nKeys = 0;

    /* parse cached hex sequence without strtok */
    {
        char buf[2048], *ctx = nullptr;
        strcpy(buf, d->cachedSeq);
        char *tok = parse_next(buf, &ctx);
        while (tok && nKeys < 128) {
            while (*tok == ' ') tok++;
            if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
                keys[nKeys++] = (int)strtol(tok, nullptr, 16);
            tok = parse_next(nullptr, &ctx);
        }
    }

    if (nKeys == 0 || intervalMs < 10) {
        d->running = FALSE;
        if (d->hDlg && IsWindow(d->hDlg))
            PostMessage(d->hDlg, WM_KBSIM_CMD, 2, 0);
        return 0;
    }

    BOOL finishedCleanly = FALSE;
    while (d->running) {
        for (int i = 0; i < nKeys; i++) {
            if (!d->running) break;

            int packed = keys[i];
            int vk = packed & 0xFFFF;
            int mod = (packed >> 16) & 0xFFFF;

            /* modifiers down */
            INPUT modDown[4];
            int nM = 0;
            memset(modDown, 0, sizeof(modDown));
            if (mod & MOD_CONTROL) { modDown[nM].type = INPUT_KEYBOARD; modDown[nM].ki.wVk = VK_CONTROL; modDown[nM].ki.dwExtraInfo = KSC_INJECTED_MAGIC; nM++; }
            if (mod & MOD_SHIFT)   { modDown[nM].type = INPUT_KEYBOARD; modDown[nM].ki.wVk = VK_SHIFT;   modDown[nM].ki.dwExtraInfo = KSC_INJECTED_MAGIC; nM++; }
            if (mod & MOD_ALT)     { modDown[nM].type = INPUT_KEYBOARD; modDown[nM].ki.wVk = VK_MENU;    modDown[nM].ki.dwExtraInfo = KSC_INJECTED_MAGIC; nM++; }
            if (mod & MOD_WIN)     { modDown[nM].type = INPUT_KEYBOARD; modDown[nM].ki.wVk = VK_LWIN;    modDown[nM].ki.dwFlags = KEYEVENTF_EXTENDEDKEY; modDown[nM].ki.dwExtraInfo = KSC_INJECTED_MAGIC; nM++; }
            if (nM > 0) SendInput(nM, modDown, sizeof(INPUT));

            if (!d->running) { /* release mods and exit */
                for (int j = 0; j < nM; j++) {
                    modDown[j].ki.dwFlags |= KEYEVENTF_KEYUP;
                }
                if (nM > 0) SendInput(nM, modDown, sizeof(INPUT));
                break;
            }

            /* key-down */
            DWORD extFlag = is_extended_vk(vk) ? KEYEVENTF_EXTENDEDKEY : 0;
            INPUT kd;
            memset(&kd, 0, sizeof(kd));
            kd.type = INPUT_KEYBOARD;
            kd.ki.wVk = (WORD)vk;
            kd.ki.dwFlags = extFlag;
            kd.ki.dwExtraInfo = KSC_INJECTED_MAGIC;
            SendInput(1, &kd, sizeof(INPUT));
            Sleep(10);

            if (!d->running) {
                kd.ki.dwFlags = extFlag | KEYEVENTF_KEYUP;
                SendInput(1, &kd, sizeof(INPUT));
                for (int j = 0; j < nM; j++) {
                    modDown[j].ki.dwFlags |= KEYEVENTF_KEYUP;
                }
                if (nM > 0) SendInput(nM, modDown, sizeof(INPUT));
                break;
            }

            /* key-up */
            kd.ki.dwFlags = extFlag | KEYEVENTF_KEYUP;
            SendInput(1, &kd, sizeof(INPUT));

            /* release modifiers */
            for (int j = 0; j < nM; j++)
                modDown[j].ki.dwFlags |= KEYEVENTF_KEYUP;
            if (nM > 0) SendInput(nM, modDown, sizeof(INPUT));

            if (i < nKeys - 1) Sleep(10);
            if (!d->running) break;
        }

        if (!d->running) break;

        d->cyclesSoFar++;
        if (!continuous && d->cyclesSoFar >= limit) {
            d->running = FALSE;
            finishedCleanly = TRUE;
            break;
        }

        int delay = intervalMs;
        if (offsetMs > 0) {
            int r = (rand() % (2 * offsetMs + 1)) - offsetMs;
            delay += r;
        }
        if (delay < 5) delay = 5;

        /* sleep in small chunks so stop is detected quickly */
        while (delay > 0 && d->running) {
            int chunk = delay > 50 ? 50 : delay;
            Sleep(chunk);
            delay -= chunk;
        }
    }

    if (finishedCleanly && d->hDlg && IsWindow(d->hDlg))
        PostMessage(d->hDlg, WM_KBSIM_CMD, 2, 0);
    return 0;
}

static LRESULT CALLBACK KeyboardSimWndProc(HWND hWnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        KbSimData *d = calloc(1, sizeof(KbSimData));
        if (!d) return -1;
        d->hDlg = hWnd;
        d->hotkeyStartId = HOTKEY_ID_START_KBSIM;
        d->hotkeyStopId  = HOTKEY_ID_STOP_KBSIM;
        {
            CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
            d->hMainWnd = cs->hwndParent;
        }

        int y = 10, h = 22;

        CreateWindow(WC_STATIC, "Key Sequence:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        d->hSeqLbl = CreateWindow(WC_STATIC, "",
                       WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                       115, y, 260, h, hWnd,
                       (HMENU)IDC_KBSIM_SEQ_LBL, g_hInst, nullptr);
        CreateWindow(WC_BUTTON, "Record",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     385, y, 55, h, hWnd,
                     (HMENU)IDC_KBSIM_RECORD, g_hInst, nullptr);
        d->hRecordBtn = GetDlgItem(hWnd, IDC_KBSIM_RECORD);

        y += 24;
        CreateWindow(WC_STATIC, "Recorded keys:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        d->hReadableLbl = CreateWindow(WC_STATIC, "",
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            115, y, 260, h, hWnd,
                            (HMENU)IDC_KBSIM_READABLE, g_hInst, nullptr);
        CreateWindow(WC_BUTTON, "Clear",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     385, y, 55, h, hWnd,
                     (HMENU)IDC_KBSIM_CLEAR, g_hInst, nullptr);

        y += 24;
        CreateWindow(WC_STATIC, "Interval:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 55, h, hWnd, nullptr, g_hInst, nullptr);
        y += 24;
        d->hMinEdit = CreateWindow(WC_EDIT, "0",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        15, y, 50, h, hWnd,
                        (HMENU)IDC_KBSIM_INT_MIN, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "m",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     70, y + 2, 12, h, hWnd, nullptr, g_hInst, nullptr);
        d->hSecEdit = CreateWindow(WC_EDIT, "1",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        90, y, 50, h, hWnd,
                        (HMENU)IDC_KBSIM_INT_SEC, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "s",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     145, y + 2, 12, h, hWnd, nullptr, g_hInst, nullptr);
        d->hMsEdit = CreateWindow(WC_EDIT, "0",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                        165, y, 50, h, hWnd,
                        (HMENU)IDC_KBSIM_INT_MS, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "ms",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     220, y + 2, 20, h, hWnd, nullptr, g_hInst, nullptr);

        y += 28;
        CreateWindow(WC_STATIC, "Random Offset:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        y += 24;
        CreateWindow(WC_STATIC, " -+",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     18, y + 2, 22, h, hWnd, nullptr, g_hInst, nullptr);
        d->hOffsetEdit = CreateWindow(WC_EDIT, "0",
                           WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                           42, y, 55, h, hWnd,
                           (HMENU)IDC_KBSIM_OFFSET, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "ms",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     102, y + 2, 20, h, hWnd, nullptr, g_hInst, nullptr);

        y += 28;
        CreateWindow(WC_STATIC, "Mode:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 60, h, hWnd, nullptr, g_hInst, nullptr);
        y += 24;
        d->hContinuous = CreateWindow(WC_BUTTON, "Continuous",
                           WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON |
                           WS_GROUP,
                           25, y, 130, h, hWnd,
                           (HMENU)IDC_KBSIM_CONT, g_hInst, nullptr);
        y += 24;
        d->hLimited = CreateWindow(WC_BUTTON, "Limited:",
                       WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                       25, y, 80, h, hWnd,
                       (HMENU)IDC_KBSIM_LIMITED, g_hInst, nullptr);
        d->hLimitedCount = CreateWindow(WC_EDIT, "10",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                              115, y, 55, h, hWnd,
                              (HMENU)IDC_KBSIM_LIMIT, g_hInst, nullptr);
        CreateWindow(WC_STATIC, "repeats",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     175, y + 2, 60, h, hWnd, nullptr, g_hInst, nullptr);

        y += 28;
        CreateWindow(WC_STATIC, "Start Shortcut:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        y += 24;
        d->hShortStart = CreateWindow(WC_STATIC, "Ctrl+Shift+J",
                           WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                           120, y, 170, h, hWnd,
                           (HMENU)IDC_KBSIM_SH_START, g_hInst, nullptr);
        d->hBtnSetStart = CreateWindow(WC_BUTTON, "Set",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            300, y, 55, h, hWnd,
                            (HMENU)IDC_KBSIM_SET_START, g_hInst, nullptr);

        y += 26;
        CreateWindow(WC_STATIC, "Stop Shortcut:",
                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                     10, y, 100, h, hWnd, nullptr, g_hInst, nullptr);
        y += 24;
        d->hShortStop = CreateWindow(WC_STATIC, "Ctrl+Shift+M",
                          WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTER,
                          120, y, 170, h, hWnd,
                          (HMENU)IDC_KBSIM_SH_STOP, g_hInst, nullptr);
        d->hBtnSetStop = CreateWindow(WC_BUTTON, "Set",
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           300, y, 55, h, hWnd,
                           (HMENU)IDC_KBSIM_SET_STOP, g_hInst, nullptr);

        y += 32;
        d->hStatus = CreateWindow(WC_STATIC, "Status: Idle",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       10, y, 400, h, hWnd,
                       (HMENU)IDC_KBSIM_STATUS, g_hInst, nullptr);

        y += 24;
        d->hBtnStart = CreateWindow(WC_BUTTON, "Start",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        120, y, 85, 26, hWnd,
                        (HMENU)IDC_KBSIM_START, g_hInst, nullptr);
        d->hBtnStop = CreateWindow(WC_BUTTON, "Stop",
                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                       230, y, 85, 26, hWnd,
                       (HMENU)IDC_KBSIM_STOP, g_hInst, nullptr);

        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)d);

        /* load saved sequence */
        {
            char buf[2048];
            extern void db_get_setting_str(const char *k,
                const char *d, char *o, int s);
            db_get_setting_str("kbsim_sequence", "", buf, sizeof(buf));
            if (buf[0]) SetWindowText(d->hSeqLbl, buf);
            update_readable_label(d);
        }
        /* load saved interval */
        {
            int ims = db_get_setting_int("kbsim_interval_ms", 1000);
            SetDlgItemInt(hWnd, IDC_KBSIM_INT_MIN, ims / 60000, FALSE);
            SetDlgItemInt(hWnd, IDC_KBSIM_INT_SEC,
                          (ims % 60000) / 1000, FALSE);
            SetDlgItemInt(hWnd, IDC_KBSIM_INT_MS, ims % 1000, FALSE);
        }
        SetDlgItemInt(hWnd, IDC_KBSIM_OFFSET,
            db_get_setting_int("kbsim_random_offset", 0), FALSE);
        if (db_get_setting_int("kbsim_continuous", 1)) {
            SendMessage(d->hContinuous, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(d->hLimitedCount, FALSE);
        } else {
            SendMessage(d->hLimited, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(d->hLimitedCount, TRUE);
        }
        SetDlgItemInt(hWnd, IDC_KBSIM_LIMIT,
            db_get_setting_int("kbsim_limited_count", 10), FALSE);

        {
            int ss = db_get_setting_int("kbsim_start_shortcut",
                       (MOD_CONTROL | MOD_SHIFT) << 16 | 'J');
            int st = db_get_setting_int("kbsim_stop_shortcut",
                       (MOD_CONTROL | MOD_SHIFT) << 16 | 'M');
            char buf[64];
            format_shortcut(ss, buf, sizeof(buf));
            SetWindowText(d->hShortStart, buf);
            format_shortcut(st, buf, sizeof(buf));
            SetWindowText(d->hShortStop, buf);
            register_kbsim_hotkey(d->hMainWnd, d->hotkeyStartId, ss);
            register_kbsim_hotkey(d->hMainWnd, d->hotkeyStopId, st);
        }

        /* dark mode */
        {
            BOOL dark = db_get_setting_int("dark_mode", 0);
            if (dark) {
                BOOL useDark = TRUE;
                DwmSetWindowAttribute(hWnd,
                    DWMWA_USE_IMMERSIVE_DARK_MODE,
                    &useDark, sizeof(useDark));
                if (g_pAllowDarkModeForWindow)
                    g_pAllowDarkModeForWindow(hWnd, TRUE);
                SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                    SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
        }
        return 0;
    }

    case WM_ERASEBKGND:
        if (handle_dark_erasebkgnd(hWnd, (HDC)wParam)) return TRUE;
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return handle_dark_ctlcolor(hWnd, msg, wParam, lParam);

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        KbSimData *d = (KbSimData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d && d->capturing) {
            int vk = (int)wParam;
            if (vk == VK_CONTROL || vk == VK_SHIFT ||
                vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN)
                return 0;
            int mod = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
            if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN))
                & 0x8000) mod |= MOD_WIN;
            if (d->capturing != 1 && (mod == 0 || vk == VK_ESCAPE)) {
                char buf[64];
                if (d->capturing == 2) {
                    int ss = db_get_setting_int("kbsim_start_shortcut",
                        (MOD_CONTROL | MOD_SHIFT) << 16 | 'J');
                    format_shortcut(ss, buf, sizeof(buf));
                    SetWindowText(d->hShortStart, buf);
                } else if (d->capturing == 3) {
                    int st = db_get_setting_int("kbsim_stop_shortcut",
                        (MOD_CONTROL | MOD_SHIFT) << 16 | 'M');
                    format_shortcut(st, buf, sizeof(buf));
                    SetWindowText(d->hShortStop, buf);
                }
                d->capturing = 0;
                return 0;
            }

            if (d->capturing == 1) {
                /* Record mode: append to sequence (max 128 keys) */
                int packed = (mod << 16) | vk;
                char hex[16], curText[2048] = "";
                sprintf(hex, "0x%08X", packed);
                GetWindowText(d->hSeqLbl, curText, sizeof(curText));
                if (strlen(curText) + strlen(hex) + 2 < sizeof(curText)) {
                    if (curText[0]) strcat(curText, ",");
                    strcat(curText, hex);
                    SetWindowText(d->hSeqLbl, curText);
                    update_readable_label(d);
                    extern void db_set_setting_str(const char *k,
                        const char *v);
                    db_set_setting_str("kbsim_sequence", curText);
                }
                if (d->keyCount >= 128 ||
                    strlen(curText) + 12 >= sizeof(curText)) {
                    d->capturing = 0;
                    SetWindowText(d->hRecordBtn, "Record");
                }
                return 0;
            } else if (d->capturing == 2) {
                /* Set start shortcut */
                int packed = (mod << 16) | vk;
                char buf[64];
                format_shortcut(packed, buf, sizeof(buf));
                SetWindowText(d->hShortStart, buf);
                db_set_setting_int("kbsim_start_shortcut", packed);
                unregister_kbsim_hotkey(d->hMainWnd, d->hotkeyStartId);
                register_kbsim_hotkey(d->hMainWnd, d->hotkeyStartId,
                                      packed);
            } else if (d->capturing == 3) {
                /* Set stop shortcut */
                int packed = (mod << 16) | vk;
                char buf[64];
                format_shortcut(packed, buf, sizeof(buf));
                SetWindowText(d->hShortStop, buf);
                db_set_setting_int("kbsim_stop_shortcut", packed);
                unregister_kbsim_hotkey(d->hMainWnd, d->hotkeyStopId);
                register_kbsim_hotkey(d->hMainWnd, d->hotkeyStopId,
                                      packed);
            }
            d->capturing = 0;
            return 0;
        }
        break;
    }

    case WM_KBSIM_CMD: {
        KbSimData *d = (KbSimData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (!d) return 0;
        int cmd = (int)wParam;
        if (cmd == 0 && !d->running) {
            /* populate cached settings before spawning thread */
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 1000);
                CloseHandle(d->hThread);
                d->hThread = nullptr;
            }
            char seqCheck[2048];
            GetWindowText(d->hSeqLbl, seqCheck, sizeof(seqCheck));
            if (seqCheck[0] == '\0') return 0;
            strcpy(d->cachedSeq, seqCheck);
            d->intervalMs = GetDlgItemInt(hWnd, IDC_KBSIM_INT_MIN,
                nullptr, FALSE) * 60000
                + GetDlgItemInt(hWnd, IDC_KBSIM_INT_SEC,
                nullptr, FALSE) * 1000
                + GetDlgItemInt(hWnd, IDC_KBSIM_INT_MS,
                nullptr, FALSE);
            if (d->intervalMs < 10) d->intervalMs = 10;
            d->offsetMs = GetDlgItemInt(hWnd, IDC_KBSIM_OFFSET,
                                         nullptr, FALSE);
            d->isContinuous = (SendMessage(d->hContinuous,
                BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            d->limitCount = GetDlgItemInt(hWnd, IDC_KBSIM_LIMIT,
                                           nullptr, FALSE);

            d->running = TRUE;
            d->hThread = CreateThread(nullptr, 0, KbSimThreadProc,
                                       d, 0, nullptr);
            SetWindowText(d->hStatus, "Status: Running...");
        } else if (cmd == 1 && d->running) {
            d->running = FALSE;
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
                d->hThread = nullptr;
            }
            char buf[64];
            sprintf(buf, "Status: Stopped (%d cycles)",
                    d->cyclesSoFar);
            SetWindowText(d->hStatus, buf);
        } else if (cmd == 2) {
            if (d->hThread) {
                CloseHandle(d->hThread);
                d->hThread = nullptr;
            }
            d->running = FALSE;
            char buf[64];
            sprintf(buf, "Status: Done (%d cycles)",
                    d->cyclesSoFar);
            SetWindowText(d->hStatus, buf);
        }
        return 0;
    }

    case WM_COMMAND: {
        KbSimData *d = (KbSimData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (!d) break;
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (code == BN_CLICKED) {
            if (id == IDC_KBSIM_RECORD) {
                if (d->capturing == 1) {
                    d->capturing = 0;
                    SetWindowText(d->hRecordBtn, "Record");
                    return 0;
                }
                d->capturing = 1;
                SetWindowText(d->hRecordBtn, "Stop");
                if (GetWindowTextLength(d->hSeqLbl) == 0)
                    SetWindowText(d->hSeqLbl, "");
                SetFocus(hWnd);
                return 0;
            }
            if (id == IDC_KBSIM_CLEAR) {
                SetWindowText(d->hSeqLbl, "");
                SetWindowText(d->hReadableLbl, "");
                d->keyCount = 0;
                extern void db_set_setting_str(const char *k,
                    const char *v);
                db_set_setting_str("kbsim_sequence", "");
                return 0;
            }
            if (id == IDC_KBSIM_SET_START) {
                d->capturing = 2;
                SetWindowText(d->hShortStart, "Press keys...");
                SetFocus(hWnd);
                return 0;
            }
            if (id == IDC_KBSIM_SET_STOP) {
                d->capturing = 3;
                SetWindowText(d->hShortStop, "Press keys...");
                SetFocus(hWnd);
                return 0;
            }
            if (id == IDC_KBSIM_START) {
                /* check sequence not empty */
                char seqCheck[2048];
                GetWindowText(d->hSeqLbl, seqCheck, sizeof(seqCheck));
                if (seqCheck[0] == '\0') {
                    MessageBox(hWnd,
                        "No key sequence recorded.\n"
                        "Click Record and press key combos first.",
                        "Keyboard Sim", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                int intervalMs = GetDlgItemInt(hWnd,
                    IDC_KBSIM_INT_MIN, nullptr, FALSE) * 60000
                    + GetDlgItemInt(hWnd,
                    IDC_KBSIM_INT_SEC, nullptr, FALSE) * 1000
                    + GetDlgItemInt(hWnd,
                    IDC_KBSIM_INT_MS, nullptr, FALSE);
                if (intervalMs < 10) intervalMs = 10;
                BOOL continuous = (SendMessage(d->hContinuous,
                                    BM_GETCHECK, 0, 0) == BST_CHECKED);

                db_set_setting_int("kbsim_interval_ms", intervalMs);
                db_set_setting_int("kbsim_random_offset",
                    GetDlgItemInt(hWnd, IDC_KBSIM_OFFSET,
                                  nullptr, FALSE));
                db_set_setting_int("kbsim_continuous",
                                   continuous ? 1 : 0);
                db_set_setting_int("kbsim_limited_count",
                    GetDlgItemInt(hWnd, IDC_KBSIM_LIMIT,
                                   nullptr, FALSE));

                /* cache settings for the thread (no UI reads) */
                GetWindowText(d->hSeqLbl, d->cachedSeq,
                              sizeof(d->cachedSeq));
                d->intervalMs = intervalMs;
                d->offsetMs = GetDlgItemInt(hWnd, IDC_KBSIM_OFFSET,
                                             nullptr, FALSE);
                d->isContinuous = continuous ? 1 : 0;
                d->limitCount = GetDlgItemInt(hWnd, IDC_KBSIM_LIMIT,
                                               nullptr, FALSE);

                PostMessage(hWnd, WM_KBSIM_CMD, 0, 0);
                return 0;
            }
            if (id == IDC_KBSIM_STOP) {
                PostMessage(hWnd, WM_KBSIM_CMD, 1, 0);
                return 0;
            }
            if (id == IDC_KBSIM_CONT) {
                EnableWindow(d->hLimitedCount, FALSE);
                return 0;
            }
            if (id == IDC_KBSIM_LIMITED) {
                EnableWindow(d->hLimitedCount, TRUE);
                return 0;
            }
        }
        break;
    }

    case WM_THEME_CHANGED:
        handle_theme_changed(hWnd);
        return 0;

    case WM_CLOSE: {
        int ims = GetDlgItemInt(hWnd, IDC_KBSIM_INT_MIN,
            nullptr, FALSE) * 60000
            + GetDlgItemInt(hWnd, IDC_KBSIM_INT_SEC,
            nullptr, FALSE) * 1000
            + GetDlgItemInt(hWnd, IDC_KBSIM_INT_MS,
            nullptr, FALSE);
        db_set_setting_int("kbsim_interval_ms", ims);
        db_set_setting_int("kbsim_random_offset",
            GetDlgItemInt(hWnd, IDC_KBSIM_OFFSET, nullptr, FALSE));
        db_set_setting_int("kbsim_continuous",
            (SendMessage(GetDlgItem(hWnd, IDC_KBSIM_CONT),
             BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0);
        db_set_setting_int("kbsim_limited_count",
            GetDlgItemInt(hWnd, IDC_KBSIM_LIMIT, nullptr, FALSE));
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }

    case WM_DESTROY: {
        KbSimData *d = (KbSimData *)GetWindowLongPtr(
            hWnd, GWLP_USERDATA);
        if (d) {
            d->running = FALSE;
            if (d->hThread) {
                WaitForSingleObject(d->hThread, 3000);
                CloseHandle(d->hThread);
                d->hThread = nullptr;
            }
            free(d);
        }
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void show_keyboard_sim(HWND hParent)
{
    HWND existing = FindWindow("KSC_KeyboardSim", nullptr);
    if (existing) {
        if (!IsWindowVisible(existing))
            ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return;
    }
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = KeyboardSimWndProc;
        wc.hInstance = g_hInst;
        wc.hIcon = g_hAppIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = "KSC_KeyboardSim";
        RegisterClass(&wc);
        registered = TRUE;
    }
    HWND hDlg = CreateWindow("KSC_KeyboardSim", "ksc - Keyboard Simulator",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                 CW_USEDEFAULT, CW_USEDEFAULT, 460, 440,
                 hParent, nullptr, g_hInst, nullptr);
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
                         hWnd, nullptr, g_hInst, nullptr);

        g_hListView = CreateWindow(WC_LISTVIEW, "",
                                    WS_CHILD | WS_VISIBLE |
                                    LVS_REPORT | LVS_SINGLESEL,
                                    0, 24, rc.right, rc.bottom - 24,
                                    hWnd, (HMENU)IDC_LISTVIEW,
                                    g_hInst, nullptr);

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

        SetTimer(hWnd, ID_TIMER_REFRESH, 1000, nullptr);
        SetTimer(hWnd, ID_TIMER_STATUS, 1000, nullptr);

        {
            int cs = cloudsync_get_schedule();
            DWORD intervals[] = {
                0, 300000, 900000, 1800000, 3600000, 43200000, 86400000
            };
            if (cs > 0 && cs <= 6)
                SetTimer(hWnd, ID_TIMER_CLOUD_SYNC,
                         intervals[cs], nullptr);
        }

        HMENU hMenu = CreateMenu();
        HMENU hFileMenu = CreatePopupMenu();
        AppendMenu(hFileMenu, MF_STRING, IDM_SETTINGS, "Settings");
        AppendMenu(hFileMenu, MF_STRING, IDM_PAUSE_TRACKING, "Pause Tracking");
        AppendMenu(hFileMenu, MF_STRING, IDM_HEATMAP, "Key Heatmap");
        AppendMenu(hFileMenu, MF_STRING, IDM_STATS, "Stats");
        AppendMenu(hFileMenu, MF_STRING, IDM_VIEW_LOGS, "View Logs");
        AppendMenu(hFileMenu, MF_STRING, IDM_EXPORT_CSV, "Export Data...");
        AppendMenu(hFileMenu, MF_STRING, IDM_MOUSE_CLICKER, "Mouse Clicker");
        AppendMenu(hFileMenu, MF_STRING, IDM_KEYBOARD_SIM, "Keyboard Sim");
        AppendMenu(hFileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hFileMenu, MF_STRING, IDM_BACKUP_DB, "Backup Database...");
        AppendMenu(hFileMenu, MF_STRING, IDM_CLOUD_BACKUP, "Cloud Backup");
        AppendMenu(hFileMenu, MF_STRING, IDM_RESTORE_DB, "Restore Database...");
        AppendMenu(hFileMenu, MF_SEPARATOR, 0, nullptr);
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
            SetWindowPos(g_hTotalLabel, nullptr, 0, 0,
                         rc.right, 24, SWP_NOZORDER);
        }
        if (g_hListView) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            SetWindowPos(g_hListView, nullptr, 0, 24,
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
        LRESULT cdRes = 0;
        if (handle_dark_lv_customdraw(IDC_LISTVIEW, lParam, &cdRes))
            return cdRes;
        NMHDR *nmh = (NMHDR *)lParam;
        if (nmh->idFrom == IDC_LISTVIEW && nmh->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *pnm = (NMLISTVIEW *)lParam;
            listview_toggle_sort(g_hListView, &g_mainSort, pnm->iSubItem,
                                 pnm->iSubItem == 1);
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_STATUS) {
            update_main_status_bar();
            tray_update_tip(hWnd, TRAY_ID);
        } else if (wParam == ID_TIMER_REFRESH) {
            refresh_list_view();
            tray_update_tip(hWnd, TRAY_ID);
        } else if (wParam == ID_TIMER_CLOUD_SYNC) {
            HWND hCloud = FindWindow("KSC_CloudBackup", nullptr);
            cloudsync_backup_trigger(hCloud);
        }
        return 0;

    case WM_INITMENUPOPUP:
        CheckMenuItem((HMENU)wParam, IDM_PAUSE_TRACKING,
            MF_BYCOMMAND | (keyhook_is_paused() ? MF_CHECKED : MF_UNCHECKED));
        return 0;

    case WM_THEME_CHANGED:
        update_theme(hWnd);
        update_auto_refresh(hWnd);
        EnumThreadWindows(GetCurrentThreadId(), EnumThemeWindowsProc, (LPARAM)hWnd);
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
        if (wParam == HOTKEY_ID_SHOW_KSC) {
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
            } else {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            return 0;
        }
        if (g_hClickerWnd && IsWindow(g_hClickerWnd)) {
            if (wParam == HOTKEY_ID_START_CLICK) {
                PostMessage(g_hClickerWnd, WM_CLICKER_CMD, 0, 0);
            } else if (wParam == HOTKEY_ID_STOP_CLICK) {
                PostMessage(g_hClickerWnd, WM_CLICKER_CMD, 1, 0);
            }
        } else if (wParam == HOTKEY_ID_START_CLICK ||
                   wParam == HOTKEY_ID_STOP_CLICK) {
            show_mouse_clicker(hWnd);
            if (g_hClickerWnd) {
                ShowWindow(g_hClickerWnd, SW_HIDE);
                PostMessage(g_hClickerWnd, WM_CLICKER_CMD,
                    wParam == HOTKEY_ID_START_CLICK ? 0 : 1, 0);
            }
        }
        if (wParam == HOTKEY_ID_START_KBSIM ||
            wParam == HOTKEY_ID_STOP_KBSIM) {
            HWND hKbSim = FindWindow("KSC_KeyboardSim", nullptr);
            if (!hKbSim) {
                show_keyboard_sim(hWnd);
                hKbSim = FindWindow("KSC_KeyboardSim", nullptr);
            }
            if (hKbSim) {
                ShowWindow(hKbSim, SW_HIDE);
                PostMessage(hKbSim, WM_KBSIM_CMD,
                    wParam == HOTKEY_ID_START_KBSIM ? 0 : 1, 0);
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
        case IDM_PAUSE_TRACKING: {
            bool paused = !keyhook_is_paused();
            keyhook_set_paused(paused);
            HMENU hMenu = GetMenu(hWnd);
            if (hMenu) {
                CheckMenuItem(hMenu, IDM_PAUSE_TRACKING,
                    MF_BYCOMMAND | (paused ? MF_CHECKED : MF_UNCHECKED));
            }
            update_main_status_bar();
            tray_update_tip(hWnd, TRAY_ID);
            break;
        }
        case IDM_HEATMAP:
            show_heatmap(hWnd);
            break;
        case IDM_STATS:
            show_stats_window(hWnd);
            break;
        case IDM_VIEW_LOGS:
            show_view_logs(hWnd);
            break;
        case IDM_EXPORT_CSV:
            export_all_data(hWnd);
            break;
        case IDM_MOUSE_CLICKER:
            show_mouse_clicker(hWnd);
            break;
        case IDM_KEYBOARD_SIM:
            show_keyboard_sim(hWnd);
            break;
        case IDM_KEYLOG_TOGGLE: {
            int curr = db_get_setting_int("keylogger_enabled", 0);
            int newv = curr ? 0 : 1;
            db_set_setting_int("keylogger_enabled", newv);
            keyhook_set_keylogger_enabled(newv);
            if (newv) keylog_open();
            else { keylog_flush_events(); keylog_close(); }
            MessageBox(hWnd,
                newv ? "Keylogger enabled." : "Keylogger disabled.",
                "Keylogger", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case IDM_CLOUD_BACKUP:
            show_cloud_backup(hWnd);
            break;
        case IDM_BACKUP_DB: {
            char dir[MAX_PATH] = "";
            db_get_db_dir(dir, sizeof(dir));
            if (dir[0] == '\0') break;
            SYSTEMTIME st;
            GetLocalTime(&st);
            char ts[32];
            sprintf(ts, "%04d%02d%02d_%02d%02d%02d",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
            BOOL preferZip = cloudsync_get_compress() ? TRUE : FALSE;
            char filePath[MAX_PATH] = "";
            snprintf(filePath, sizeof(filePath), "%s\\ksc_backup_%s.%s",
                     dir, ts, preferZip ? "zip" : "db");

            OPENFILENAME ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter =
                "ZIP Archive (*.zip)\0*.zip\0SQLite Database (*.db)\0*.db\0\0";
            ofn.nFilterIndex = preferZip ? 1 : 2;
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrInitialDir = dir;
            ofn.lpstrDefExt = preferZip ? "zip" : "db";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!GetSaveFileName(&ofn)) break;

            const char *dot = strrchr(filePath, '.');
            bool wantZip = (dot && _stricmp(dot, ".zip") == 0);
            int ok = 0;
            if (wantZip) {
                char tmpDb[MAX_PATH + 32];
                snprintf(tmpDb, sizeof(tmpDb), "%s.tmpdb", filePath);
                if (db_backup_safe(tmpDb)) {
                    ok = zip_compress_file(tmpDb, filePath, "ksc.db") ? 1 : 0;
                    DeleteFileA(tmpDb);
                }
            } else {
                ok = db_backup_safe(filePath);
            }

            if (ok) {
                bool klogOk = true;
                char klogSrc[MAX_PATH + 32];
                snprintf(klogSrc, sizeof(klogSrc), "%s\\ksc_keylog.db", dir);
                if (GetFileAttributesA(klogSrc) != INVALID_FILE_ATTRIBUTES) {
                    char base[MAX_PATH] = "";
                    size_t stemLen = dot ? (size_t)(dot - filePath) : strlen(filePath);
                    if (stemLen >= sizeof(base)) stemLen = sizeof(base) - 1;
                    memcpy(base, filePath, stemLen);
                    base[stemLen] = '\0';

                    if (wantZip) {
                        char tmpKlog[MAX_PATH + 40], klogZip[MAX_PATH + 40];
                        snprintf(tmpKlog, sizeof(tmpKlog), "%s_keylog.tmpdb", base);
                        snprintf(klogZip, sizeof(klogZip), "%s_keylog.zip", base);
                        if (keylog_backup_safe(tmpKlog)) {
                            klogOk = zip_compress_file(tmpKlog, klogZip, "ksc_keylog.db");
                            DeleteFileA(tmpKlog);
                        } else {
                            klogOk = false;
                        }
                    } else {
                        char klogDst[MAX_PATH + 40];
                        snprintf(klogDst, sizeof(klogDst), "%s_keylog.db", base);
                        klogOk = (keylog_backup_safe(klogDst) != 0);
                    }
                }
                MessageBox(hWnd,
                    klogOk ? "Backup created successfully."
                           : "Main database backed up, but keylogger backup failed.",
                    "Backup",
                    MB_OK | (klogOk ? MB_ICONINFORMATION : MB_ICONWARNING));
            } else {
                MessageBox(hWnd,
                    "Backup failed. Is the database in use?",
                    "Backup", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case IDM_RESTORE_DB: {
            OPENFILENAME ofn = {};
            char filePath[MAX_PATH] = "";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter =
                "Database & ZIP backups (*.db;*.zip)\0*.db;*.zip\0"
                "SQLite Database (*.db)\0*.db\0ZIP Archive (*.zip)\0*.zip\0\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (!GetOpenFileName(&ofn)) break;

            int res = MessageBox(hWnd,
                "This will replace all current statistics.\n"
                "ksc will exit. Restart manually.\n\nContinue?",
                "Restore Database", MB_YESNO | MB_ICONWARNING);
            if (res != IDYES) break;

            char dir[MAX_PATH] = "";
            db_get_db_dir(dir, sizeof(dir));
            if (dir[0] == '\0') break;

            const char *dot = strrchr(filePath, '.');
            bool isZip = (dot && _stricmp(dot, ".zip") == 0);
            char restoreSrc[MAX_PATH + 32];
            bool cleanupTemp = false;
            if (isZip) {
                snprintf(restoreSrc, sizeof(restoreSrc),
                         "%s\\ksc_restore_tmp.db", dir);
                if (!zip_extract_first_file(filePath, restoreSrc)) {
                    MessageBox(hWnd,
                        "Failed to extract database from ZIP archive.",
                        "Restore", MB_OK | MB_ICONERROR);
                    break;
                }
                cleanupTemp = true;
            } else {
                snprintf(restoreSrc, sizeof(restoreSrc), "%s", filePath);
            }

            FILE *vf = fopen(restoreSrc, "rb");
            char hdr[16] = {};
            size_t nr = vf ? fread(hdr, 1, sizeof(hdr), vf) : 0;
            if (vf) fclose(vf);
            if (nr != sizeof(hdr) || memcmp(hdr, "SQLite format 3", 15) != 0) {
                if (cleanupTemp) DeleteFileA(restoreSrc);
                MessageBox(hWnd,
                    "Selected file is not a valid SQLite database.",
                    "Restore", MB_OK | MB_ICONERROR);
                break;
            }

            keyhook_stop();
            db_close();
            keylog_flush_events();
            keylog_close();

            char dest[MAX_PATH + 32], wal[MAX_PATH + 40], shm[MAX_PATH + 40];
            snprintf(dest, sizeof(dest), "%s\\ksc.db", dir);
            snprintf(wal,  sizeof(wal),  "%s\\ksc.db-wal", dir);
            snprintf(shm,  sizeof(shm),  "%s\\ksc.db-shm", dir);
            DeleteFileA(wal);
            DeleteFileA(shm);

            BOOL copied = CopyFile(restoreSrc, dest, FALSE);
            if (cleanupTemp) DeleteFileA(restoreSrc);
            if (copied) {
                MessageBox(hWnd,
                    "Database restored. ksc will now exit.\n"
                    "Please restart ksc manually.",
                    "Restore", MB_OK | MB_ICONINFORMATION);
                DestroyWindow(hWnd);
            } else {
                MessageBox(hWnd,
                    "Restore failed. Ensure ksc is not running.",
                    "Restore", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case IDM_REFRESH:
            refresh_list_view();
            break;
        case IDM_QUIT:
            DestroyWindow(hWnd);
            break;
        case IDM_ABOUT:
            MessageBox(hWnd,
                "ksc - Keystroke Counter\n"
                "Version 1.1\n\n"
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
        {
            HWND hKb = FindWindow("KSC_KeyboardSim", nullptr);
            if (hKb) DestroyWindow(hKb);
            if (g_hClickerWnd && IsWindow(g_hClickerWnd))
                DestroyWindow(g_hClickerWnd);
        }
        UnregisterHotKey(hWnd, HOTKEY_ID_SHOW_KSC);
        UnregisterHotKey(hWnd, HOTKEY_ID_START_CLICK);
        UnregisterHotKey(hWnd, HOTKEY_ID_STOP_CLICK);
        UnregisterHotKey(hWnd, HOTKEY_ID_START_KBSIM);
        UnregisterHotKey(hWnd, HOTKEY_ID_STOP_KBSIM);
        if (g_hDarkBrush) { DeleteObject(g_hDarkBrush); g_hDarkBrush = nullptr; }
        if (g_hLvBrush)   { DeleteObject(g_hLvBrush);   g_hLvBrush = nullptr;   }
        if (g_hAppIcon)   { DestroyIcon(g_hAppIcon);    g_hAppIcon = nullptr;   }
        KillTimer(hWnd, ID_TIMER_STATUS);
        KillTimer(hWnd, ID_TIMER_REFRESH);
        KillTimer(hWnd, ID_TIMER_CLOUD_SYNC);
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
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "KSC_MainWindow";

    if (!RegisterClass(&wc)) return nullptr;

    return CreateWindow("KSC_MainWindow", "KSC - Keystroke Counter",
                         WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, 720, 460,
                         nullptr, nullptr, hInstance, nullptr);
}
