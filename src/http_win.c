#include "ksc_private.h"
#include <winhttp.h>

static int http_do(const wchar_t *verb, const wchar_t *fullUrl,
                    const char *bearer, const char *body,
                    char **outResponse, int *outStatus)
{
    *outResponse = NULL;
    *outStatus = 0;

    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[2048] = {0}, extra[4096] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(fullUrl, 0, 0, &uc)) return 0;

    wchar_t fullPath[8192];
    wcscpy(fullPath, path);
    if (extra[0]) wcscat(fullPath, extra);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    HINTERNET hConnect = WinHttpConnect(hSession, host,
        uc.nPort ? uc.nPort : (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80), 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 0; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS)
                  ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb, fullPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return 0; }

    if (bearer && bearer[0]) {
        char hdr[1024];
        sprintf(hdr, "Bearer %s", bearer);
        wchar_t whdr[1024];
        MultiByteToWideChar(CP_UTF8, 0, hdr, -1, whdr, 1024);
        WinHttpAddRequestHeaders(hRequest, whdr, -1,
            WINHTTP_ADDREQ_FLAG_ADD);
    }

    const char *sendBody = body ? body : "";
    DWORD bodyLen = (DWORD)strlen(sendBody);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
            0, (LPVOID)sendBody, bodyLen, bodyLen, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
        WINHTTP_NO_HEADER_INDEX);
    *outStatus = (int)status;

    DWORD total = 0;
    char *buf = NULL;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
        char *tmp = realloc(buf, total + available + 1);
        if (!tmp) { free(buf); break; }
        buf = tmp;
        DWORD read = 0;
        WinHttpReadData(hRequest, buf + total, available, &read);
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
    wchar_t wurl[2048];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);
    return http_do(L"GET", wurl, bearer, NULL, response, status);
}

int http_post_form(const char *url, const char *bearer,
                    const char *body, char **response, int *status)
{
    wchar_t wurl[2048];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);

    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, pth[2048] = {0}, xtra[4096] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pth;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = xtra;
    uc.dwExtraInfoLength = 4095;
    WinHttpCrackUrl(wurl, 0, 0, &uc);

    wchar_t fullPath[8192];
    wcscpy(fullPath, pth);
    if (xtra[0]) wcscat(fullPath, xtra);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    HINTERNET hConnect = WinHttpConnect(hSession, host, 443, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 0; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", fullPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0; }

    wchar_t whdr[2048];
    if (bearer && bearer[0])
        swprintf(whdr, 2048,
            L"Content-Type: application/x-www-form-urlencoded\r\n"
            L"Authorization: Bearer %S", bearer);
    else
        wcscpy(whdr,
            L"Content-Type: application/x-www-form-urlencoded");
    WinHttpAddRequestHeaders(hRequest, whdr, -1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    DWORD bodyLen = (DWORD)strlen(body);
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
            0, (LPVOID)body, bodyLen, bodyLen, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0;
    }

    DWORD httpStatus = 0, sz = sizeof(httpStatus);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &httpStatus, &sz,
        WINHTTP_NO_HEADER_INDEX);
    *status = (int)httpStatus;

    DWORD total = 0;
    char *buf = NULL;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
        char *tmp = realloc(buf, total + available + 1);
        if (!tmp) { free(buf); break; }
        buf = tmp;
        DWORD read = 0;
        WinHttpReadData(hRequest, buf + total, available, &read);
        total += read;
        buf[total] = '\0';
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    *response = buf;
    return 1;
}

