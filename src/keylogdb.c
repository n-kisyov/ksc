#include "keylogdb.h"
#include "ksc_private.h"
#include "sqlite3.h"

static sqlite3 *g_klogDb = NULL;

#define KLOG_BUF_SIZE 256

typedef struct {
    char key_name[64];
    int vk_code;
    char app[256];
} KlogEvent;

static KlogEvent g_klogBuf[KLOG_BUF_SIZE];
static int g_klogCount = 0;
static int g_klogWrite = 0;
static int g_klogRead  = 0;
static CRITICAL_SECTION g_klogCs;
static HANDLE g_klogSignal = NULL;
static HANDLE g_klogThread = NULL;
static volatile BOOL g_klogRunning = FALSE;

static DWORD WINAPI klog_writer_thread(LPVOID param)
{
    (void)param;
    while (g_klogRunning) {
        WaitForSingleObject(g_klogSignal, 100);
        EnterCriticalSection(&g_klogCs);
        if (g_klogCount > 0 && g_klogDb) {
            sqlite3_exec(g_klogDb, "BEGIN IMMEDIATE", NULL, NULL, NULL);
            while (g_klogCount > 0) {
                const KlogEvent *e = &g_klogBuf[g_klogRead];
                keylog_insert(e->key_name, e->vk_code, e->app);
                g_klogRead = (g_klogRead + 1) % KLOG_BUF_SIZE;
                g_klogCount--;
            }
            sqlite3_exec(g_klogDb, "COMMIT", NULL, NULL, NULL);
        }
        LeaveCriticalSection(&g_klogCs);
    }
    return 0;
}

static void get_klog_path(char *buf, int bufsz)
{
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0)
        sprintf(buf, "%s\\KSC\\ksc_keylog.db", appdata);
    else
        strcpy(buf, "ksc_keylog.db");
}

int keylog_open(void)
{
    if (g_klogDb) return 1;

    char dbpath[MAX_PATH];
    get_klog_path(dbpath, sizeof(dbpath));

    int rc = sqlite3_open_v2(dbpath, &g_klogDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) return 0;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS keylog ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "timestamp TEXT NOT NULL, "
        "key_name TEXT NOT NULL, "
        "vk_code INTEGER NOT NULL, "
        "app TEXT NOT NULL DEFAULT ''"
        ");"

        "CREATE INDEX IF NOT EXISTS idx_keylog_ts "
        "ON keylog(timestamp);";

    char *err = NULL;
    rc = sqlite3_exec(g_klogDb, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(g_klogDb);
        g_klogDb = NULL;
        return 0;
    }

    InitializeCriticalSection(&g_klogCs);
    g_klogSignal = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_klogRunning = TRUE;
    g_klogThread = CreateThread(NULL, 0, klog_writer_thread,
                                 NULL, 0, NULL);

    return 1;
}

void keylog_close(void)
{
    if (g_klogRunning) {
        g_klogRunning = FALSE;
        SetEvent(g_klogSignal);
        if (g_klogThread) {
            DWORD wr = WaitForSingleObject(g_klogThread, 5000);
            if (wr == WAIT_TIMEOUT) {
                TerminateThread(g_klogThread, 0);
            }
            CloseHandle(g_klogThread);
            g_klogThread = NULL;
        }
    }
    if (g_klogSignal) {
        CloseHandle(g_klogSignal);
        g_klogSignal = NULL;
    }
    DeleteCriticalSection(&g_klogCs);

    if (g_klogDb) {
        sqlite3_close(g_klogDb);
        g_klogDb = NULL;
    }
}

