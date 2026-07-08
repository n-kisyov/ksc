#ifndef GUI_H
#define GUI_H

#include <windows.h>

HWND gui_create_main_window(HINSTANCE hInstance);
void gui_init_dark_mode(void);
HICON gui_get_app_icon(void);

#endif
