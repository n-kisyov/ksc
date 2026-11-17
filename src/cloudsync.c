#include <winsock2.h>
#include <ws2tcpip.h>
#include "cloudsync.h"
#include "ssh_sync.h"
#include "telegram.h"
#include "database.h"
#include "keylogdb.h"
#include "zip_util.h"
#include "ksc_private.h"
#include <dpapi.h>
#include <wincrypt.h>
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
static bool g_loggedIn = false;
static SRWLOCK g_cloudLock = SRWLOCK_INIT;
static volatile LONG g_backupInProgress = 0;
static volatile LONG g_loginInProgress = 0;

/* ---- JSON helpers (from json_micro.c) ---- */
extern bool json_find_str_buf(const char *json, const char *key,
                              char *out, int out_sz);
extern int  json_find_int(const char *json, const char *key, int def);

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
extern int http_delete(const char *url, const char *bearer, int *status);

static int target_result(int configured, int attempted, int ok)
{
    return !configured || (attempted && ok);
}

/* ---- Token storage ---- */
static void get_cloud_dir(char *buf, int bufsz)
{
    db_get_db_dir(buf, bufsz);
}

static void save_token_values(const char *refreshTok, const char *email)
{
    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s\\cloud_token.bin", dir);

    char tokenData[2048];
    snprintf(tokenData, sizeof(tokenData), "%s\n%s",
             refreshTok ? refreshTok : "", email ? email : "");

    DATA_BLOB in = { (DWORD)strlen(tokenData) + 1,
                     (BYTE *)tokenData };
    DATA_BLOB out = {};
    if (CryptProtectData(&in, L"ksc cloud token", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        HANDLE h = CreateFile(path, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, out.pbData, out.cbData, &written, nullptr);
            CloseHandle(h);
        }
        LocalFree(out.pbData);
    }
    SecureZeroMemory(tokenData, sizeof(tokenData));
}

static int load_token_values(char *outRefresh, size_t rtSz,
                             char *outEmail, size_t emSz)
{
    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s\\cloud_token.bin", dir);

    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD sz = GetFileSize(h, nullptr);
    if (sz == 0 || sz > 8192) { CloseHandle(h); return 0; }
    BYTE *enc = malloc(sz);
    if (!enc) { CloseHandle(h); return 0; }
    DWORD rd = 0;
    if (!ReadFile(h, enc, sz, &rd, nullptr) || rd == 0) {
        free(enc);
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);

    DATA_BLOB in  = { rd, enc };
    DATA_BLOB out = {};
    int ok = 0;
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        char *nl = strchr((char *)out.pbData, '\n');
        if (nl) {
            *nl = '\0';
            strncpy(outRefresh, (char *)out.pbData, rtSz - 1);
            outRefresh[rtSz - 1] = '\0';
            strncpy(outEmail, nl + 1, emSz - 1);
            outEmail[emSz - 1] = '\0';
            ok = outRefresh[0] != '\0';
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
    snprintf(path, sizeof(path), "%s\\cloud_token.bin", dir);
    DeleteFile(path);
}

/* ---- Token refresh ---- */
static int refresh_access_token(char *outAccess, size_t outSz)
{
    if (outAccess && outSz > 0) outAccess[0] = '\0';

    char rt[256] = "";
    AcquireSRWLockShared(&g_cloudLock);
    snprintf(rt, sizeof(rt), "%s", g_refreshToken);
    ReleaseSRWLockShared(&g_cloudLock);
    if (!rt[0]) return 0;

    char body[1024];
    snprintf(body, sizeof(body),
        "client_id=%s&client_secret=%s"
        "&refresh_token=%s&grant_type=refresh_token",
        GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, rt);

    char *resp = nullptr;
    int status = 0;
    if (!http_post_form(GOOGLE_TOKEN_URI, nullptr, body, &resp, &status))
        return 0;
    if (status != 200 || !resp) { free(resp); return 0; }

    char at[1024] = "";
    bool ok = json_find_str_buf(resp, "access_token", at, sizeof(at)) &&
              at[0] != '\0';
    free(resp);
    if (!ok) return 0;

    AcquireSRWLockExclusive(&g_cloudLock);
    snprintf(g_accessToken, sizeof(g_accessToken), "%s", at);
    ReleaseSRWLockExclusive(&g_cloudLock);

    if (outAccess && outSz > 0) snprintf(outAccess, outSz, "%s", at);
    return 1;
}

/* ---- Drive folder management ---- */
static void save_folder_id_value(const char *fid)
{
    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s\\cloud_folder_id", dir);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s", fid ? fid : ""); fclose(f); }
}

