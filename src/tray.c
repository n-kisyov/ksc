#include "tray.h"
#include "ksc_private.h"

static NOTIFYICONDATA g_nid;

int tray_init(HWND hWnd, UINT id, HICON hIcon)
{

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hWnd;
    g_nid.uID = id;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = hIcon ? hIcon : LoadIcon(NULL, IDI_APPLICATION);
    strcpy(g_nid.szTip, "KSC - Keystroke Counter");

    return Shell_NotifyIcon(NIM_ADD, &g_nid);
}

void tray_cleanup(HWND hWnd, UINT id)
{
    NOTIFYICONDATA nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hWnd;
    nid.uID = id;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

void tray_show_menu(HWND hWnd, UINT id)
{
    (void)id;

    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, IDM_SHOW, "Show/Hide");
    AppendMenu(hMenu, MF_STRING, IDM_HEATMAP, "Key Heatmap");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_SETTINGS, "Settings");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_QUIT, "Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}
