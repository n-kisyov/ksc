#include <winsock2.h>
#include <ws2tcpip.h>
#include "cloudsync.h"
#include "ssh_sync.h"
#include "telegram.h"
#include "ksc_private.h"
#include <dpapi.h>
#include <winhttp.h>

#define GOOGLE_CLIENT_ID "175371281931-fjhj509cs3metn0588rttiia2oeis880.apps.googleusercontent.com"
#define GOOGLE_CLIENT_SECRET "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"
#define GOOGLE_AUTH_URI  "https://accounts.google.com/o/oauth2/v2/auth"
#define GOOGLE_TOKEN_URI "https://oauth2.googleapis.com/token"
#define GOOGLE_SCOPE     "https://www.googleapis.com/auth/drive.file"
#define GOOGLE_DRIVE_API "https://www.googleapis.com/drive/v3"
#define GOOGLE_USERINFO  "https://www.googleapis.com/oauth2/v1/userinfo?alt=json"

static char g_accessToken[1024];
static char g_refreshToken[256];
static char g_userEmail[128];
static char g_folderId[128];
static int  g_loggedIn = 0;
static int  g_backupInProgress = 0;

/* ---- JSON helpers (from json_micro.c) ---- */
extern char *json_find_str(const char *json, const char *key);
extern int   json_find_int(const char *json, const char *key, int def);

/* ---- HTTP helpers (from http_win.c) ---- */
extern int http_get_json(const char *url, const char *bearer,
                          char **response, int *status);
extern int http_post_form(const char *url, const char *bearer,
                           const char *body, char **response, int *status);
extern int http_post_json(const char *url, const char *bearer,
                           const char *jsonBody, char **response, int *status);
extern int http_upload_file(const char *url, const char *bearer,
                             const char *filePath, const char *remoteName,
                             const char *folderId,
                             char **response, int *status);

/* ---- Token storage ---- */
static void get_cloud_dir(char *buf, int bufsz)
{
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0)
        sprintf(buf, "%s\\KSC", appdata);
    else
        strcpy(buf, ".");
}

static void save_token(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    sprintf(path, "%s\\cloud_token.bin", dir);

    char tokenData[2048];
    sprintf(tokenData, "%s\n%s", g_refreshToken, g_userEmail);

    DATA_BLOB in = { (DWORD)strlen(tokenData) + 1,
                     (BYTE *)tokenData };
    DATA_BLOB out = {0};
    if (CryptProtectData(&in, L"ksc cloud token", NULL, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, out.pbData, out.cbData, &written, NULL);
            CloseHandle(h);
        }
        LocalFree(out.pbData);
    }
}

static int load_token(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    sprintf(path, "%s\\cloud_token.bin", dir);

    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD sz = GetFileSize(h, NULL);
    if (sz == 0 || sz > 8192) { CloseHandle(h); return 0; }
    BYTE *enc = malloc(sz);
    DWORD rd;
    ReadFile(h, enc, sz, &rd, NULL);
    CloseHandle(h);

    DATA_BLOB in  = { sz, enc };
    DATA_BLOB out = {0};
    int ok = 0;
    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                           CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        /* parse: refresh_token \n email */
        char *nl = strchr((char *)out.pbData, '\n');
        if (nl) {
            *nl = '\0';
            strncpy(g_refreshToken, (char *)out.pbData, 255);
            g_refreshToken[255] = '\0';
            strncpy(g_userEmail, nl + 1, 127);
            g_userEmail[127] = '\0';
            ok = 1;
        }
        LocalFree(out.pbData);
    }
    free(enc);
    return ok;
}

static void delete_token_file(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    sprintf(path, "%s\\cloud_token.bin", dir);
    DeleteFile(path);
}

