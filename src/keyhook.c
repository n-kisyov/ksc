#include "keyhook.h"
#include "database.h"
#include "keylogdb.h"
#include "ksc_private.h"

static HHOOK g_hKbHook = nullptr;
static HHOOK g_hMouseHook = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_readyEvent = nullptr;
static DWORD g_thread_id = 0;
static volatile bool g_keyloggerEnabled = false;
static volatile bool g_paused = false;

#define WPM_RING_SIZE 1024
static ULONGLONG g_wpmRing[WPM_RING_SIZE];
static int g_wpmHead = 0;
static int g_wpmCount = 0;
static int g_peakWpm = 0;
static SRWLOCK g_wpmLock = SRWLOCK_INIT;

static bool is_typing_vk(unsigned int vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return true;
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)
        return true;
    if ((vk >= VK_OEM_1 && vk <= VK_OEM_8) || vk == VK_OEM_102)
        return true;
    return vk == VK_SPACE || vk == VK_RETURN || vk == VK_BACK || vk == VK_TAB;
}

static void record_wpm_key(unsigned int vk)
{
    if (!is_typing_vk(vk)) return;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_wpmLock);
    g_wpmRing[g_wpmHead] = now;
    g_wpmHead = (g_wpmHead + 1) % WPM_RING_SIZE;
    if (g_wpmCount < WPM_RING_SIZE) g_wpmCount++;
    ReleaseSRWLockExclusive(&g_wpmLock);
}

void keyhook_get_wpm(int *out_wpm, int *out_peak_wpm)
{
    ULONGLONG now = GetTickCount64();
    int recentCount = 0;
    ULONGLONG oldest = now;
    ULONGLONG newest = 0;

    AcquireSRWLockExclusive(&g_wpmLock);
    for (int i = 0; i < g_wpmCount; i++) {
        int idx = (g_wpmHead - 1 - i + WPM_RING_SIZE) % WPM_RING_SIZE;
        ULONGLONG ts = g_wpmRing[idx];
        if (now - ts > 60000ULL) break;
        recentCount++;
        if (ts < oldest) oldest = ts;
        if (ts > newest) newest = ts;
    }

    int wpm = 0;
    if (recentCount > 0 && (now - newest) <= 10000ULL) {
        ULONGLONG span = now - oldest;
        if (span < 5000ULL) span = 5000ULL;
        if (span > 60000ULL) span = 60000ULL;
        wpm = (int)((recentCount * 60000.0) / ((double)span * 5.0) + 0.5);
        if (recentCount >= 10 && wpm > g_peakWpm) {
            g_peakWpm = wpm;
        }
    }
    int peak = g_peakWpm;
    ReleaseSRWLockExclusive(&g_wpmLock);

    if (out_wpm) *out_wpm = wpm;
    if (out_peak_wpm) *out_peak_wpm = peak;
}

void keyhook_reset_wpm(void)
{
    AcquireSRWLockExclusive(&g_wpmLock);
    g_wpmHead = 0;
    g_wpmCount = 0;
    g_peakWpm = 0;
    ReleaseSRWLockExclusive(&g_wpmLock);
}

