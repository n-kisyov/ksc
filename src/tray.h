#ifndef TRAY_H
#define TRAY_H

#include <windows.h>

int tray_init(HWND hWnd, UINT id, HICON hIcon);
void tray_cleanup(HWND hWnd, UINT id);
void tray_show_menu(HWND hWnd, UINT id);
void tray_update_tip(HWND hWnd, UINT id);

#endif