static void load_folder_id_buf(char *fid, size_t sz)
{
    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s\\cloud_folder_id", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(fid, (int)sz, f)) {
            char *nl = strchr(fid, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
}

static int ensure_ksc_folder(const char *accessToken, char *outFid, size_t outSz)
{
    char fid[128] = "";
    AcquireSRWLockExclusive(&g_cloudLock);
    if (!g_folderId[0]) {
        load_folder_id_buf(g_folderId, sizeof(g_folderId));
    }
    snprintf(fid, sizeof(fid), "%s", g_folderId);
    ReleaseSRWLockExclusive(&g_cloudLock);

    if (fid[0]) {
        char url[1024];
        snprintf(url, sizeof(url), "%s/files/%s?fields=id,trashed",
                 GOOGLE_DRIVE_API, fid);
        char *resp = nullptr;
        int status = 0;
        http_get_json(url, accessToken, &resp, &status);

        int valid = 0;
        if (resp && status == 200) {
            if (!strstr(resp, "\"trashed\": true") &&
                !strstr(resp, "\"trashed\":true")) {
                valid = 1;
            }
        }
        free(resp);

        if (valid) {
            snprintf(outFid, outSz, "%s", fid);
            return 1;
        }

        if (status == 404 || status == 200) {
            fid[0] = '\0';
            AcquireSRWLockExclusive(&g_cloudLock);
            g_folderId[0] = '\0';
            ReleaseSRWLockExclusive(&g_cloudLock);
            save_folder_id_value("");
        } else {
            snprintf(outFid, outSz, "%s", fid);
            return 1;
        }
    }

    char url[1024];
    snprintf(url, sizeof(url),
        "%s/files?q=name='ksc-backups'+and+"
        "mimeType='application/vnd.google-apps.folder'+and+"
        "trashed=false"
        "&fields=files(id,name)",
        GOOGLE_DRIVE_API);

    char *resp = nullptr;
    int status = 0;
    http_get_json(url, accessToken, &resp, &status);

    if (resp && status == 200) {
        json_find_str_buf(resp, "id", fid, sizeof(fid));
    }
    free(resp);

    if (fid[0]) {
        AcquireSRWLockExclusive(&g_cloudLock);
        snprintf(g_folderId, sizeof(g_folderId), "%s", fid);
        ReleaseSRWLockExclusive(&g_cloudLock);
        save_folder_id_value(fid);
        snprintf(outFid, outSz, "%s", fid);
        return 1;
    }

    char body[256];
    snprintf(body, sizeof(body),
        "{\"name\":\"ksc-backups\","
        "\"mimeType\":\"application/vnd.google-apps.folder\"}");

    char *resp2 = nullptr;
    int st2 = 0;
    http_post_json(GOOGLE_DRIVE_API "/files", accessToken,
                    body, &resp2, &st2);
    if (resp2 && st2 == 200) {
        if (json_find_str_buf(resp2, "id", fid, sizeof(fid)) && fid[0]) {
            AcquireSRWLockExclusive(&g_cloudLock);
            snprintf(g_folderId, sizeof(g_folderId), "%s", fid);
            ReleaseSRWLockExclusive(&g_cloudLock);
            save_folder_id_value(fid);
            snprintf(outFid, outSz, "%s", fid);
            free(resp2);
            return 1;
        }
    }
    free(resp2);
    return 0;
}

typedef struct {
    char id[128];
    char name[128];
} DriveFileItem;

static int desc_drive_item_cmp(const void *a, const void *b)
{
    const DriveFileItem *da = (const DriveFileItem *)a;
    const DriveFileItem *db = (const DriveFileItem *)b;
    return strcmp(db->name, da->name);
}

static void prune_drive_group(DriveFileItem *items, int count, int keepCount,
                              const char *accessToken)
{
    if (!items || count <= keepCount) return;
    qsort(items, (size_t)count, sizeof(items[0]), desc_drive_item_cmp);
    for (int i = keepCount; i < count; i++) {
        char delUrl[512];
        int st = 0;
        snprintf(delUrl, sizeof(delUrl), "%s/files/%s",
                 GOOGLE_DRIVE_API, items[i].id);
        http_delete(delUrl, accessToken, &st);
    }
}

static void drive_prune_old_backups(int keepCount, const char *accessToken,
                                    const char *folderId)
{
    if (keepCount <= 0 || !accessToken || !accessToken[0] ||
        !folderId || !folderId[0]) {
        return;
    }

    char url[1024];
    snprintf(url, sizeof(url),
        "%s/files?q='%s'+in+parents+and+trashed=false"
        "&orderBy=name+desc&pageSize=200&fields=files(id,name)",
        GOOGLE_DRIVE_API, folderId);

    char *resp = nullptr;
    int status = 0;
    if (!http_get_json(url, accessToken, &resp, &status) ||
        status != 200 || !resp) {
        free(resp);
        return;
    }

    const char *arr = strstr(resp, "\"files\"");
    if (!arr) { free(resp); return; }
    arr = strchr(arr, '[');
    if (!arr) { free(resp); return; }

    DriveFileItem mainList[200];
    DriveFileItem klogList[200];
    int mainCount = 0, klogCount = 0;

    const char *p = arr;
    while ((p = strchr(p, '{')) != nullptr) {
        const char *end = strchr(p, '}');
        if (!end) break;
        size_t objLen = (size_t)(end - p + 1);
        if (objLen < 512) {
            char obj[512];
            memcpy(obj, p, objLen);
            obj[objLen] = '\0';
            DriveFileItem item = {};
            if (json_find_str_buf(obj, "id", item.id, sizeof(item.id)) &&
                json_find_str_buf(obj, "name", item.name, sizeof(item.name)) &&
                item.id[0] && item.name[0]) {
                if (strncmp(item.name, "ksc_backup_", 11) == 0 &&
                    mainCount < 200) {
                    mainList[mainCount++] = item;
                } else if (strncmp(item.name, "ksc_keylog_backup_", 18) == 0 &&
                           klogCount < 200) {
                    klogList[klogCount++] = item;
                }
            }
        }
        p = end + 1;
    }
    free(resp);

    prune_drive_group(mainList, mainCount, keepCount, accessToken);
    prune_drive_group(klogList, klogCount, keepCount, accessToken);
}

static void write_json_str(HANDLE hf, const char *s)
{
    DWORD w = 0;
    WriteFile(hf, "\"", 1, &w, nullptr);
    if (s) {
        for (size_t i = 0; s[i]; i++) {
            char c = s[i];
            if (c == '\\' || c == '"') {
                char esc[2] = {'\\', c};
                WriteFile(hf, esc, 2, &w, nullptr);
            } else if ((unsigned char)c >= 0x20) {
                WriteFile(hf, &c, 1, &w, nullptr);
            }
        }
    }
    WriteFile(hf, "\"", 1, &w, nullptr);
}

static void save_sync_history_entry(const char *dir, const SYSTEMTIME *st,
                                    const char *filesStr, int totalSize,
                                    const char *statusStr)
{
    CloudSyncEntry *prev = nullptr;
    int prevCount = 0;
    if (!cloudsync_load_history(&prev, &prevCount)) {
        prev = nullptr;
        prevCount = 0;
    }

    char histPath[MAX_PATH];
    snprintf(histPath, sizeof(histPath), "%s\\cloud_sync_history.json", dir);
    HANDLE hf = CreateFile(histPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        char newTs[32];
        snprintf(newTs, sizeof(newTs), "%04d%02d%02dT%02d%02d%02d",
                 st->wYear, st->wMonth, st->wDay,
                 st->wHour, st->wMinute, st->wSecond);

        WriteFile(hf, "[\r\n", 3, &w, nullptr);

        WriteFile(hf, "{\"ts\":", 6, &w, nullptr);
        write_json_str(hf, newTs);
        WriteFile(hf, ",\"files\":", 9, &w, nullptr);
        write_json_str(hf, filesStr);
        char mid[64];
        snprintf(mid, sizeof(mid), ",\"size\":%d,\"status\":", totalSize);
        WriteFile(hf, mid, (DWORD)strlen(mid), &w, nullptr);
        write_json_str(hf, statusStr);
        WriteFile(hf, "}", 1, &w, nullptr);

        int keepPrev = (prevCount < 199) ? prevCount : 199;
        for (int i = 0; i < keepPrev; i++) {
            WriteFile(hf, ",\r\n{\"ts\":", 9, &w, nullptr);
            write_json_str(hf, prev[i].ts);
            WriteFile(hf, ",\"files\":", 9, &w, nullptr);
            write_json_str(hf, prev[i].files);
            snprintf(mid, sizeof(mid), ",\"size\":%d,\"status\":", prev[i].size);
            WriteFile(hf, mid, (DWORD)strlen(mid), &w, nullptr);
            write_json_str(hf, prev[i].status);
            WriteFile(hf, "}", 1, &w, nullptr);
        }

        WriteFile(hf, "\r\n]\r\n", 5, &w, nullptr);
        CloseHandle(hf);
    }
    cloudsync_free_history(prev);
}

/* ---- Backup & Upload thread ---- */
static DWORD WINAPI cloudsync_backup_thread(LPVOID param)
{
    HWND hCloudWnd = (HWND)param;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char dir[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    int totalSize = 0;
    int fileSizes[4] = {0};
    char bakNames[4][128];
    int nFiles = 0;

    /* backup ksc.db (WAL-safe) */
    char localBak[MAX_PATH];
    snprintf(bakNames[nFiles], sizeof(bakNames[nFiles]), "ksc_backup_%s.db", ts);
    snprintf(localBak, sizeof(localBak), "%s\\%s", dir, bakNames[nFiles]);
    if (db_backup_safe(localBak)) {
        nFiles++;
    }

    /* backup keylog db (WAL-safe) */
    snprintf(bakNames[nFiles], sizeof(bakNames[nFiles]),
             "ksc_keylog_backup_%s.db", ts);
    snprintf(localBak, sizeof(localBak), "%s\\%s", dir, bakNames[nFiles]);
    if (keylog_backup_safe(localBak)) {
        nFiles++;
    }

    if (nFiles == 0) {
        if (hCloudWnd && IsWindow(hCloudWnd))
            PostMessage(hCloudWnd, WM_CLOUD_SYNC, 100, -1);
        goto done;
    }

    /* Compress each database backup into its own .zip file when enabled */
    if (cloudsync_get_compress()) {
        for (int i = 0; i < nFiles; i++) {
            char dbBakPath[MAX_PATH], zipName[128], zipPath[MAX_PATH];
            snprintf(dbBakPath, sizeof(dbBakPath), "%s\\%s", dir, bakNames[i]);
            strncpy(zipName, bakNames[i], sizeof(zipName) - 1);
            zipName[sizeof(zipName) - 1] = '\0';
            char *ext = strrchr(zipName, '.');
            if (ext && (size_t)(ext - zipName) + 5 <= sizeof(zipName)) {
                strcpy(ext, ".zip");
            } else {
                strncat(zipName, ".zip",
                        sizeof(zipName) - strlen(zipName) - 1);
            }
            snprintf(zipPath, sizeof(zipPath), "%s\\%s", dir, zipName);
            if (zip_compress_file(dbBakPath, zipPath, bakNames[i])) {
                DeleteFile(dbBakPath);
                strncpy(bakNames[i], zipName, sizeof(bakNames[i]) - 1);
                bakNames[i][sizeof(bakNames[i]) - 1] = '\0';
            }
        }
    }

    for (int i = 0; i < nFiles; i++) {
        snprintf(localBak, sizeof(localBak), "%s\\%s", dir, bakNames[i]);
        HANDLE hf = CreateFile(localBak, GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            int sz = (int)GetFileSize(hf, nullptr);
            totalSize += sz;
            fileSizes[i] = sz;
            CloseHandle(hf);
        }
    }

    int driveConfigured = cloudsync_is_logged_in() ? 1 : 0;
    int sshConfigured = ssh_sync_is_configured() ? 1 : 0;
    int driveAttempted = 0, sshAttempted = 0;
    int driveOK = 0, sshOK = 0;
    char accessTokenSnap[1024] = "";
    char folderIdSnap[128] = "";

    /* upload to Google Drive */
    if (driveConfigured) {
        driveAttempted = 1;
        if (!refresh_access_token(accessTokenSnap, sizeof(accessTokenSnap)))
            goto skip_drive;
        if (!ensure_ksc_folder(accessTokenSnap, folderIdSnap, sizeof(folderIdSnap)))
            goto skip_drive;

        int ok = 1;
        for (int i = 0; i < nFiles; i++) {
            snprintf(localBak, sizeof(localBak), "%s\\%s", dir, bakNames[i]);
            const char *url =
                "https://www.googleapis.com/upload/drive/v3/files"
                "?uploadType=multipart";
            char *r = nullptr;
            int st2 = 0;
            http_upload_file(url, accessTokenSnap, localBak,
                             bakNames[i], folderIdSnap, &r, &st2);
            if (st2 != 200) ok = 0;
            free(r);
            int pct = (int)(((i + 1) * 100LL) / nFiles);
            if (hCloudWnd && IsWindow(hCloudWnd))
                PostMessage(hCloudWnd, WM_CLOUD_SYNC, (WPARAM)pct, 1);
        }
        driveOK = ok;
    }

    /* upload to SSH */
    skip_drive:
    if (sshConfigured) {
        sshAttempted = 1;
        int ok = 1;
        for (int i = 0; i < nFiles; i++) {
            snprintf(localBak, sizeof(localBak), "%s\\%s", dir, bakNames[i]);
            if (!ssh_sync_upload(localBak, bakNames[i]))
                ok = 0;
        }
        sshOK = ok;
    }

    int allOK =
        target_result(driveConfigured, driveAttempted, driveOK) &&
        target_result(sshConfigured, sshAttempted, sshOK);

    /* Keep the local backup set unless every configured target succeeds. */
    if (allOK) {
        for (int i = 0; i < nFiles; i++) {
            snprintf(localBak, sizeof(localBak), "%s\\%s", dir, bakNames[i]);
            DeleteFile(localBak);
        }
    }

    int keepCount = cloudsync_get_keep_count();
    if (keepCount > 0) {
        if (driveOK) drive_prune_old_backups(keepCount, accessTokenSnap, folderIdSnap);
        if (sshOK)   ssh_sync_prune(keepCount);
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
    for (int i = 0; i < nFiles; i++) {
        if (i > 0) strcat(filesStr, ", ");
        strncat(filesStr, bakNames[i],
                sizeof(filesStr) - strlen(filesStr) - 1);
    }

    /* save sync history */
    save_sync_history_entry(dir, &st, filesStr, totalSize, statusStr);

    /* send Telegram notification if enabled */
    if (db_get_setting_int("tg_enabled", 0)) {
        int mode = db_get_setting_int("tg_notify_mode", 0);
        if (mode == 0 || (mode == 1 && !allOK)) {
            char msg[1024];
            char filesDetail[512] = "";
            for (int i = 0; i < nFiles; i++) {
                if (i > 0) strcat(filesDetail, "\n");
                snprintf(filesDetail + strlen(filesDetail),
                         sizeof(filesDetail) - strlen(filesDetail),
                         "%s (%.2f MB)",
                         bakNames[i], fileSizes[i] / (1024.0 * 1024.0));
            }
            snprintf(msg, sizeof(msg), "%s ksc sync %s: %s\n%s",
                allOK ? "\xE2\x9C\x85" : "\xE2\x9D\x8C",
                allOK ? "OK" : "FAILED",
                statusStr, filesDetail);
            telegram_send(msg);
        }
    }

    if (hCloudWnd && IsWindow(hCloudWnd))
        PostMessage(hCloudWnd, WM_CLOUD_SYNC, 100, allOK ? 1 : 0);

done:
    InterlockedExchange(&g_backupInProgress, 0);
    return 0;
}

/* ---- Public API ---- */
bool cloudsync_is_logged_in(void)
{
    AcquireSRWLockShared(&g_cloudLock);
    bool logged = g_loggedIn;
    ReleaseSRWLockShared(&g_cloudLock);
    return logged;
}

void cloudsync_get_email(char *buf, int bufsize)
{
    if (!buf || bufsize <= 0) return;
    AcquireSRWLockShared(&g_cloudLock);
    if (g_loggedIn) {
        if (g_userEmail[0])
            strncpy(buf, g_userEmail, (size_t)(bufsize - 1));
        else
            strncpy(buf, "Logged in", (size_t)(bufsize - 1));
    } else {
        strncpy(buf, "Not logged in", (size_t)(bufsize - 1));
    }
    ReleaseSRWLockShared(&g_cloudLock);
    buf[bufsize - 1] = '\0';
}

void cloudsync_login(HWND hParent)
{
    if (InterlockedCompareExchange(&g_loginInProgress, 1, 0) != 0) return;
    if (cloudsync_is_logged_in()) {
        InterlockedExchange(&g_loginInProgress, 0);
        return;
    }

    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) goto cleanup;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int port = 0;
    for (port = 49152; port < 65535; port++) {
        addr.sin_port = htons((u_short)port);
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
    }
    if (port >= 65535) goto cleanup;
    listen(srv, 1);

    /* PKCE: generate code_verifier and code_challenge */
    char codeVerifier[128] = "";
    char codeChallenge[128] = "";
    {
        BYTE rnd[32];
        HCRYPTPROV hProv = 0;
        if (CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL,
                                 CRYPT_VERIFYCONTEXT)) {
            CryptGenRandom(hProv, sizeof(rnd), rnd);
            CryptReleaseContext(hProv, 0);
        } else {
            srand(GetTickCount());
            for (int i = 0; i < 32; i++)
                rnd[i] = (BYTE)(rand() % 256);
        }

        const char *b64u =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        {
            int cp = 0;
            for (int i = 0; i < 32; i += 3) {
                int rem = 32 - i;
                DWORD val = ((DWORD)rnd[i] << 16);
                if (rem >= 2) val |= ((DWORD)rnd[i + 1] << 8);
                if (rem >= 3) val |= (DWORD)rnd[i + 2];
                codeVerifier[cp++] = b64u[(val >> 18) & 0x3F];
                codeVerifier[cp++] = b64u[(val >> 12) & 0x3F];
                if (rem >= 2) codeVerifier[cp++] = b64u[(val >> 6) & 0x3F];
                if (rem >= 3) codeVerifier[cp++] = b64u[val & 0x3F];
            }
            codeVerifier[cp] = '\0';
        }

        HCRYPTPROV hProv2 = 0;
        HCRYPTHASH hHash = 0;
        if (CryptAcquireContext(&hProv2, nullptr, nullptr, PROV_RSA_FULL,
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
                DWORD val = ((DWORD)hash[i] << 16);
                if (rem >= 2) val |= ((DWORD)hash[i + 1] << 8);
                if (rem >= 3) val |= (DWORD)hash[i + 2];
                codeChallenge[cp++] = b64u[(val >> 18) & 0x3F];
                codeChallenge[cp++] = b64u[(val >> 12) & 0x3F];
                if (rem >= 2) codeChallenge[cp++] = b64u[(val >> 6) & 0x3F];
                if (rem >= 3) codeChallenge[cp++] = b64u[val & 0x3F];
            }
            codeChallenge[cp] = '\0';
        } else if (hProv2) {
            CryptReleaseContext(hProv2, 0);
        }
    }

    char authUrl[1536];
    if (codeChallenge[0]) {
        snprintf(authUrl, sizeof(authUrl),
            "%s?client_id=%s"
            "&redirect_uri=http://127.0.0.1:%d/auth"
            "&response_type=code"
            "&scope=%s"
            "&access_type=offline"
            "&prompt=consent"
            "&code_challenge=%s"
            "&code_challenge_method=S256",
            GOOGLE_AUTH_URI, GOOGLE_CLIENT_ID, port, GOOGLE_SCOPE, codeChallenge);
    } else {
        snprintf(authUrl, sizeof(authUrl),
            "%s?client_id=%s"
            "&redirect_uri=http://127.0.0.1:%d/auth"
            "&response_type=code"
            "&scope=%s"
            "&access_type=offline"
            "&prompt=consent",
            GOOGLE_AUTH_URI, GOOGLE_CLIENT_ID, port, GOOGLE_SCOPE);
    }
    ShellExecute(nullptr, "open", authUrl, nullptr, nullptr, SW_SHOW);

    /* Wait up to 120 seconds while keeping UI responsive */
    ULONGLONG startTick = GetTickCount64();
    int ready = 0;
    while (GetTickCount64() - startTick < 120000ULL) {
        if (hParent && !IsWindow(hParent)) break;
        fd_set fds;
        struct timeval tv = {0, 100000};
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel > 0) { ready = 1; break; }
        if (sel < 0) break;

        MSG msg;
        bool gotQuit = false;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                gotQuit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (gotQuit) { ready = 0; break; }
    }
    if (!ready) goto cleanup;

    cli = accept(srv, nullptr, nullptr);
    closesocket(srv);
    srv = INVALID_SOCKET;
    if (cli == INVALID_SOCKET) goto cleanup;

    char req[4096] = "";
    int rlen = recv(cli, req, sizeof(req) - 1, 0);
    if (rlen <= 0) goto cleanup;
    req[rlen] = '\0';

    /* extract code */
    char *codeStart = strstr(req, "code=");
    if (!codeStart) goto cleanup;
    codeStart += 5;
    char *codeEnd = strchr(codeStart, '&');
    if (!codeEnd) codeEnd = strchr(codeStart, ' ');
    if (!codeEnd) codeEnd = strchr(codeStart, '\r');
    if (!codeEnd) codeEnd = strchr(codeStart, '\n');
    if (!codeEnd) goto cleanup;
    int codeLen = (int)(codeEnd - codeStart);
    if (codeLen <= 0 || codeLen > 1023) goto cleanup;

    char code[1024];
    memcpy(code, codeStart, (size_t)codeLen);
    code[codeLen] = '\0';

    /* URL-encode the code for form POST */
    char encCode[2048];
    int ep = 0;
    for (int i = 0; code[i] && ep < 2040; i++) {
        unsigned char c = (unsigned char)code[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            encCode[ep++] = (char)c;
        } else {
            ep += sprintf(encCode + ep, "%%%02X", c);
        }
    }
    encCode[ep] = '\0';

    /* send success page */
    const char *page =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><h3>Login successful.</h3>"
        "<p>You may close this tab.</p></body></html>";
    send(cli, page, (int)strlen(page), 0);
    closesocket(cli);
    cli = INVALID_SOCKET;

    /* exchange code for tokens */
    char body[2560];
    if (codeChallenge[0]) {
        snprintf(body, sizeof(body),
            "code=%s"
            "&client_id=%s"
            "&client_secret=%s"
            "&redirect_uri=http://127.0.0.1:%d/auth"
            "&code_verifier=%s"
            "&grant_type=authorization_code",
            encCode, GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, port, codeVerifier);
    } else {
        snprintf(body, sizeof(body),
            "code=%s"
            "&client_id=%s"
            "&client_secret=%s"
            "&redirect_uri=http://127.0.0.1:%d/auth"
            "&grant_type=authorization_code",
            encCode, GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, port);
    }

    char *resp = nullptr;
    int status = 0;
    if (!http_post_form(GOOGLE_TOKEN_URI, nullptr, body, &resp, &status))
        goto cleanup;
    if (status != 200 || !resp) { free(resp); goto cleanup; }

    char atBuf[1024] = "";
    char rtBuf[256] = "";
    bool gotAt = json_find_str_buf(resp, "access_token", atBuf, sizeof(atBuf)) && atBuf[0];
    bool gotRt = json_find_str_buf(resp, "refresh_token", rtBuf, sizeof(rtBuf)) && rtBuf[0];
    free(resp);
    if (!gotAt || !gotRt) goto cleanup;

    /* get user email */
    char emailBuf[128] = "";
    char *uresp = nullptr;
    int ust = 0;
    http_get_json(GOOGLE_USERINFO, atBuf, &uresp, &ust);
    if (uresp && ust == 200) {
        json_find_str_buf(uresp, "email", emailBuf, sizeof(emailBuf));
    }
    free(uresp);

    AcquireSRWLockExclusive(&g_cloudLock);
    snprintf(g_accessToken, sizeof(g_accessToken), "%s", atBuf);
    snprintf(g_refreshToken, sizeof(g_refreshToken), "%s", rtBuf);
    snprintf(g_userEmail, sizeof(g_userEmail), "%s", emailBuf);
    g_loggedIn = true;
    ReleaseSRWLockExclusive(&g_cloudLock);

    save_token_values(rtBuf, emailBuf);
    SecureZeroMemory(atBuf, sizeof(atBuf));
    SecureZeroMemory(rtBuf, sizeof(rtBuf));

cleanup:
    if (cli != INVALID_SOCKET) closesocket(cli);
    if (srv != INVALID_SOCKET) closesocket(srv);
    InterlockedExchange(&g_loginInProgress, 0);
}

