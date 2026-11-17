#include "telegram.h"
#include "database.h"
#include "ksc_private.h"
#include <winhttp.h>

static void append_json_escaped(char *dst, size_t cap, size_t *pos,
                                const char *src)
{
    if (!src || *pos >= cap) return;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = nullptr;
        char ubuf[8];
        if (c == '\\')      esc = "\\\\";
        else if (c == '"')  esc = "\\\"";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            snprintf(ubuf, sizeof(ubuf), "\\u%04X", c);
            esc = ubuf;
        }
        if (esc) {
            size_t elen = strlen(esc);
            if (*pos + elen >= cap) break;
            memcpy(dst + *pos, esc, elen);
            *pos += elen;
        } else {
            if (*pos + 1 >= cap) break;
            dst[(*pos)++] = (char)c;
        }
    }
    if (*pos < cap) dst[*pos] = '\0';
    else dst[cap - 1] = '\0';
}

static bool telegram_send_now(const char *token, const char *chatId,
                              const char *msg, int *outStatus)
{
    if (outStatus) *outStatus = 0;
    if (!token || !token[0] || !chatId || !chatId[0] || !msg) return false;

    char json[4096];
    size_t jp = 0;
    const char *p1 = "{\"chat_id\":\"";
    size_t l1 = strlen(p1);
    memcpy(json + jp, p1, l1);
    jp += l1;
    append_json_escaped(json, sizeof(json) - 32, &jp, chatId);

    const char *p2 = "\",\"text\":\"";
    size_t l2 = strlen(p2);
    memcpy(json + jp, p2, l2);
    jp += l2;
    append_json_escaped(json, sizeof(json) - 4, &jp, msg);

    json[jp++] = '"';
    json[jp++] = '}';
    json[jp] = '\0';

    wchar_t wpath[512];
    swprintf(wpath, 512, L"/bot%S/sendMessage", token);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 8000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.telegram.org", 443, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json", -1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    bool ok = false;
    DWORD bodyLen = (DWORD)strlen(json);
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
            0, (LPVOID)json, bodyLen, bodyLen, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
            WINHTTP_NO_HEADER_INDEX);
        if (outStatus) *outStatus = (int)status;
        ok = (status == 200);

        char buf[512];
        DWORD avail = 0, rd = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
            WinHttpReadData(hRequest, buf, avail < 512 ? avail : 512, &rd);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

static DWORD WINAPI telegram_thread(LPVOID param)
{
    char *msg = (char *)param;
    char token[256] = "";
    char chatId[128] = "";
    db_get_setting_str("tg_bot_token", "", token, sizeof(token));
    db_get_setting_str("tg_chat_id", "", chatId, sizeof(chatId));

    if (token[0] && chatId[0] && msg) {
        telegram_send_now(token, chatId, msg, nullptr);
    }
    free(msg);
    return 0;
}

void telegram_send(const char *msg)
{
    if (!msg) return;
    char *copy = _strdup(msg);
    if (!copy) return;
    HANDLE h = CreateThread(nullptr, 0, telegram_thread, copy, 0, nullptr);
    if (h) CloseHandle(h);
    else free(copy);
}

void telegram_test(HWND hParent)
{
    char token[256] = "";
    char chatId[128] = "";
    db_get_setting_str("tg_bot_token", "", token, sizeof(token));
    db_get_setting_str("tg_chat_id", "", chatId, sizeof(chatId));

    if (!token[0] || !chatId[0]) {
        MessageBox(hParent,
            "Please enter and save both Bot Token and Group Chat ID first.",
            "Telegram Test", MB_OK | MB_ICONWARNING);
        return;
    }

    int status = 0;
    bool ok = telegram_send_now(token, chatId,
        "\xE2\x9C\x85 ksc Telegram test \xE2\x80\x94 it works!", &status);
    if (ok) {
        MessageBox(hParent,
            "Test message delivered (HTTP 200).\nCheck your Telegram group.",
            "Telegram Test", MB_OK | MB_ICONINFORMATION);
    } else if (status > 0) {
        char err[160];
        snprintf(err, sizeof(err),
            "Telegram returned HTTP %d.\nCheck your Bot Token and Chat ID.",
            status);
        MessageBox(hParent, err, "Telegram Test", MB_OK | MB_ICONERROR);
    } else {
        MessageBox(hParent,
            "Failed to reach Telegram API.\nCheck your internet connection.",
            "Telegram Test", MB_OK | MB_ICONERROR);
    }
}