int http_post_json(const char *url, const char *bearer,
                    const char *jsonBody, char **response, int *status)
{
    wchar_t wurl[2048];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);

    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, pth[2048] = {0}, xtra[4096] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pth;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = xtra;
    uc.dwExtraInfoLength = 4095;
    WinHttpCrackUrl(wurl, 0, 0, &uc);

    wchar_t fullPath[8192];
    wcscpy(fullPath, pth);
    if (xtra[0]) wcscat(fullPath, xtra);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;

    HINTERNET hConnect = WinHttpConnect(hSession, host, 443, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return 0; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", fullPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return 0; }

    wchar_t whdr[2048];
    swprintf(whdr, 2048,
        L"Content-Type: application/json\r\nAuthorization: Bearer %S",
        bearer ? bearer : "");
    WinHttpAddRequestHeaders(hRequest, whdr, -1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    DWORD bodyLen = (DWORD)strlen(jsonBody);
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
            0, (LPVOID)jsonBody, bodyLen, bodyLen, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0;
    }

    DWORD httpStatus = 0, sz = sizeof(httpStatus);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &httpStatus, &sz,
        WINHTTP_NO_HEADER_INDEX);
    *status = (int)httpStatus;

    DWORD total = 0;
    char *buf = NULL;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
        char *tmp = realloc(buf, total + available + 1);
        if (!tmp) { free(buf); break; }
        buf = tmp;
        DWORD read = 0;
        WinHttpReadData(hRequest, buf + total, available, &read);
        total += read;
        buf[total] = '\0';
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    *response = buf;
    return 1;
}

int http_upload_file(const char *url, const char *bearer,
                      const char *filePath, const char *remoteName,
                      const char *folderId,
                      char **response, int *status)
{
    HANDLE hFile = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    DWORD fileSize = GetFileSize(hFile, NULL);
    char *fileData = malloc(fileSize);
    if (!fileData) { CloseHandle(hFile); return 0; }
    DWORD read = 0;
    ReadFile(hFile, fileData, fileSize, &read, NULL);
    CloseHandle(hFile);

    char meta[512];
    sprintf(meta,
        "{\"name\":\"%s\",\"parents\":[\"%s\"]}",
        remoteName, (folderId && folderId[0]) ? folderId : "root");

    char boundary[64];
    sprintf(boundary, "ksc_boundary_%lu", GetTickCount());

    int bodySize = 256 + (int)strlen(meta) + 64 + fileSize + 64;
    char *body = malloc(bodySize);
    if (!body) { free(fileData); return 0; }

    int pos = 0;
    pos += sprintf(body + pos, "--%s\r\n", boundary);
    pos += sprintf(body + pos,
        "Content-Type: application/json\r\n\r\n%s\r\n", meta);
    pos += sprintf(body + pos, "--%s\r\n", boundary);
    pos += sprintf(body + pos,
        "Content-Type: application/octet-stream\r\n\r\n");
    memcpy(body + pos, fileData, fileSize);
    pos += fileSize;
    pos += sprintf(body + pos, "\r\n--%s--\r\n", boundary);

    free(fileData);

    wchar_t wurl[2048];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);

    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, pth[2048] = {0}, xtra[4096] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pth;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = xtra;
    uc.dwExtraInfoLength = 4095;
    WinHttpCrackUrl(wurl, 0, 0, &uc);

    wchar_t fullPath[8192];
    wcscpy(fullPath, pth);
    if (xtra[0]) wcscat(fullPath, xtra);

    HINTERNET hSession = WinHttpOpen(L"ksc/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { free(body); return 0; }

    HINTERNET hConnect = WinHttpConnect(hSession, host, 443, 0);
    if (!hConnect) { free(body); WinHttpCloseHandle(hSession); return 0; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", fullPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) { free(body); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return 0; }

    wchar_t whdr[4096];
    swprintf(whdr, 4096,
        L"Content-Type: multipart/related; boundary=%S\r\n"
        L"Authorization: Bearer %S",
        boundary, bearer ? bearer : "");
    WinHttpAddRequestHeaders(hRequest, whdr, -1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
            0, (LPVOID)body, pos, pos, 0)) {
        free(body); WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return 0;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        free(body); WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return 0;
    }

    DWORD st = 0, sz = sizeof(st);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
        WINHTTP_NO_HEADER_INDEX);
    *status = (int)st;

    DWORD total = 0;
    char *rbuf = NULL;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        char *tmp = realloc(rbuf, total + avail + 1);
        if (!tmp) { free(rbuf); break; }
        rbuf = tmp;
        DWORD rd = 0;
        WinHttpReadData(hRequest, rbuf + total, avail, &rd);
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
