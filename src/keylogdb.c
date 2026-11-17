#include "keylogdb.h"
#include "database.h"
#include "ksc_private.h"
#include "sqlite3.h"

static sqlite3 *g_klogDb = nullptr;

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
static SRWLOCK g_klogWriteLock = SRWLOCK_INIT;
static bool g_klogCsInit = false;
static HANDLE g_klogSignal = nullptr;
static HANDLE g_klogThread = nullptr;
static volatile bool g_klogRunning = false;

static int keylog_copy_events_locked(KlogEvent *batch, bool *outStop)
{
    int n = 0;
    while (g_klogCount > 0 && n < KLOG_BUF_SIZE) {
        batch[n++] = g_klogBuf[g_klogRead];
        g_klogRead = (g_klogRead + 1) % KLOG_BUF_SIZE;
        g_klogCount--;
    }
    if (outStop) {
        *outStop = (!g_klogRunning && g_klogCount == 0);
    }
    return n;
}

static void keylog_write_batch(const KlogEvent *batch, int count)
{
    AcquireSRWLockExclusive(&g_klogWriteLock);
    if (count > 0 && g_klogDb) {
        sqlite3_exec(g_klogDb, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        for (int i = 0; i < count; i++) {
            keylog_insert(batch[i].key_name, batch[i].vk_code, batch[i].app);
        }
        sqlite3_exec(g_klogDb, "COMMIT", nullptr, nullptr, nullptr);
    }
    ReleaseSRWLockExclusive(&g_klogWriteLock);
}

static DWORD WINAPI klog_writer_thread([[maybe_unused]] LPVOID param)
{
    KlogEvent batch[KLOG_BUF_SIZE];
    for (;;) {
        WaitForSingleObject(g_klogSignal, 100);
        EnterCriticalSection(&g_klogCs);
        bool stop = false;
        int n = keylog_copy_events_locked(batch, &stop);
        LeaveCriticalSection(&g_klogCs);

        if (n > 0) {
            keylog_write_batch(batch, n);
        }
        if (stop) break;
    }
    return 0;
}

static void get_klog_path(char *buf, int bufsz)
{
    char dir[MAX_PATH];
    db_get_db_dir(dir, sizeof(dir));
    snprintf(buf, (size_t)bufsz, "%s\\ksc_keylog.db", dir);
}

int keylog_open(void)
{
    if (g_klogDb) return 1;

    char dbpath[MAX_PATH];
    get_klog_path(dbpath, sizeof(dbpath));

    int rc = sqlite3_open_v2(dbpath, &g_klogDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        if (g_klogDb) { sqlite3_close(g_klogDb); g_klogDb = nullptr; }
        return 0;
    }

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

    char *err = nullptr;
    rc = sqlite3_exec(g_klogDb, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(g_klogDb);
        g_klogDb = nullptr;
        return 0;
    }

    sqlite3_exec(g_klogDb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_busy_timeout(g_klogDb, 2000);

    InitializeCriticalSection(&g_klogCs);
    g_klogCsInit = true;
    g_klogCount = 0;
    g_klogWrite = 0;
    g_klogRead = 0;
    g_klogSignal = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_klogRunning = true;
    g_klogThread = CreateThread(nullptr, 0, klog_writer_thread,
                                 nullptr, 0, nullptr);
    if (!g_klogThread) {
        g_klogRunning = false;
    }

    return 1;
}

void keylog_close(void)
{
    if (g_klogRunning) {
        g_klogRunning = false;
        if (g_klogSignal) SetEvent(g_klogSignal);
        if (g_klogThread) {
            WaitForSingleObject(g_klogThread, INFINITE);
            CloseHandle(g_klogThread);
            g_klogThread = nullptr;
        }
    }
    if (g_klogSignal) {
        CloseHandle(g_klogSignal);
        g_klogSignal = nullptr;
    }
    if (g_klogCsInit) {
        DeleteCriticalSection(&g_klogCs);
        g_klogCsInit = false;
    }

    if (g_klogDb) {
        sqlite3_close(g_klogDb);
        g_klogDb = nullptr;
    }
}

void keylog_insert(const char *key_name, int vk_code, const char *app)
{
    if (!g_klogDb) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    const char *sql =
        "INSERT INTO keylog (timestamp, key_name, vk_code, app) "
        "VALUES (?1, ?2, ?3, ?4);";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_klogDb, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, vk_code);
    sqlite3_bind_text(stmt, 4, app ? app : "", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static sqlite3 *open_temp_klog_if_exists(void)
{
    char dbpath[MAX_PATH];
    get_klog_path(dbpath, sizeof(dbpath));
    DWORD attr = GetFileAttributes(dbpath);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
        return nullptr;

    sqlite3 *tmpDb = nullptr;
    if (sqlite3_open_v2(dbpath, &tmpDb,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (tmpDb) sqlite3_close(tmpDb);
        return nullptr;
    }
    sqlite3_busy_timeout(tmpDb, 2000);
    return tmpDb;
}

int keylog_query_filtered(const char *from, const char *to,
                          const char *app, const char *search,
                          KeylogEntry **out_entries)
{
    if (out_entries) *out_entries = nullptr;
    if (!out_entries || !from || !to) return 0;

    sqlite3 *db = g_klogDb;
    sqlite3 *tempDb = nullptr;
    if (!db) {
        tempDb = open_temp_klog_if_exists();
        db = tempDb;
    }
    if (!db) return 0;

    const char *appFilter = (app && app[0]) ? app : "";
    const char *searchFilter = (search && search[0]) ? search : "";
    const char *sql =
        "SELECT id, timestamp, key_name, vk_code, app "
        "FROM keylog "
        "WHERE timestamp >= ?1 AND timestamp <= (?2 || ' 23:59:59') "
        "AND (?3 = '' OR app = ?3) "
        "AND (?4 = '' OR key_name LIKE ('%' || ?4 || '%') "
        "     OR app LIKE ('%' || ?4 || '%')) "
        "ORDER BY timestamp DESC, id DESC;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (tempDb) sqlite3_close(tempDb);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, from, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, to, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, appFilter, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, searchFilter, -1, SQLITE_STATIC);

    int cap = 64, cnt = 0;
    KeylogEntry *entries = malloc(sizeof(KeylogEntry) * cap);
    if (!entries) {
        sqlite3_finalize(stmt);
        if (tempDb) sqlite3_close(tempDb);
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cnt >= cap) {
            cap *= 2;
            KeylogEntry *tmp = realloc(entries, sizeof(KeylogEntry) * cap);
            if (!tmp) {
                free(entries);
                sqlite3_finalize(stmt);
                if (tempDb) sqlite3_close(tempDb);
                return 0;
            }
            entries = tmp;
        }
        entries[cnt].id = sqlite3_column_int(stmt, 0);
        const char *tsText = (const char *)sqlite3_column_text(stmt, 1);
        if (tsText) strncpy(entries[cnt].timestamp, tsText, 31);
        else entries[cnt].timestamp[0] = '\0';
        entries[cnt].timestamp[31] = '\0';

        const char *knText = (const char *)sqlite3_column_text(stmt, 2);
        if (knText) strncpy(entries[cnt].key_name, knText, 63);
        else entries[cnt].key_name[0] = '\0';
        entries[cnt].key_name[63] = '\0';

        entries[cnt].vk_code = sqlite3_column_int(stmt, 3);
        const char *ap = (const char *)sqlite3_column_text(stmt, 4);
        if (ap) strncpy(entries[cnt].app, ap, 255);
        else entries[cnt].app[0] = '\0';
        entries[cnt].app[255] = '\0';
        cnt++;
    }
    sqlite3_finalize(stmt);
    if (tempDb) sqlite3_close(tempDb);

    if (cnt == 0) {
        free(entries);
        return 0;
    }
    *out_entries = entries;
    return cnt;
}

int keylog_query(const char *from, const char *to, const char *app,
                  KeylogEntry **out_entries)
{
    return keylog_query_filtered(from, to, app, nullptr, out_entries);
}

int keylog_get_apps(char ***out_apps, int *out_count)
{
    if (out_apps) *out_apps = nullptr;
    if (out_count) *out_count = 0;
    if (!out_apps || !out_count) return 0;

    sqlite3 *db = g_klogDb;
    sqlite3 *tempDb = nullptr;
    if (!db) {
        tempDb = open_temp_klog_if_exists();
        db = tempDb;
    }
    if (!db) return 0;

    const char *sql =
        "SELECT DISTINCT app FROM keylog WHERE app != '' ORDER BY app ASC;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (tempDb) sqlite3_close(tempDb);
        return 0;
    }

    int cap = 16, cnt = 0;
    char **apps = malloc(sizeof(char *) * cap);
    if (!apps) {
        sqlite3_finalize(stmt);
        if (tempDb) sqlite3_close(tempDb);
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (!name || !name[0]) continue;
        if (cnt >= cap) {
            cap *= 2;
            char **tmp = realloc(apps, sizeof(char *) * cap);
            if (!tmp) {
                for (int i = 0; i < cnt; i++) free(apps[i]);
                free(apps);
                sqlite3_finalize(stmt);
                if (tempDb) sqlite3_close(tempDb);
                return 0;
            }
            apps = tmp;
        }
        apps[cnt] = _strdup(name);
        if (apps[cnt]) cnt++;
    }
    sqlite3_finalize(stmt);
    if (tempDb) sqlite3_close(tempDb);

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
    char aux[MAX_PATH];
    get_klog_path(dbpath, sizeof(dbpath));
    DeleteFile(dbpath);
    snprintf(aux, sizeof(aux), "%s-wal", dbpath);
    DeleteFile(aux);
    snprintf(aux, sizeof(aux), "%s-shm", dbpath);
    DeleteFile(aux);
}

bool keylog_is_open(void)
{
    return g_klogDb != nullptr;
}

void keylog_queue_event(const char *key_name, int vk_code,
                         const char *app)
{
    if (!g_klogCsInit || !g_klogRunning) return;
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
    if (g_klogSignal) SetEvent(g_klogSignal);
}

void keylog_flush_events(void)
{
    if (!g_klogCsInit || !g_klogSignal) return;
    KlogEvent batch[KLOG_BUF_SIZE];
    EnterCriticalSection(&g_klogCs);
    int n = keylog_copy_events_locked(batch, nullptr);
    LeaveCriticalSection(&g_klogCs);
    keylog_write_batch(batch, n);
}

int keylog_backup_safe(const char *destPath)
{
    if (!destPath) return 0;

    keylog_flush_events();

    sqlite3 *srcDb = g_klogDb;
    sqlite3 *tempDb = nullptr;
    if (!srcDb) {
        tempDb = open_temp_klog_if_exists();
        srcDb = tempDb;
    }
    if (!srcDb) return 0;

    sqlite3 *backupDb = nullptr;
    int rc = sqlite3_open_v2(destPath, &backupDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        if (backupDb) sqlite3_close(backupDb);
        if (tempDb) sqlite3_close(tempDb);
        return 0;
    }

    sqlite3_backup *bk = sqlite3_backup_init(backupDb, "main", srcDb, "main");
    if (!bk) {
        sqlite3_close(backupDb);
        if (tempDb) sqlite3_close(tempDb);
        return 0;
    }

    do {
        rc = sqlite3_backup_step(bk, -1);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
            Sleep(10);
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    sqlite3_backup_finish(bk);
    sqlite3_close(backupDb);
    if (tempDb) sqlite3_close(tempDb);

    return (rc == SQLITE_DONE) ? 1 : 0;
}