/* ---- Token refresh ---- */
static int refresh_access_token(void)
{
    char body[1024];
    sprintf(body,
        "client_id=%s&client_secret=%s"
        "&refresh_token=%s&grant_type=refresh_token",
        GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, g_refreshToken);

    char *resp = NULL;
    int status = 0;
    if (!http_post_form(GOOGLE_TOKEN_URI, NULL, body, &resp, &status))
        return 0;
    if (status != 200 || !resp) { free(resp); return 0; }

    char *tok = json_find_str(resp, "access_token");
    if (!tok) { free(resp); return 0; }
    strncpy(g_accessToken, tok, 1023);
    g_accessToken[1023] = '\0';
    free(resp);
    return 1;
}

/* ---- Drive folder management ---- */
static void save_folder_id(void)
{
    char path[MAX_PATH];
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0)
        sprintf(path, "%s\\KSC\\cloud_folder_id", appdata);
    else
        return;
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s", g_folderId); fclose(f); }
}

static void load_folder_id(void)
{
    char path[MAX_PATH];
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) <= 0)
        return;
    sprintf(path, "%s\\KSC\\cloud_folder_id", appdata);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(g_folderId, sizeof(g_folderId), f)) {
            char *nl = strchr(g_folderId, '\n');
            if (nl) *nl = '\0';
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
}

static int ensure_ksc_folder(void)
{
    /* cached in memory */
    if (g_folderId[0]) return 1;

    /* load from disk */
    load_folder_id();
    if (g_folderId[0]) return 1;

    /* search Drive */
    char url[1024];
    sprintf(url,
        "%s/files?q=name='ksc-backups'+and+"
        "mimeType='application/vnd.google-apps.folder'+and+"
        "trashed=false"
        "&fields=files(id,name)",
        GOOGLE_DRIVE_API);

    char *resp = NULL;
    int status = 0;
    http_get_json(url, g_accessToken, &resp, &status);

    if (resp && status == 200) {
        char *start = strstr(resp, "\"id\"");
        if (start) {
            start = strstr(start + 4, "\"");
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = (int)(end - start);
                    if (len > 0 && len < 127) {
                        memcpy(g_folderId, start, len);
                        g_folderId[len] = '\0';
                    }
                }
            }
        }
        free(resp);
    }

    if (g_folderId[0]) { save_folder_id(); return 1; }

    /* create folder */
    char body[256];
    sprintf(body,
        "{\"name\":\"ksc-backups\","
        "\"mimeType\":\"application/vnd.google-apps.folder\"}");

    char *resp2 = NULL;
    int st2 = 0;
    http_post_json(GOOGLE_DRIVE_API "/files", g_accessToken,
                    body, &resp2, &st2);
    if (resp2 && st2 == 200) {
        char *id2 = json_find_str(resp2, "id");
        if (id2) {
            strncpy(g_folderId, id2, 127);
            g_folderId[127] = '\0';
            save_folder_id();
            free(resp2);
            return 1;
        }
        free(resp2);
    }
    return 0;
}

