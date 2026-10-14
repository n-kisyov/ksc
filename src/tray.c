#include "tray.h"
#include "ksc_private.h"
#include "database.h"

static NOTIFYICONDATA g_nid;

bool tray_init(HWND hWnd, UINT id, HICON hIcon)
{
    g_nid = (NOTIFYICONDATA){
        .cbSize = sizeof(NOTIFYICONDATA),
        .hWnd = hWnd,
        .uID = id,
        .uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP,
        .uCallbackMessage = WM_TRAYICON,
        .hIcon = hIcon ? hIcon : LoadIcon(nullptr, IDI_APPLICATION)
    };
    strcpy(g_nid.szTip, "KSC - Keystroke Counter");

    return Shell_NotifyIcon(NIM_ADD, &g_nid) != FALSE;
}

void tray_cleanup(HWND hWnd, UINT id)
{
    NOTIFYICONDATA nid = {
        .cbSize = sizeof(NOTIFYICONDATA),
        .hWnd = hWnd,
        .uID = id
    };
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

void tray_show_menu(HWND hWnd, [[maybe_unused]] UINT id)
{
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, IDM_SHOW, "Show/Hide");
    AppendMenu(hMenu, MF_STRING, IDM_HEATMAP, "Key Heatmap");
    AppendMenu(hMenu, MF_STRING, IDM_STATS, "Stats");
    AppendMenu(hMenu, MF_STRING, IDM_MOUSE_CLICKER, "Mouse Clicker");
    AppendMenu(hMenu, MF_STRING, IDM_KEYBOARD_SIM, "Keyboard Sim");
    AppendMenu(hMenu, MF_STRING, IDM_KEYLOG_TOGGLE, "Toggle Keylogger");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_SETTINGS, "Settings");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_QUIT, "Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

void tray_update_tip(HWND hWnd, UINT id)
{
    int64_t today = db_get_today_count();
    char tip[128];
    sprintf(tip, "ksc - %lld today", (long long)today);

    NOTIFYICONDATA nid = {
        .cbSize = sizeof(NOTIFYICONDATA),
        .hWnd = hWnd,
        .uID = id,
        .uFlags = NIF_TIP
    };
    strncpy(nid.szTip, tip, 127);
    nid.szTip[127] = '\0';
    Shell_NotifyIcon(NIM_MODIFY, &nid);
}