void cloudsync_logout(void)
{
    AcquireSRWLockExclusive(&g_cloudLock);
    g_loggedIn = false;
    SecureZeroMemory(g_accessToken, sizeof(g_accessToken));
    SecureZeroMemory(g_refreshToken, sizeof(g_refreshToken));
    g_userEmail[0] = '\0';
    g_folderId[0] = '\0';
    ReleaseSRWLockExclusive(&g_cloudLock);
    delete_token_file();
}

void cloudsync_backup_trigger(HWND hCloudWnd)
{
    if (!cloudsync_is_logged_in() && !ssh_sync_is_configured())
        return;
    if (InterlockedCompareExchange(&g_backupInProgress, 1, 0) != 0)
        return;
    HANDLE h = CreateThread(nullptr, 0, cloudsync_backup_thread,
                             (LPVOID)hCloudWnd, 0, nullptr);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_backupInProgress, 0);
}

int cloudsync_load_history(CloudSyncEntry **out, int *pcount)
{
    *out = nullptr;
    *pcount = 0;

    char dir[MAX_PATH], path[MAX_PATH];
    get_cloud_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s\\cloud_sync_history.json", dir);

    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD sz = GetFileSize(h, nullptr);
    if (sz == 0 || sz > 262144) { CloseHandle(h); return 0; }
    char *buf = malloc(sz + 1);
    if (!buf) { CloseHandle(h); return 0; }
    DWORD rd = 0;
    ReadFile(h, buf, sz, &rd, nullptr);
    buf[rd] = '\0';
    CloseHandle(h);

    int cap = 32, cnt = 0;
    CloudSyncEntry *entries = malloc(sizeof(CloudSyncEntry) * cap);
    if (!entries) { free(buf); return 0; }

    char *p = buf;
    while ((p = strstr(p, "\"ts\"")) != nullptr) {
        if (cnt >= cap) {
            cap *= 2;
            CloudSyncEntry *tmp = realloc(entries,
                sizeof(CloudSyncEntry) * cap);
            if (!tmp) { free(entries); free(buf); return 0; }
            entries = tmp;
        }
        CloudSyncEntry *e = &entries[cnt];
        memset(e, 0, sizeof(*e));
        json_find_str_buf(p, "ts", e->ts, sizeof(e->ts));
        json_find_str_buf(p, "files", e->files, sizeof(e->files));
        e->size = json_find_int(p, "size", 0);
        json_find_str_buf(p, "status", e->status, sizeof(e->status));
        cnt++;
        p = strchr(p, '}');
        if (!p) break;
        p++;
    }

    free(buf);
    if (cnt == 0) {
        free(entries);
        return 0;
    }
    *out = entries;
    *pcount = cnt;
    return 1;
}