/* ---- Backup & Upload thread ---- */
static DWORD WINAPI cloudsync_backup_thread(LPVOID param)
{
    HWND hCloudWnd = (HWND)param;
    g_backupInProgress = 1;

    if (!g_loggedIn) goto done;
    if (!refresh_access_token()) goto done;
    if (!ensure_ksc_folder()) goto done;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char dir[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    char ts[32];
    sprintf(ts, "%04d%02d%02d_%02d%02d%02d",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);

    int totalSize = 0;
    int fileSizes[4] = {0};
    char bakNames[4][128];
    int nFiles = 0;

    /* backup ksc.db */
    char src[300], localBak[300];
    sprintf(src, "%s\\ksc.db", dir);
    sprintf(bakNames[nFiles], "ksc_backup_%s.db", ts);
    sprintf(localBak, "%s\\%s", dir, bakNames[nFiles]);
    if (CopyFile(src, localBak, FALSE)) {
        HANDLE hf = CreateFile(localBak, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            int sz = (int)GetFileSize(hf, NULL);
            totalSize += sz;
            fileSizes[nFiles] = sz;
            CloseHandle(hf);
        }
        nFiles++;
    }

    /* backup keylog db */
    sprintf(src, "%s\\ksc_keylog.db", dir);
    sprintf(bakNames[nFiles], "ksc_keylog_backup_%s.db", ts);
    sprintf(localBak, "%s\\%s", dir, bakNames[nFiles]);
    if (CopyFile(src, localBak, TRUE)) {
        HANDLE hf = CreateFile(localBak, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            int sz = (int)GetFileSize(hf, NULL);
            totalSize += sz;
            fileSizes[nFiles] = sz;
            CloseHandle(hf);
        }
        nFiles++;
    }

    if (nFiles == 0) goto done;

    int driveConfigured = g_loggedIn ? 1 : 0;
    int sshConfigured = ssh_sync_is_configured();
    int driveOK = !driveConfigured; /* trivially OK if not configured */
    int sshOK   = !sshConfigured;

    /* upload to Google Drive */
    if (driveConfigured) {
        if (!refresh_access_token()) goto skip_drive;
        if (!ensure_ksc_folder())   goto skip_drive;

        int ok = 1;
        for (int i = 0; i < nFiles; i++) {
            sprintf(localBak, "%s\\%s", dir, bakNames[i]);
            char url[512];
            sprintf(url,
                "https://www.googleapis.com/upload/drive/v3/files"
                "?uploadType=multipart");
            char *r = NULL;
            int st2 = 0;
            http_upload_file(url, g_accessToken, localBak,
                             bakNames[i], g_folderId, &r, &st2);
            if (st2 != 200) ok = 0;
            free(r);
            int pct = (int)(((i + 1) * 100LL) / nFiles);
            if (hCloudWnd)
                PostMessage(hCloudWnd, WM_CLOUD_SYNC, pct, 0);
        }
        driveOK = ok;
    }

    /* upload to SSH */
    skip_drive:
    if (sshConfigured) {
        int ok = 1;
        for (int i = 0; i < nFiles; i++) {
            sprintf(localBak, "%s\\%s", dir, bakNames[i]);
            if (!ssh_sync_upload(localBak, bakNames[i]))
                ok = 0;
        }
        sshOK = ok;
    }

    /* delete local files only if all configured targets succeeded */
    if (driveOK && sshOK) {
        for (int i = 0; i < nFiles; i++) {
            sprintf(localBak, "%s\\%s", dir, bakNames[i]);
            DeleteFile(localBak);
        }
    }

    /* build status string */
    char statusStr[32] = "";
    if (driveConfigured)
        strcat(statusStr, driveOK ? "Drive" : "Drive(fail)");
    if (driveConfigured && sshConfigured)
        strcat(statusStr, "+");
    if (sshConfigured)
        strcat(statusStr, sshOK ? "SSH" : "SSH(fail)");
    if (statusStr[0] == '\0')
        strcpy(statusStr, "none");

    char filesStr[512] = "";

    /* save sync history */
    {
        char histPath[MAX_PATH];
        sprintf(histPath, "%s\\cloud_sync_history.json", dir);

        for (int i = 0; i < nFiles; i++) {
            if (i > 0) strcat(filesStr, ", ");
            strcat(filesStr, bakNames[i]);
        }

        char entry[512];
        sprintf(entry,
            "{\"ts\":\"%04d%02d%02dT%02d%02d%02d\","
            "\"files\":\"%s\",\"size\":%d,\"status\":\"%s\"}",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            filesStr, totalSize, statusStr);

        HANDLE hf = CreateFile(histPath, GENERIC_READ, 0, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        char *existing = NULL;
        DWORD existingSize = 0;
        if (hf != INVALID_HANDLE_VALUE) {
            existingSize = GetFileSize(hf, NULL);
            if (existingSize > 0 && existingSize < 65536) {
                existing = malloc(existingSize + 1);
                DWORD rd2 = 0;
                ReadFile(hf, existing, existingSize, &rd2, NULL);
                existing[rd2] = '\0';
            }
            CloseHandle(hf);
        }

        hf = CreateFile(histPath, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD w2 = 0;
            WriteFile(hf, "[\r\n", 3, &w2, NULL);
            WriteFile(hf, entry, (DWORD)strlen(entry), &w2, NULL);
            if (existing && existing[0] == '[') {
                char *start = strchr(existing + 1, '{');
                if (start) {
                    WriteFile(hf, ",\r\n", 3, &w2, NULL);
                    WriteFile(hf, start,
                              (DWORD)(existingSize - (start - existing)),
                              &w2, NULL);
                }
            }
            WriteFile(hf, "\r\n]\r\n", 5, &w2, NULL);
            CloseHandle(hf);
        }
        free(existing);
    }

    /* send Telegram notification if enabled */
    {
        extern int db_get_setting_int(const char *k, int d);
        if (db_get_setting_int("tg_enabled", 0)) {
            int allOK = (driveOK && sshOK);
            int mode = db_get_setting_int("tg_notify_mode", 0);
            if (mode == 0 || (mode == 1 && !allOK)) {
                char msg[512];
                char filesDetail[512] = "";
                for (int i = 0; i < nFiles; i++) {
                    if (i > 0) strcat(filesDetail, "\n");
                    sprintf(filesDetail + strlen(filesDetail),
                        "%s (%.2f MB)",
                        bakNames[i], fileSizes[i] / (1024.0 * 1024.0));
                }
                sprintf(msg, "%s ksc sync %s: %s\n%s",
                    allOK ? "\xE2\x9C\x85" : "\xE2\x9D\x8C",
                    allOK ? "OK" : "FAILED",
                    statusStr, filesDetail);
                telegram_send(msg);
            }
        }
    }

    if (hCloudWnd)
        PostMessage(hCloudWnd, WM_CLOUD_SYNC, 100, 0);

done:
    g_backupInProgress = 0;
    return 0;
}

/* ---- Public API ---- */
int cloudsync_is_logged_in(void) { return g_loggedIn; }

void cloudsync_get_email(char *buf, int bufsize)
{
    if (g_loggedIn) {
        if (g_userEmail[0])
            strncpy(buf, g_userEmail, bufsize - 1);
        else
            strncpy(buf, "Logged in", bufsize - 1);
    } else
        strcpy(buf, "Not logged in");
    buf[bufsize - 1] = '\0';
}

void cloudsync_login(HWND hParent)
{
    (void)hParent;
    if (g_loggedIn) return;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int port = 0;
    for (port = 49152; port < 65535; port++) {
        addr.sin_port = htons((u_short)port);
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
    }
    if (port >= 65535) { closesocket(srv); return; }
    listen(srv, 1);

    /* PKCE: generate code_verifier and code_challenge */
    char codeVerifier[128] = "";
    char codeChallenge[128] = "";
    {
        /* PKCE: standard approach per RFC 7636
           32 random bytes -> base64url (no padding) -> code_verifier
           SHA256(code_verifier) -> base64url (no padding) -> code_challenge */
        BYTE rnd[32];
        HCRYPTPROV hProv = 0;
        if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL,
                                 CRYPT_VERIFYCONTEXT)) {
            CryptGenRandom(hProv, sizeof(rnd), rnd);
            CryptReleaseContext(hProv, 0);
        } else {
            srand(GetTickCount());
            for (int i = 0; i < 32; i++)
                rnd[i] = (BYTE)(rand() % 256);
        }

        /* base64url encode random bytes -> code_verifier */
        const char *b64u =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        {
            int cp = 0;
            for (int i = 0; i < 32; i += 3) {
                int rem = 32 - i;
                DWORD val = (rnd[i] << 16);
                if (rem >= 2) val |= (rnd[i + 1] << 8);
                if (rem >= 3) val |= rnd[i + 2];
                codeVerifier[cp++] = b64u[(val >> 18) & 0x3F];
                codeVerifier[cp++] = b64u[(val >> 12) & 0x3F];
                if (rem >= 2) codeVerifier[cp++] = b64u[(val >> 6) & 0x3F];
                if (rem >= 3) codeVerifier[cp++] = b64u[val & 0x3F];
            }
            codeVerifier[cp] = '\0';
        }

        /* SHA256 of code_verifier -> base64url encode -> code_challenge */
        HCRYPTPROV hProv2 = 0;
        HCRYPTHASH hHash = 0;
        if (CryptAcquireContext(&hProv2, NULL, NULL, PROV_RSA_FULL,
                                 CRYPT_VERIFYCONTEXT) &&
            CryptCreateHash(hProv2, CALG_SHA_256, 0, 0, &hHash)) {
            CryptHashData(hHash, (BYTE *)codeVerifier,
                          (DWORD)strlen(codeVerifier), 0);
            BYTE hash[32];
            DWORD hashLen = 32;
            CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv2, 0);

            int cp = 0;
            for (int i = 0; i < 32; i += 3) {
                int rem = 32 - i;
                DWORD val = (hash[i] << 16);
                if (rem >= 2) val |= (hash[i + 1] << 8);
                if (rem >= 3) val |= hash[i + 2];
                codeChallenge[cp++] = b64u[(val >> 18) & 0x3F];
                codeChallenge[cp++] = b64u[(val >> 12) & 0x3F];
                if (rem >= 2)
                    codeChallenge[cp++] = b64u[(val >> 6) & 0x3F];
                if (rem >= 3)
                    codeChallenge[cp++] = b64u[val & 0x3F];
            }
            codeChallenge[cp] = '\0';
        }
    }

    char authUrl[1024];
    sprintf(authUrl,
        "%s?client_id=%s"
        "&redirect_uri=http://127.0.0.1:%d/auth"
        "&response_type=code"
        "&scope=%s"
        "&access_type=offline"
        "&prompt=consent",
        GOOGLE_AUTH_URI, GOOGLE_CLIENT_ID, port, GOOGLE_SCOPE);
    ShellExecute(NULL, "open", authUrl, NULL, NULL, SW_SHOW);

    /* timeout: 120 seconds */
    fd_set fds;
    struct timeval tv = {120, 0};
    FD_ZERO(&fds);
    FD_SET(srv, &fds);
    if (select(0, &fds, NULL, NULL, &tv) <= 0) {
        closesocket(srv); return;
    }

    SOCKET cli = accept(srv, NULL, NULL);
    closesocket(srv);

    char req[4096] = "";
    recv(cli, req, sizeof(req) - 1, 0);

    /* extract code */
    char *codeStart = strstr(req, "code=");
    if (!codeStart) { closesocket(cli); return; }
    codeStart += 5;
    char *codeEnd = strchr(codeStart, '&');
    if (!codeEnd) codeEnd = strchr(codeStart, ' ');
    if (!codeEnd) codeEnd = strchr(codeStart, '\r');
    if (!codeEnd) codeEnd = strchr(codeStart, '\n');
    if (!codeEnd) { closesocket(cli); return; }
    int codeLen = (int)(codeEnd - codeStart);
    if (codeLen <= 0 || codeLen > 1023) {
        closesocket(cli); return;
    }

    char code[1024];
    memcpy(code, codeStart, codeLen);
    code[codeLen] = '\0';

    /* URL-encode the code for form POST */
    char encCode[2048];
    int ep = 0;
    for (int i = 0; code[i] && ep < 2040; i++) {
        unsigned char c = (unsigned char)code[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            encCode[ep++] = c;
        } else {
            ep += sprintf(encCode + ep, "%%%02X", c);
        }
    }
    encCode[ep] = '\0';

    /* send success page */
    char *page = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                 "Connection: close\r\n\r\n"
                 "<html><body><h3>Login successful.</h3>"
                 "<p>You may close this tab.</p></body></html>";
    send(cli, page, (int)strlen(page), 0);
    closesocket(cli);

    /* exchange code for tokens */
    char body[1536];
    sprintf(body,
        "code=%s"
        "&client_id=%s"
        "&client_secret=%s"
        "&redirect_uri=http://127.0.0.1:%d/auth"
        "&grant_type=authorization_code",
        encCode, GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, port);

    char *resp = NULL;
    int status = 0;
    if (!http_post_form(GOOGLE_TOKEN_URI, NULL, body, &resp, &status))
        return;
    if (status != 200 || !resp) { free(resp); return; }

    char *at = json_find_str(resp, "access_token");
    char *rt = json_find_str(resp, "refresh_token");
    if (!at || !rt) { free(resp); return; }

    strncpy(g_accessToken, at, 1023);
    g_accessToken[1023] = '\0';
    strncpy(g_refreshToken, rt, 255);
    g_refreshToken[255] = '\0';
    free(resp);

    /* get user email */
    char *uresp = NULL;
    int ust = 0;
    http_get_json(GOOGLE_USERINFO, g_accessToken, &uresp, &ust);
    if (uresp && ust == 200) {
        char *em = json_find_str(uresp, "email");
        if (em) {
            strncpy(g_userEmail, em, 127);
            g_userEmail[127] = '\0';
        }
        free(uresp);
    }

    g_loggedIn = 1;
    save_token();
}