const char *keyhook_get_name(unsigned int vk_code)
{
    switch (vk_code) {
    case VK_LBUTTON:    return "Left Mouse Btn";
    case VK_RBUTTON:    return "Right Mouse Btn";
    case VK_MBUTTON:    return "Middle Mouse Btn";
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

    static _Thread_local char buf[32];
    if ((vk_code >= '0' && vk_code <= '9') ||
        (vk_code >= 'A' && vk_code <= 'Z')) {
        snprintf(buf, sizeof(buf), "%c", (char)vk_code);
        return buf;
    }

    snprintf(buf, sizeof(buf), "Key 0x%02X", vk_code);
    return buf;
}

static void record_hook_event(int vk_code, const char *name)
{
    static DWORD s_lastPid = 0;
    static char s_lastExe[128] = "";

    char exeName[128] = "";
    char winTitle[256] = "";
    HWND hFg = GetForegroundWindow();
    if (hFg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hFg, &pid);
        if (pid != 0) {
            if (pid == s_lastPid) {
                snprintf(exeName, sizeof(exeName), "%s", s_lastExe);
            } else {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                           FALSE, pid);
                if (hProc) {
                    char fullPath[MAX_PATH] = "";
                    DWORD sz = sizeof(fullPath);
                    if (QueryFullProcessImageNameA(hProc, 0, fullPath, &sz)) {
                        const char *base = strrchr(fullPath, '\\');
                        const char *part = base ? (base + 1) : fullPath;
                        snprintf(exeName, sizeof(exeName), "%s", part);
                    }
                    CloseHandle(hProc);
                }
                s_lastPid = pid;
                snprintf(s_lastExe, sizeof(s_lastExe), "%s", exeName);
            }
        }
        if (g_keyloggerEnabled || exeName[0] == '\0') {
            GetWindowText(hFg, winTitle, sizeof(winTitle));
        }
    }

    const char *statsApp = exeName[0] ? exeName : winTitle;
    db_queue_event(vk_code, name, statsApp);
    if (g_keyloggerEnabled) {
        char klogApp[256] = "";
        if (exeName[0] && winTitle[0])
            snprintf(klogApp, sizeof(klogApp), "%s (%s)", exeName, winTitle);
        else
            snprintf(klogApp, sizeof(klogApp), "%s", statsApp);
        keylog_queue_event(name, vk_code, klogApp);
    }
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam,
                                              LPARAM lParam)
{
    if (nCode == HC_ACTION && !g_paused) {
        const KBDLLHOOKSTRUCT *p = (const KBDLLHOOKSTRUCT *)lParam;
        if (p->dwExtraInfo != KSC_INJECTED_MAGIC &&
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            const char *name = keyhook_get_name(p->vkCode);
            record_hook_event((int)p->vkCode, name);
            record_wpm_key(p->vkCode);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam,
                                           LPARAM lParam)
{
    if (nCode == HC_ACTION && !g_paused) {
        const MSLLHOOKSTRUCT *pm = (const MSLLHOOKSTRUCT *)lParam;
        if (pm->dwExtraInfo != KSC_INJECTED_MAGIC) {
            int vk = 0;
            if (wParam == WM_LBUTTONDOWN) vk = VK_LBUTTON;
            else if (wParam == WM_RBUTTONDOWN) vk = VK_RBUTTON;
            else if (wParam == WM_MBUTTONDOWN) vk = VK_MBUTTON;
            else if (wParam == WM_XBUTTONDOWN) {
                WORD xb = HIWORD(pm->mouseData);
                vk = (xb == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            }
            if (vk != 0) {
                record_hook_event(vk, keyhook_get_name((unsigned int)vk));
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static DWORD WINAPI HookThreadProc([[maybe_unused]] LPVOID lpParam)
{
    MSG msg = {};
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    g_hKbHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                  GetModuleHandle(nullptr), 0);
    g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc,
                                     GetModuleHandle(nullptr), 0);
    if (g_readyEvent) SetEvent(g_readyEvent);

    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hMouseHook) { UnhookWindowsHookEx(g_hMouseHook); g_hMouseHook = nullptr; }
    if (g_hKbHook)    { UnhookWindowsHookEx(g_hKbHook);    g_hKbHook = nullptr; }
    return 0;
}

bool keyhook_start(void)
{
    if (g_thread) return true;

    g_readyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!g_readyEvent) return false;

    g_thread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, &g_thread_id);
    if (!g_thread) {
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
        return false;
    }

    WaitForSingleObject(g_readyEvent, 2000);
    CloseHandle(g_readyEvent);
    g_readyEvent = nullptr;

    if (!g_hKbHook) {
        keyhook_stop();
        return false;
    }
    return true;
}

void keyhook_stop(void)
{
    if (g_thread) {
        PostThreadMessage(g_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_thread_id = 0;
    }
}

void keyhook_set_keylogger_enabled(bool enabled)
{
    g_keyloggerEnabled = enabled;
}

void keyhook_set_paused(bool paused)
{
    g_paused = paused;
}

bool keyhook_is_paused(void)
{
    return g_paused;
}
