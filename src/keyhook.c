#include "keyhook.h"
#include "database.h"
#include "ksc_private.h"

static HHOOK g_hook = NULL;
static HANDLE g_thread = NULL;
static DWORD g_thread_id = 0;

const char *keyhook_get_name(unsigned int vk_code)
{
    switch (vk_code) {
    case VK_LBUTTON:    return "Left Mouse";
    case VK_RBUTTON:    return "Right Mouse";
    case VK_MBUTTON:    return "Middle Mouse";
    case VK_XBUTTON1:   return "Mouse X1";
    case VK_XBUTTON2:   return "Mouse X2";
    case VK_BACK:       return "Backspace";
    case VK_TAB:        return "Tab";
    case VK_CLEAR:      return "Clear";
    case VK_RETURN:     return "Enter";
    case VK_SHIFT:      return "Shift";
    case VK_CONTROL:    return "Ctrl";
    case VK_MENU:       return "Alt";
    case VK_PAUSE:      return "Pause";
    case VK_CAPITAL:    return "Caps Lock";
    case VK_ESCAPE:     return "Esc";
    case VK_SPACE:      return "Space";
    case VK_PRIOR:      return "Page Up";
    case VK_NEXT:       return "Page Down";
    case VK_END:        return "End";
    case VK_HOME:       return "Home";
    case VK_LEFT:       return "Left Arrow";
    case VK_UP:         return "Up Arrow";
    case VK_RIGHT:      return "Right Arrow";
    case VK_DOWN:       return "Down Arrow";
    case VK_SELECT:     return "Select";
    case VK_PRINT:      return "Print";
    case VK_EXECUTE:    return "Execute";
    case VK_SNAPSHOT:   return "Print Screen";
    case VK_INSERT:     return "Insert";
    case VK_DELETE:     return "Delete";
    case VK_HELP:       return "Help";
    case VK_LWIN:       return "Left Win";
    case VK_RWIN:       return "Right Win";
    case VK_APPS:       return "Apps/Menu";
    case VK_SLEEP:      return "Sleep";
    case VK_NUMPAD0:    return "Num 0";
    case VK_NUMPAD1:    return "Num 1";
    case VK_NUMPAD2:    return "Num 2";
    case VK_NUMPAD3:    return "Num 3";
    case VK_NUMPAD4:    return "Num 4";
    case VK_NUMPAD5:    return "Num 5";
    case VK_NUMPAD6:    return "Num 6";
    case VK_NUMPAD7:    return "Num 7";
    case VK_NUMPAD8:    return "Num 8";
    case VK_NUMPAD9:    return "Num 9";
    case VK_MULTIPLY:   return "Num *";
    case VK_ADD:        return "Num +";
    case VK_SEPARATOR:  return "Num Separator";
    case VK_SUBTRACT:   return "Num -";
    case VK_DECIMAL:    return "Num .";
    case VK_DIVIDE:     return "Num /";
    case VK_F1:         return "F1";
    case VK_F2:         return "F2";
    case VK_F3:         return "F3";
    case VK_F4:         return "F4";
    case VK_F5:         return "F5";
    case VK_F6:         return "F6";
    case VK_F7:         return "F7";
    case VK_F8:         return "F8";
    case VK_F9:         return "F9";
    case VK_F10:        return "F10";
    case VK_F11:        return "F11";
    case VK_F12:        return "F12";
    case VK_F13:        return "F13";
    case VK_F14:        return "F14";
    case VK_F15:        return "F15";
    case VK_F16:        return "F16";
    case VK_F17:        return "F17";
    case VK_F18:        return "F18";
    case VK_F19:        return "F19";
    case VK_F20:        return "F20";
    case VK_F21:        return "F21";
    case VK_F22:        return "F22";
    case VK_F23:        return "F23";
    case VK_F24:        return "F24";
    case VK_NUMLOCK:    return "Num Lock";
    case VK_SCROLL:     return "Scroll Lock";
    case VK_LSHIFT:     return "Left Shift";
    case VK_RSHIFT:     return "Right Shift";
    case VK_LCONTROL:   return "Left Ctrl";
    case VK_RCONTROL:   return "Right Ctrl";
    case VK_LMENU:      return "Left Alt";
    case VK_RMENU:      return "Right Alt";
    case VK_OEM_1:      return "Semicolon (;)";
    case VK_OEM_PLUS:   return "Equals (=)";
    case VK_OEM_COMMA:  return "Comma (,)";
    case VK_OEM_MINUS:  return "Minus (-)";
    case VK_OEM_PERIOD: return "Period (.)";
    case VK_OEM_2:      return "Slash (/)";
    case VK_OEM_3:      return "Tilde (~)";
    case VK_OEM_4:      return "Left Bracket ([)";
    case VK_OEM_5:      return "Backslash (\\)";
    case VK_OEM_6:      return "Right Bracket (])";
    case VK_OEM_7:      return "Quote (')";
    case VK_OEM_8:      return "OEM 8";
    case VK_OEM_102:    return "Angle Bracket (<>)";
    case VK_ATTN:       return "Attn";
    case VK_CRSEL:      return "CrSel";
    case VK_EXSEL:      return "ExSel";
    case VK_EREOF:      return "Erase EOF";
    case VK_PLAY:       return "Play";
    case VK_ZOOM:       return "Zoom";
    case VK_NONAME:     return "NoName";
    case VK_PA1:        return "PA1";
    case VK_OEM_CLEAR:  return "Clear";
    }

    if (vk_code >= '0' && vk_code <= '9') {
        static char buf[8];
        sprintf(buf, "%c", (char)vk_code);
        return buf;
    }
    if (vk_code >= 'A' && vk_code <= 'Z') {
        static char buf[8];
        sprintf(buf, "%c", (char)vk_code);
        return buf;
    }

    static char buf[16];
    sprintf(buf, "Key 0x%02X", vk_code);
    return buf;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *)lParam;
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            const char *name = keyhook_get_name(p->vkCode);
            db_increment_key(p->vkCode, name);
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI HookThreadProc(LPVOID lpParam)
{
    (void)lpParam;

    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandle(NULL), 0);
    if (!g_hook) return 1;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    return 0;
}

int keyhook_start(void)
{
    g_thread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, &g_thread_id);
    return g_thread != NULL;
}

void keyhook_stop(void)
{
    if (g_thread) {
        PostThreadMessage(g_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
        g_thread_id = 0;
    }
}