void cloudsync_logout(void)
{
    g_loggedIn = 0;
    g_accessToken[0] = '\0';
    g_refreshToken[0] = '\0';
    g_userEmail[0] = '\0';
    g_folderId[0] = '\0';
    delete_token_file();
}

void cloudsync_backup_trigger(HWND hCloudWnd)
{
    if ((!g_loggedIn && !ssh_sync_is_configured()) || g_backupInProgress)
        return;
    CreateThread(NULL, 0, cloudsync_backup_thread,
                  (LPVOID)hCloudWnd, 0, NULL);
}

int cloudsync_load_history(CloudSyncEntry **out, int *pcount)
{
    *out = NULL;
    *pcount = 0;

    char path[MAX_PATH];
    get_cloud_dir(path, sizeof(path));
    strcat(path, "\\cloud_sync_history.json");

    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD sz = GetFileSize(h, NULL);
    if (sz == 0 || sz > 65536) { CloseHandle(h); return 0; }
    char *buf = malloc(sz + 1);
    DWORD rd;
    ReadFile(h, buf, sz, &rd, NULL);
    buf[rd] = '\0';
    CloseHandle(h);

    int cap = 32, cnt = 0;
    CloudSyncEntry *entries = malloc(sizeof(CloudSyncEntry) * cap);
    if (!entries) { free(buf); return 0; }

    char *p = buf;
    while ((p = strstr(p, "\"ts\"")) != NULL) {
        if (cnt >= cap) {
            cap *= 2;
            CloudSyncEntry *tmp = realloc(entries,
                sizeof(CloudSyncEntry) * cap);
            if (!tmp) { free(entries); free(buf); return 0; }
            entries = tmp;
        }
        CloudSyncEntry *e = &entries[cnt];
        memset(e, 0, sizeof(*e));
        {
            char *tsv = json_find_str(p, "ts");
            if (tsv) strncpy(e->ts, tsv, 31);
        }
        {
            char *fv = json_find_str(p, "files");
            if (fv) strncpy(e->files, fv, 255);
        }
        e->size = json_find_int(p, "size", 0);
        {
            char *sv = json_find_str(p, "status");
            if (sv) strncpy(e->status, sv, 15);
        }
        cnt++;
        p = strchr(p, '}');
        if (p) p++;
    }

    free(buf);
    *out = entries;
    *pcount = cnt;
    return 1;
}

void cloudsync_free_history(CloudSyncEntry *e) { free(e); }

int cloudsync_get_schedule(void)
{
    extern int db_get_setting_int(const char *k, int d);
    return db_get_setting_int("cloud_schedule", 0);
}

void cloudsync_set_schedule(int value)
{
    extern void db_set_setting_int(const char *k, int v);
    db_set_setting_int("cloud_schedule", value);
}

/* called at startup */
void cloudsync_init(void)
{
    if (load_token()) {
        g_loggedIn = 1;
        /* attempt a token refresh to validate */
        refresh_access_token();
    }
}
