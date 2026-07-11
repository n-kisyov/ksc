#include "cloudsync.h"
#include "ksc_private.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dpapi.h>
#include <winhttp.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

#define GOOGLE_CLIENT_ID "175371281931-fjhj509cs3metn0588rttiia2oeis880.apps.googleusercontent.com"
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
        "client_id=%s&refresh_token=%s&grant_type=refresh_token",
        GOOGLE_CLIENT_ID, g_refreshToken);

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
static int ensure_ksc_folder(void)
{
    /* find existing */
    char url[512];
    sprintf(url,
        "%s/files?q=name='ksc-backups' and "
        "mimeType='application/vnd.google-apps.folder' and "
        "trashed=false&fields=files(id,name)",
        GOOGLE_DRIVE_API);

    char *resp = NULL;
    int status = 0;
    http_get_json(url, g_accessToken, &resp, &status);

    char *id = NULL;
    if (resp && status == 200) {
        /* look for "id" inside "files" */
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
                        id = g_folderId;
                    }
                }
            }
        }
        free(resp);
    }

    if (id) return 1;

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
    const char *files[4];
    int nFiles = 0;

    /* backup ksc.db */
    char src[300], localBak[300], remoteName[300];
    sprintf(src, "%s\\ksc.db", dir);
    sprintf(localBak, "%s\\ksc_backup_%s.db", dir, ts);
    if (CopyFile(src, localBak, FALSE)) {
        sprintf(remoteName, "ksc_backup_%s.db", ts);
        HANDLE hf = CreateFile(localBak, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            totalSize += GetFileSize(hf, NULL);
            CloseHandle(hf);
        }
        files[nFiles++] = "ksc.db";
    }

    /* backup keylog db */
    sprintf(src, "%s\\ksc_keylog.db", dir);
    sprintf(localBak, "%s\\ksc_keylog_backup_%s.db", dir, ts);
    if (CopyFile(src, localBak, TRUE)) {
        sprintf(remoteName, "ksc_keylog_backup_%s.db", ts);
        HANDLE hf = CreateFile(localBak, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            totalSize += GetFileSize(hf, NULL);
            CloseHandle(hf);
        }
        files[nFiles++] = "ksc_keylog.db";
    }

    if (nFiles == 0) goto done;

    /* upload to Drive */
    char fullName[320];
    for (int i = 0; i < nFiles; i++) {
        sprintf(localBak, "%s\\%s_backup_%s.db",
                dir, files[i], ts);
        sprintf(fullName, "%s_backup_%s.db", files[i], ts);

        char url[512];
        sprintf(url, "%s/upload/drive/v3/files?uploadType=multipart",
                GOOGLE_DRIVE_API);

        char *r = NULL;
        int st2 = 0;
        http_upload_file(url, g_accessToken, localBak, fullName,
                         g_folderId, &r, &st2);
        free(r);

        int pct = (int)(((i + 1) * 100LL) / nFiles);
        if (hCloudWnd)
            PostMessage(hCloudWnd, WM_CLOUD_SYNC, pct, 0);
    }

    /* save sync history */
    {
        char histPath[MAX_PATH];
        sprintf(histPath, "%s\\cloud_sync_history.json", dir);

        char filesStr[512] = "";
        for (int i = 0; i < nFiles; i++) {
            if (i > 0) strcat(filesStr, ", ");
            strcat(filesStr, files[i]);
        }

        char entry[512];
        sprintf(entry,
            "{\"ts\":\"%04d%02d%02dT%02d%02d%02d\","
            "\"files\":\"%s\",\"size\":%d,\"status\":\"ok\"}",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            filesStr, totalSize);

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
    if (g_loggedIn && g_userEmail[0])
        strncpy(buf, g_userEmail, bufsize - 1);
    else
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
    if (srv == INVALID_SOCKET) { WSACleanup(); return; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int port = 0;
    for (port = 49152; port < 65535; port++) {
        addr.sin_port = htons((u_short)port);
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
    }
    if (port >= 65535) { closesocket(srv); WSACleanup(); return; }
    listen(srv, 1);

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
        closesocket(srv); WSACleanup(); return;
    }

    SOCKET cli = accept(srv, NULL, NULL);
    closesocket(srv);

    char req[4096] = "";
    recv(cli, req, sizeof(req) - 1, 0);

    /* extract code */
    char *codeStart = strstr(req, "code=");
    if (!codeStart) { closesocket(cli); WSACleanup(); return; }
    codeStart += 5;
    char *codeEnd = strchr(codeStart, ' ');
    if (!codeEnd) codeEnd = strchr(codeStart, '\r');
    if (!codeEnd) codeEnd = strchr(codeStart, '&');
    if (!codeEnd) { closesocket(cli); WSACleanup(); return; }
    int codeLen = (int)(codeEnd - codeStart);
    if (codeLen <= 0 || codeLen > 1023) {
        closesocket(cli); WSACleanup(); return;
    }

    char code[1024];
    memcpy(code, codeStart, codeLen);
    code[codeLen] = '\0';

    /* send success page */
    char *page = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                 "Connection: close\r\n\r\n"
                 "<html><body><h3>Login successful.</h3>"
                 "<p>You may close this tab.</p></body></html>";
    send(cli, page, (int)strlen(page), 0);
    closesocket(cli);
    WSACleanup();

    /* exchange code for tokens */
    char body[1536];
    sprintf(body,
        "code=%s"
        "&client_id=%s"
        "&redirect_uri=http://127.0.0.1:%d/auth"
        "&grant_type=authorization_code",
        code, GOOGLE_CLIENT_ID, port);

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
    if (!g_loggedIn || g_backupInProgress) return;
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
        p += 4;
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