void keylog_insert(const char *key_name, int vk_code, const char *app)
{
    if (!g_klogDb) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);

    const char *sql =
        "INSERT INTO keylog (timestamp, key_name, vk_code, app) "
        "VALUES (?1, ?2, ?3, ?4);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_klogDb, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, vk_code);
    sqlite3_bind_text(stmt, 4, app ? app : "", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int keylog_query(const char *from, const char *to, const char *app,
                  KeylogEntry **out_entries)
{
    if (!g_klogDb || !out_entries || !from || !to) return 0;

    const char *appFilter = (app && app[0]) ? app : "";
    const char *sql =
        "SELECT id, timestamp, key_name, vk_code, app "
        "FROM keylog "
        "WHERE timestamp >= ?1 AND timestamp <= (?2 || ' 23:59:59') "
        "AND (?3 = '' OR app = ?3) "
        "ORDER BY timestamp DESC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_klogDb, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, from, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, to, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, appFilter, -1, SQLITE_STATIC);

    int cap = 64, cnt = 0;
    KeylogEntry *entries = malloc(sizeof(KeylogEntry) * cap);
    if (!entries) { sqlite3_finalize(stmt); return 0; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cnt >= cap) {
            cap *= 2;
            KeylogEntry *tmp = realloc(entries, sizeof(KeylogEntry) * cap);
            if (!tmp) { free(entries); sqlite3_finalize(stmt); return 0; }
            entries = tmp;
        }
        entries[cnt].id = sqlite3_column_int(stmt, 0);
        strncpy(entries[cnt].timestamp,
                (const char *)sqlite3_column_text(stmt, 1), 31);
        entries[cnt].timestamp[31] = '\0';
        strncpy(entries[cnt].key_name,
                (const char *)sqlite3_column_text(stmt, 2), 63);
        entries[cnt].key_name[63] = '\0';
        entries[cnt].vk_code = sqlite3_column_int(stmt, 3);
        {
            const char *ap = (const char *)sqlite3_column_text(stmt, 4);
            if (ap) strncpy(entries[cnt].app, ap, 255);
            else entries[cnt].app[0] = '\0';
            entries[cnt].app[255] = '\0';
        }
        cnt++;
    }
    sqlite3_finalize(stmt);
    *out_entries = entries;
    return cnt;
}

int keylog_get_apps(char ***out_apps, int *out_count)
{
    if (!g_klogDb || !out_apps || !out_count) return 0;

    const char *sql =
        "SELECT DISTINCT app FROM keylog WHERE app != '' ORDER BY app ASC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_klogDb, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    int cap = 16, cnt = 0;
    char **apps = malloc(sizeof(char *) * cap);
    if (!apps) { sqlite3_finalize(stmt); return 0; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cnt >= cap) {
            cap *= 2;
            char **tmp = realloc(apps, sizeof(char *) * cap);
            if (!tmp) {
                for (int i = 0; i < cnt; i++) free(apps[i]);
                free(apps); sqlite3_finalize(stmt); return 0;
            }
            apps = tmp;
        }
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        apps[cnt] = name ? _strdup(name) : NULL;
        cnt++;
    }
    sqlite3_finalize(stmt);
    *out_apps = apps;
    *out_count = cnt;
    return 1;
}

void keylog_free_entries(KeylogEntry *entries)
{
    free(entries);
}

void keylog_free_apps(char **apps, int count)
{
    if (!apps) return;
    for (int i = 0; i < count; i++) free(apps[i]);
    free(apps);
}

void keylog_delete_db(void)
{
    keylog_close();
    char dbpath[MAX_PATH];
    get_klog_path(dbpath, sizeof(dbpath));
    DeleteFile(dbpath);
}

int keylog_is_open(void)
{
    return g_klogDb != NULL;
}

void keylog_queue_event(const char *key_name, int vk_code,
                         const char *app)
{
    EnterCriticalSection(&g_klogCs);
    if (g_klogCount < KLOG_BUF_SIZE) {
        KlogEvent *e = &g_klogBuf[g_klogWrite];
        strncpy(e->key_name, key_name, 63);
        e->key_name[63] = '\0';
        e->vk_code = vk_code;
        if (app && app[0])
            strncpy(e->app, app, 255);
        else
            e->app[0] = '\0';
        e->app[255] = '\0';
        g_klogWrite = (g_klogWrite + 1) % KLOG_BUF_SIZE;
        g_klogCount++;
    }
    LeaveCriticalSection(&g_klogCs);
    SetEvent(g_klogSignal);
}

void keylog_flush_events(void)
{
    if (!g_klogSignal) return;
    EnterCriticalSection(&g_klogCs);
    if (g_klogCount > 0 && g_klogDb) {
        sqlite3_exec(g_klogDb, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        while (g_klogCount > 0) {
            const KlogEvent *e = &g_klogBuf[g_klogRead];
            keylog_insert(e->key_name, e->vk_code, e->app);
            g_klogRead = (g_klogRead + 1) % KLOG_BUF_SIZE;
            g_klogCount--;
        }
        sqlite3_exec(g_klogDb, "COMMIT", NULL, NULL, NULL);
    }
    LeaveCriticalSection(&g_klogCs);
}
