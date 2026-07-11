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
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef TDF_ALLOW_DIALOG_CANCELLATION
#define TDF_ALLOW_DIALOG_CANCELLATION  0x0008
#endif

#ifndef TDF_POSITION_RELATIVE_TO_WINDOW
#define TDF_POSITION_RELATIVE_TO_WINDOW 0x1000
#endif

#define WM_TRAYICON      (WM_APP + 1)
#define WM_THEME_CHANGED (WM_APP + 2)

#define TRAY_ID 1

#define IDM_SHOW               1001
#define IDM_SETTINGS           1002
#define IDM_QUIT               1003
#define IDM_REFRESH            1004
#define IDM_ABOUT              1005
#define IDM_HEATMAP            1006
#define IDM_STATS              1007
#define IDM_EXPORT_CSV         1008
#define IDM_MOUSE_CLICKER      1009
#define IDM_VIEW_LOGS          1010
#define IDM_BACKUP_DB          1011
#define IDM_RESTORE_DB         1012
#define IDM_KEYLOG_TOGGLE       1013

#define IDC_LISTVIEW            2001
#define IDC_STARTUP_CHK          2002
#define IDC_START_MINIMIZED_CHK  2003
#define IDC_DARK_MODE_CHK        2004
#define IDC_AUTO_REFRESH_CHK     2005
#define IDC_DATE_FROM            2007
#define IDC_DATE_TO              2008
#define IDC_STATS_REFRESH_BTN    2009
#define IDC_EXPORT_BTN           2010
#define IDC_APP_COMBO            2011
#define IDC_CLICK_INTERVAL_MIN   2012
#define IDC_CLICK_INTERVAL_SEC   2013
#define IDC_CLICK_INTERVAL_MS    2014
#define IDC_CLICK_RANDOM_OFFSET  2015
#define IDC_CLICK_BTN_LEFT       2016
#define IDC_CLICK_BTN_RIGHT      2017
#define IDC_CLICK_MODE_CONT      2018
#define IDC_CLICK_MODE_LIMITED   2019
#define IDC_CLICK_LIMITED_COUNT  2020
#define IDC_CLICK_SHORT_START    2021
#define IDC_CLICK_SHORT_STOP     2022
#define IDC_CLICK_BTN_SET_START  2023
#define IDC_CLICK_BTN_SET_STOP   2024
#define IDC_CLICK_BTN_START      2025
#define IDC_CLICK_BTN_STOP       2026
#define IDC_CLICK_STATUS         2027
#define IDC_KEYLOGGER_CHK        2028
#define IDC_DELETE_KEYLOG_BTN    2029
#define IDC_RESET_STATS_BTN      2031
#define IDC_HOTKEY_SHOW_LBL      2032
#define IDC_HOTKEY_SET_SHOW      2033

#define HOTKEY_ID_START_CLICK 100
#define HOTKEY_ID_STOP_CLICK  101
#define HOTKEY_ID_SHOW_KSC    102
#define WM_CLICKER_CMD        (WM_APP + 5)

#define ID_TIMER_REFRESH 1

#endif
