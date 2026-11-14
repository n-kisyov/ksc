#include "startup.h"
#include "ksc_private.h"

static constexpr char REG_PATH[] = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr char APP_NAME[] = "KSC";

bool startup_is_enabled(void)
{
    HKEY hKey;
    LONG result;

    result = RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0,
                          KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return false;

    char value[MAX_PATH];
    DWORD size = sizeof(value);

    result = RegQueryValueEx(hKey, APP_NAME, nullptr, nullptr,
                             (BYTE *)value, &size);
    RegCloseKey(hKey);

    return result == ERROR_SUCCESS;
}

bool startup_set_enabled(bool enable)
{
    HKEY hKey;
    LONG result;

    result = RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0,
                          KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS) return false;

    if (enable) {
        char exePath[MAX_PATH];
        GetModuleFileName(nullptr, exePath, MAX_PATH);

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
