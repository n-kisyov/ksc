#include "telegram.h"
#include "ksc_private.h"
#include <winhttp.h>

static DWORD WINAPI telegram_thread(LPVOID param)
{
    char *msg = (char *)param;

    extern void db_get_setting_str(const char *k, const char *d,
                                    char *o, int s);
    char token[256] = "";
    char chatId[128] = "";
    db_get_setting_str("tg_bot_token", "", token, sizeof(token));
    db_get_setting_str("tg_chat_id", "", chatId, sizeof(chatId));

    if (!token[0] || !chatId[0]) { free(msg); return 0; }

    /* build JSON body — escape \, ", \n for JSON */
    char json[4096];
    int jp = 0;
    jp += sprintf(json + jp, "{\"chat_id\":\"%s\",\"text\":\"", chatId);
    for (int i = 0; msg[i] && jp < 4000; i++) {
        if (msg[i] == '\\') { json[jp++] = '\\'; json[jp++] = '\\'; }
        else if (msg[i] == '"') { json[jp++] = '\\'; json[jp++] = '"'; }
        else if (msg[i] == '\n') { json[jp++] = '\\'; json[jp++] = 'n'; }
        else json[jp++] = msg[i];
    }
    jp += sprintf(json + jp, "\"}");

    wchar_t whost[256];
    MultiByteToWideChar(CP_UTF8, 0, "api.telegram.org", -1, whost, 256);
    wchar_t wpath[512];
    swprintf(wpath, 512, L"/bot%S/sendMessage", token);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { free(msg); return 0; }

    HINTERNET hConnect = WinHttpConnect(hSession, whost, 443, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); free(msg); return 0; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); free(msg); return 0; }

    wchar_t whdr[512];
    wcscpy(whdr, L"Content-Type: application/json");
    WinHttpAddRequestHeaders(hRequest, whdr, -1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    DWORD bodyLen = (DWORD)strlen(json);
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
        0, (LPVOID)json, bodyLen, bodyLen, 0);
    WinHttpReceiveResponse(hRequest, NULL);

    /* read response (ignored) */
    char buf[512];
    DWORD avail = 0, rd = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
        WinHttpReadData(hRequest, buf,
            avail < 512 ? avail : 512, &rd);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    free(msg);
    return 0;
}

void telegram_send(const char *msg)
{
    CreateThread(NULL, 0, telegram_thread,
                  _strdup(msg), 0, NULL);
}

void telegram_test(HWND hParent)
{
    telegram_send("\xE2\x9C\x85 ksc Telegram test \xE2\x80\x94 it works!");
    MessageBox(hParent,
        "Test message sent.\nCheck your Telegram group.",
        "Telegram Test", MB_OK | MB_ICONINFORMATION);
}
