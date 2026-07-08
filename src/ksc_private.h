#ifndef KSC_PRIVATE_H
#define KSC_PRIVATE_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_TRAYICON (WM_APP + 1)

#define TRAY_ID 1

#define IDM_SHOW        1001
#define IDM_SETTINGS    1002
#define IDM_QUIT        1003
#define IDM_REFRESH     1004
#define IDM_ABOUT       1005

#define IDC_LISTVIEW    2001
#define IDC_STARTUP_CHK 2002

#define ID_TIMER_REFRESH 1

#endif
