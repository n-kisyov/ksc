#include "ksc_private.h"
#include "keyhook.h"
#include "database.h"
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

    gui_init_dark_mode();

    HWND hWnd = gui_create_main_window(hInstance);
    if (!hWnd) {
        db_close();
        return 1;
    }

    if (!keyhook_start()) {
        MessageBox(NULL,
                   "Failed to start keyboard hook.\n\n"
                   "Try running as administrator or check your permissions.",
                   "KSC Error", MB_ICONERROR);
        DestroyWindow(hWnd);
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
    tray_cleanup(hWnd, TRAY_ID);
    db_close();

    return (int)msg.wParam;
}