void cloudsync_free_history(CloudSyncEntry *e) { free(e); }

int cloudsync_get_schedule(void)
{
    return db_get_setting_int("cloud_schedule", 0);
}

void cloudsync_set_schedule(int value)
{
    db_set_setting_int("cloud_schedule", value);
}

int cloudsync_get_compress(void)
{
    return db_get_setting_int("cloud_compress_zip", 0);
}

void cloudsync_set_compress(int value)
{
    db_set_setting_int("cloud_compress_zip", value ? 1 : 0);
}

int cloudsync_get_keep_count(void)
{
    return db_get_setting_int("cloud_keep_count", 0);
}

void cloudsync_set_keep_count(int value)
{
    db_set_setting_int("cloud_keep_count", value > 0 ? value : 0);
}

/* called at startup */
void cloudsync_init(void)
{
    char rt[256] = "", em[128] = "";
    if (load_token_values(rt, sizeof(rt), em, sizeof(em)) && rt[0] != '\0') {
        AcquireSRWLockExclusive(&g_cloudLock);
        snprintf(g_refreshToken, sizeof(g_refreshToken), "%s", rt);
        snprintf(g_userEmail, sizeof(g_userEmail), "%s", em);
        g_loggedIn = true;
        ReleaseSRWLockExclusive(&g_cloudLock);
    }
    SecureZeroMemory(rt, sizeof(rt));
}
