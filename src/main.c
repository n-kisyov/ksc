#include "ksc_private.h"
#include "keyhook.h"
#include "database.h"
#include "keylogdb.h"
#include "cloudsync.h"
#include "ssh_sync.h"
#include "gui.h"
#include "tray.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    HANDLE hMutex = CreateMutex(NULL, TRUE, "KSC_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES;
    InitCommonControlsEx(&icex);

    if (!db_init()) {
        MessageBox(NULL, "Failed to initialize database.",
                   "KSC Error", MB_ICONERROR);
        return 1;
    }

    if (db_get_setting_int("keylogger_enabled", 0)) {
        keylog_open();
        keyhook_set_keylogger_enabled(1);
    }

    cloudsync_init();
    ssh_sync_init();

    gui_init_dark_mode();

    HWND hWnd = gui_create_main_window(hInstance);
    if (!hWnd) {
        keylog_close();
        db_close();
        return 1;
    }

    {
        int sc = db_get_setting_int("show_ksc_shortcut",
                   (MOD_CONTROL | MOD_SHIFT) << 16 | 'K');
        if (sc > 0) {
            int vk = sc & 0xFFFF;
            int mod = (sc >> 16) & 0xFFFF;
            RegisterHotKey(hWnd, HOTKEY_ID_SHOW_KSC,
                           mod | MOD_NOREPEAT, vk);
        }
    }

    {
        int scStart = db_get_setting_int("kbsim_start_shortcut",
                       (MOD_CONTROL | MOD_SHIFT) << 16 | 'J');
        int scStop  = db_get_setting_int("kbsim_stop_shortcut",
                       (MOD_CONTROL | MOD_SHIFT) << 16 | 'M');
        if (scStart > 0) {
            RegisterHotKey(hWnd, HOTKEY_ID_START_KBSIM,
                (scStart & 0xFFFF0000 ? (scStart >> 16) & 0xFFFF : 0) |
                MOD_NOREPEAT, scStart & 0xFFFF);
        }
        if (scStop > 0) {
            RegisterHotKey(hWnd, HOTKEY_ID_STOP_KBSIM,
                (scStop & 0xFFFF0000 ? (scStop >> 16) & 0xFFFF : 0) |
                MOD_NOREPEAT, scStop & 0xFFFF);
        }
    }

    {
        int cs = db_get_setting_int("clicker_start_shortcut",
                   (MOD_CONTROL | MOD_SHIFT) << 16 | 'S');
        int ce = db_get_setting_int("clicker_stop_shortcut",
                   (MOD_CONTROL | MOD_SHIFT) << 16 | 'X');
        if (cs > 0) {
            RegisterHotKey(hWnd, HOTKEY_ID_START_CLICK,
                (cs & 0xFFFF0000 ? (cs >> 16) & 0xFFFF : 0) |
                MOD_NOREPEAT, cs & 0xFFFF);
        }
        if (ce > 0) {
            RegisterHotKey(hWnd, HOTKEY_ID_STOP_CLICK,
                (ce & 0xFFFF0000 ? (ce >> 16) & 0xFFFF : 0) |
                MOD_NOREPEAT, ce & 0xFFFF);
        }
    }

    if (!keyhook_start()) {
        MessageBox(NULL,
                   "Failed to start keyboard hook.\n\n"
                   "Try running as administrator or check your permissions.",
                   "KSC Error", MB_ICONERROR);
        DestroyWindow(hWnd);
        keylog_close();
        db_close();
        return 1;
    }

    tray_init(hWnd, TRAY_ID, gui_get_app_icon());

    if (db_get_setting_int("start_minimized", 0)) {
        ShowWindow(hWnd, SW_HIDE);
    } else {
        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    keyhook_stop();
    db_flush_events();
    keylog_flush_events();
    tray_cleanup(hWnd, TRAY_ID);
    db_close();
    keylog_close();

    return (int)msg.wParam;
}
