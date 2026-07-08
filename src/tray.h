#ifndef TRAY_H
#define TRAY_H

#include <windows.h>

int tray_init(HINSTANCE hInst, HWND hWnd, UINT id);
void tray_cleanup(HWND hWnd, UINT id);
void tray_show_menu(HWND hWnd, UINT id);

#endif
