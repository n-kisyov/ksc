#include "startup.h"
#include "ksc_private.h"

#define REG_PATH "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APP_NAME "KSC"

int startup_is_enabled(void)
{
    HKEY hKey;
    LONG result;

    result = RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0,
                          KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return 0;

    char value[MAX_PATH];
    DWORD size = sizeof(value);

    result = RegQueryValueEx(hKey, APP_NAME, NULL, NULL,
                             (BYTE *)value, &size);
    RegCloseKey(hKey);

    return result == ERROR_SUCCESS;
}

int startup_set_enabled(int enable)
{
    HKEY hKey;
    LONG result;

    result = RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0,
                          KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS) return 0;

    if (enable) {
        char exePath[MAX_PATH];
        GetModuleFileName(NULL, exePath, MAX_PATH);

        char quoted[MAX_PATH + 4];
        sprintf(quoted, "\"%s\"", exePath);

        result = RegSetValueEx(hKey, APP_NAME, 0, REG_SZ,
                               (BYTE *)quoted, (DWORD)(strlen(quoted) + 1));
    } else {
        result = RegDeleteValue(hKey, APP_NAME);
    }

    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}
