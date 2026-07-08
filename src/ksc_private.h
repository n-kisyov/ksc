#ifndef KSC_PRIVATE_H
#define KSC_PRIVATE_H

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define WM_TRAYICON      (WM_APP + 1)
#define WM_THEME_CHANGED (WM_APP + 2)

#define TRAY_ID 1

#define IDM_SHOW               1001
#define IDM_SETTINGS           1002
#define IDM_QUIT               1003
#define IDM_REFRESH            1004
#define IDM_ABOUT              1005

#define IDC_LISTVIEW           2001
#define IDC_STARTUP_CHK         2002
#define IDC_START_MINIMIZED_CHK 2003
#define IDC_DARK_MODE_CHK       2004
#define IDC_AUTO_REFRESH_CHK    2005

#define ID_TIMER_REFRESH 1

#endif
