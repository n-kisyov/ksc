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

    /* URL-encode the message */
    char encoded[2048];
    int ep = 0;
    for (int i = 0; msg[i] && ep < 2040; i++) {
        unsigned char c = (unsigned char)msg[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == ' ')
            encoded[ep++] = (c == ' ') ? '+' : c;
        else
            ep += sprintf(encoded + ep, "%%%02X", c);
    }
    encoded[ep] = '\0';

    /* build JSON body */
    char json[2560];
    sprintf(json,
        "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
        chatId, encoded);

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
