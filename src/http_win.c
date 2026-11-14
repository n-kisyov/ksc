#include "ksc_private.h"
#include <winhttp.h>

static int http_request_impl(const wchar_t *verb, const char *url,
                             const char *bearer, const wchar_t *contentTypeHdr,
                             const char *body, char **outResponse, int *outStatus)
{
    *outResponse = nullptr;
    *outStatus = 0;

    if (!url || !url[0]) return 0;

    wchar_t wurl[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048) <= 0)
        return 0;

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {}, extra[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;

    wchar_t fullPath[8192];
    wcscpy(fullPath, path);
    if (extra[0]) wcscat(fullPath, extra);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    INTERNET_PORT port = uc.nPort
        ? uc.nPort
        : (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80);
    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return 0;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS)
                  ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb, fullPath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    if (contentTypeHdr && contentTypeHdr[0]) {
        WinHttpAddRequestHeaders(hRequest, contentTypeHdr, -1,
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (bearer && bearer[0]) {
        wchar_t whdr[2048];
        swprintf(whdr, 2048, L"Authorization: Bearer %S\r\n", bearer);
        WinHttpAddRequestHeaders(hRequest, whdr, -1,
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    const char *sendBody = body ? body : "";
    DWORD bodyLen = (DWORD)strlen(sendBody);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
            0, (LPVOID)sendBody, bodyLen, bodyLen, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
        WINHTTP_NO_HEADER_INDEX);
    *outStatus = (int)status;

    DWORD total = 0;
    char *buf = nullptr;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
        char *tmp = realloc(buf, total + available + 1);
        if (!tmp) {
            free(buf);
            buf = nullptr;
            break;
        }
        buf = tmp;
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf + total, available, &read) || read == 0)
            break;
        total += read;
        buf[total] = '\0';
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    *outResponse = buf;
    return 1;
}

int http_get_json(const char *url, const char *bearer,
                   char **response, int *status)
{
    return http_request_impl(L"GET", url, bearer, nullptr, nullptr,
                             response, status);
}

int http_post_form(const char *url, const char *bearer,
                    const char *body, char **response, int *status)
{
    return http_request_impl(L"POST", url, bearer,
                             L"Content-Type: application/x-www-form-urlencoded\r\n",
                             body, response, status);
}

int http_post_json(const char *url, const char *bearer,
                    const char *jsonBody, char **response, int *status)
{
    return http_request_impl(L"POST", url, bearer,
                             L"Content-Type: application/json\r\n",
                             jsonBody, response, status);
}

int http_upload_file(const char *url, const char *bearer,
                      const char *filePath, const char *remoteName,
                      const char *folderId,
                      char **response, int *status)
{
    *response = nullptr;
    *status = 0;
    if (!url || !filePath || !remoteName) return 0;

    HANDLE hFile = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return 0;
    }

    char *fileData = malloc(fileSize > 0 ? fileSize : 1);
    if (!fileData) {
        CloseHandle(hFile);
        return 0;
    }
    DWORD read = 0;
    if (fileSize > 0 && (!ReadFile(hFile, fileData, fileSize, &read, nullptr) || read != fileSize)) {
        free(fileData);
        CloseHandle(hFile);
        return 0;
    }
    CloseHandle(hFile);

    char meta[512];
    snprintf(meta, sizeof(meta),
        "{\"name\":\"%s\",\"parents\":[\"%s\"]}",
        remoteName, (folderId && folderId[0]) ? folderId : "root");

    char boundary[64];
    snprintf(boundary, sizeof(boundary), "ksc_boundary_%lu", GetTickCount());

    size_t bodySize = 256 + strlen(meta) + 64 + fileSize + 64;
    char *body = malloc(bodySize);
    if (!body) {
        free(fileData);
        return 0;
    }

    int pos = 0;
    pos += sprintf(body + pos, "--%s\r\n", boundary);
    pos += sprintf(body + pos,
        "Content-Type: application/json\r\n\r\n%s\r\n", meta);
    pos += sprintf(body + pos, "--%s\r\n", boundary);
    pos += sprintf(body + pos,
        "Content-Type: application/octet-stream\r\n\r\n");
    if (fileSize > 0) {
        memcpy(body + pos, fileData, fileSize);
        pos += (int)fileSize;
    }
    pos += sprintf(body + pos, "\r\n--%s--\r\n", boundary);

    free(fileData);

    wchar_t wurl[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048) <= 0) {
        free(body);
        return 0;
    }

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, pth[2048] = {}, xtra[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pth;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = xtra;
    uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        free(body);
        return 0;
    }

    wchar_t fullPath[8192];
    wcscpy(fullPath, pth);
    if (xtra[0]) wcscat(fullPath, xtra);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { free(body); return 0; }

    INTERNET_PORT port = uc.nPort
        ? uc.nPort
        : (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80);
    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) { free(body); WinHttpCloseHandle(hSession); return 0; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS)
                  ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", fullPath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        free(body);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    wchar_t whdr[4096];
    swprintf(whdr, 4096,
        L"Content-Type: multipart/related; boundary=%S\r\n"
        L"Authorization: Bearer %S\r\n",
        boundary, bearer ? bearer : "");
    WinHttpAddRequestHeaders(hRequest, whdr, -1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
            0, (LPVOID)body, (DWORD)pos, (DWORD)pos, 0)) {
        free(body);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        free(body);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    DWORD st = 0, sz = sizeof(st);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
        WINHTTP_NO_HEADER_INDEX);
    *status = (int)st;

    DWORD total = 0;
    char *rbuf = nullptr;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        char *tmp = realloc(rbuf, total + avail + 1);
        if (!tmp) {
            free(rbuf);
            rbuf = nullptr;
            break;
        }
        rbuf = tmp;
        DWORD rd = 0;
        if (!WinHttpReadData(hRequest, rbuf + total, avail, &rd) || rd == 0)
            break;
        total += rd;
        rbuf[total] = '\0';
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    free(body);
    *response = rbuf;
    return 1;
}
