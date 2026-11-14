#include "database.h"
#include "ksc_private.h"
#include "sqlite3.h"

static sqlite3 *g_db = nullptr;

#define EVENT_BUF_SIZE 256

typedef struct {
    int key_code;
    char key_name[64];
    char app[256];
} KeyEvent;

static KeyEvent g_eventBuf[EVENT_BUF_SIZE];
static int g_eventCount = 0;
static int g_eventWrite = 0;
static int g_eventRead  = 0;
static CRITICAL_SECTION g_eventCs;
static bool g_csInitialized = false;
static HANDLE g_eventSignal = nullptr;
static HANDLE g_writerThread = nullptr;
static volatile bool g_writerRunning = false;
static bool g_portableOverride = false;

void db_set_portable_mode(bool portable)
{
    g_portableOverride = portable;
}

bool db_is_portable_mode(void)
{
    if (g_portableOverride) return true;

    char exePath[MAX_PATH];
    DWORD len = GetModuleFileName(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char *slash = strrchr(exePath, '\\');
        if (slash) {
            *(slash + 1) = '\0';
            char marker[MAX_PATH];
            int n = snprintf(marker, sizeof(marker), "%sportable.txt", exePath);
            if (n > 0 && n < (int)sizeof(marker)) {
                DWORD attr = GetFileAttributes(marker);
                if (attr != INVALID_FILE_ATTRIBUTES &&
                    !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void db_get_db_dir(char *buf, int bufsize)
{
    if (!buf || bufsize <= 0) return;

    if (db_is_portable_mode()) {
        char exePath[MAX_PATH];
        DWORD len = GetModuleFileName(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            char *slash = strrchr(exePath, '\\');
            if (slash) {
                *slash = '\0';
                snprintf(buf, (size_t)bufsize, "%s", exePath);
                return;
            }
        }
    }

    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0) {
        snprintf(buf, (size_t)bufsize, "%s\\KSC", appdata);
        CreateDirectory(buf, nullptr);
    } else {
        snprintf(buf, (size_t)bufsize, ".");
    }
}

static void db_drain_events_locked(void)
{
    if (g_eventCount > 0 && g_db) {
        sqlite3_exec(g_db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        while (g_eventCount > 0) {
            const KeyEvent *e = &g_eventBuf[g_eventRead];
            db_increment_key(e->key_code, e->key_name, e->app);
            g_eventRead = (g_eventRead + 1) % EVENT_BUF_SIZE;
            g_eventCount--;
        }
        sqlite3_exec(g_db, "COMMIT", nullptr, nullptr, nullptr);
    }
}

static DWORD WINAPI db_writer_thread([[maybe_unused]] LPVOID param)
{
    for (;;) {
        WaitForSingleObject(g_eventSignal, 100);

        EnterCriticalSection(&g_eventCs);
        db_drain_events_locked();
        if (!g_writerRunning && g_eventCount == 0) {
            LeaveCriticalSection(&g_eventCs);
            break;
        }
        LeaveCriticalSection(&g_eventCs);
    }
    return 0;
}

static void get_today_date(char *buf, int bufsize)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, (size_t)bufsize, "%04d-%02d-%02d",
             st.wYear, st.wMonth, st.wDay);
}

int db_init(void)
{
    if (g_db) return 1;

    char dir[MAX_PATH];
    db_get_db_dir(dir, sizeof(dir));

    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s\\ksc.db", dir);

    int rc = sqlite3_open_v2(db_path, &g_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
        return 0;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS key_counts ("
        "key_code INTEGER PRIMARY KEY, "
        "key_name TEXT NOT NULL, "
        "count INTEGER DEFAULT 0"
        ");"

        "CREATE TABLE IF NOT EXISTS settings ("
        "key TEXT PRIMARY KEY, "
        "value TEXT NOT NULL"
        ");"

        "CREATE TABLE IF NOT EXISTS key_daily ("
        "key_code INTEGER NOT NULL, "
        "date TEXT NOT NULL, "
        "app TEXT NOT NULL DEFAULT '', "
        "count INTEGER DEFAULT 0, "
        "PRIMARY KEY (key_code, date, app)"
        ");";

    char *err = nullptr;
    rc = sqlite3_exec(g_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(g_db);
        g_db = nullptr;
        return 0;
    }

    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_busy_timeout(g_db, 2000);

    InitializeCriticalSection(&g_eventCs);
    g_csInitialized = true;
    g_eventCount = 0;
    g_eventWrite = 0;
    g_eventRead = 0;
    g_eventSignal = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_writerRunning = true;
    g_writerThread = CreateThread(nullptr, 0, db_writer_thread,
                                   nullptr, 0, nullptr);
    if (!g_writerThread) {
        g_writerRunning = false;
    }

    return 1;
}

void db_close(void)
{
    if (g_writerRunning) {
        g_writerRunning = false;
        if (g_eventSignal) SetEvent(g_eventSignal);
        if (g_writerThread) {
            WaitForSingleObject(g_writerThread, INFINITE);
            CloseHandle(g_writerThread);
            g_writerThread = nullptr;
        }
    }
    if (g_eventSignal) {
        CloseHandle(g_eventSignal);
        g_eventSignal = nullptr;
    }
    if (g_csInitialized) {
        DeleteCriticalSection(&g_eventCs);
        g_csInitialized = false;
    }

    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
}

void db_increment_key(int key_code, const char *key_name, const char *app)
{
    if (!g_db) return;

    const char *sql =
        "INSERT INTO key_counts (key_code, key_name, count) "
        "VALUES (?1, ?2, 1) "
        "ON CONFLICT(key_code) DO UPDATE SET "
        "count = count + 1, key_name = ?3;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_int(stmt, 1, key_code);
    sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, key_name, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    char today[16];
    get_today_date(today, sizeof(today));
    const char *appName = (app && app[0]) ? app : "";
    const char *dailySql =
        "INSERT INTO key_daily (key_code, date, app, count) "
        "VALUES (?1, ?2, ?3, 1) "
        "ON CONFLICT(key_code, date, app) DO UPDATE SET "
        "count = count + 1;";

    sqlite3_stmt *dst = nullptr;
    if (sqlite3_prepare_v2(g_db, dailySql, -1, &dst, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(dst, 1, key_code);
    sqlite3_bind_text(dst, 2, today, -1, SQLITE_STATIC);
    sqlite3_bind_text(dst, 3, appName, -1, SQLITE_STATIC);
    sqlite3_step(dst);
    sqlite3_finalize(dst);
}

int db_get_stats(KeyStat **out_stats)
{
    if (out_stats) *out_stats = nullptr;
    if (!g_db || !out_stats) return 0;

    const char *sql =
        "SELECT key_code, key_name, count FROM key_counts ORDER BY count DESC;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    int capacity = 64;
    int count = 0;
    KeyStat *stats = malloc(sizeof(KeyStat) * capacity);
    if (!stats) {
        sqlite3_finalize(stmt);
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            KeyStat *tmp = realloc(stats, sizeof(KeyStat) * capacity);
            if (!tmp) {
                free(stats);
                sqlite3_finalize(stmt);
                return 0;
            }
            stats = tmp;
        }

        stats[count].key_code = sqlite3_column_int(stmt, 0);
        const char *kname = (const char *)sqlite3_column_text(stmt, 1);
        if (kname) {
            strncpy(stats[count].key_name, kname, 63);
        } else {
            snprintf(stats[count].key_name, 64, "Key 0x%02X", stats[count].key_code);
        }
        stats[count].key_name[63] = '\0';
        stats[count].app[0] = '\0';
        stats[count].count = sqlite3_column_int64(stmt, 2);
        count++;
    }

    sqlite3_finalize(stmt);
    if (count == 0) {
        free(stats);
        return 0;
    }
    *out_stats = stats;
    return count;
}

void db_free_stats(KeyStat *stats)
{
    free(stats);
}

int db_get_date_range_stats(const char *from, const char *to,
                             const char *app, int detailed,
                             KeyStat **out_stats)
{
    if (out_stats) *out_stats = nullptr;
    if (!g_db || !out_stats || !from || !to) return 0;

    const char *appFilter = (app && app[0]) ? app : "";
    const char *sql;
    if (detailed) {
        sql =
            "SELECT kd.key_code, kc.key_name, kd.app, SUM(kd.count) "
            "FROM key_daily kd "
            "LEFT JOIN key_counts kc ON kd.key_code = kc.key_code "
            "WHERE kd.date >= ?1 AND kd.date <= ?2 "
            "AND (?3 = '' OR kd.app = ?3) "
            "GROUP BY kd.key_code, kd.app "
            "ORDER BY SUM(kd.count) DESC;";
    } else {
        sql =
            "SELECT kd.key_code, kc.key_name, '' AS app, SUM(kd.count) "
            "FROM key_daily kd "
            "LEFT JOIN key_counts kc ON kd.key_code = kc.key_code "
            "WHERE kd.date >= ?1 AND kd.date <= ?2 "
            "AND (?3 = '' OR kd.app = ?3) "
            "GROUP BY kd.key_code "
            "ORDER BY SUM(kd.count) DESC;";
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, from, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, to, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, appFilter, -1, SQLITE_STATIC);

    int capacity = 64;
    int count = 0;
    KeyStat *stats = malloc(sizeof(KeyStat) * capacity);
    if (!stats) {
        sqlite3_finalize(stmt);
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            KeyStat *tmp = realloc(stats, sizeof(KeyStat) * capacity);
            if (!tmp) {
                free(stats);
                sqlite3_finalize(stmt);
                return 0;
            }
            stats = tmp;
        }

        stats[count].key_code = sqlite3_column_int(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (name) {
            strncpy(stats[count].key_name, name, 63);
        } else {
            snprintf(stats[count].key_name, 64, "Key 0x%02X", stats[count].key_code);
        }
        stats[count].key_name[63] = '\0';
        {
            const char *appName = (const char *)sqlite3_column_text(stmt, 2);
            if (appName) {
                strncpy(stats[count].app, appName, 255);
            } else {
                stats[count].app[0] = '\0';
            }
            stats[count].app[255] = '\0';
        }
        stats[count].count = sqlite3_column_int64(stmt, 3);
        count++;
    }

    sqlite3_finalize(stmt);
    if (count == 0) {
        free(stats);
        return 0;
    }
    *out_stats = stats;
    return count;
}

int db_get_distinct_apps(char ***out_apps, int *out_count)
{
    if (out_apps) *out_apps = nullptr;
    if (out_count) *out_count = 0;
    if (!g_db || !out_apps || !out_count) return 0;

    const char *sql =
        "SELECT DISTINCT app FROM key_daily WHERE app != '' ORDER BY app ASC;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    int cap = 16, cnt = 0;
    char **apps = malloc(sizeof(char *) * cap);
    if (!apps) { sqlite3_finalize(stmt); return 0; }

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
                return 0;
            }
            apps = tmp;
        }
        apps[cnt] = _strdup(name);
        if (apps[cnt]) cnt++;
    }
    sqlite3_finalize(stmt);
    *out_apps = apps;
    *out_count = cnt;
    return 1;
}

void db_free_apps(char **apps, int count)
{
    if (!apps) return;
    for (int i = 0; i < count; i++) free(apps[i]);
    free(apps);
}

int db_get_setting_int(const char *key, int default_val)
{
    if (!g_db) return default_val;

    const char *sql = "SELECT value FROM settings WHERE key = ?1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return default_val;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    int result = default_val;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = atoi(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

void db_set_setting_int(const char *key, int value)
{
    if (!g_db) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);

    const char *sql =
        "INSERT OR REPLACE INTO settings (key, value) VALUES (?1, ?2);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, buf, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_get_setting_str(const char *key, const char *default_val,
                         char *out, int out_size)
{
    if (!out || out_size <= 0) return;
    if (!g_db) {
        if (default_val) strncpy(out, default_val, out_size - 1);
        else out[0] = '\0';
        out[out_size - 1] = '\0';
        return;
    }

    const char *sql = "SELECT value FROM settings WHERE key = ?1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (default_val) strncpy(out, default_val, out_size - 1);
        else out[0] = '\0';
        out[out_size - 1] = '\0';
        return;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) strncpy(out, val, out_size - 1);
        else if (default_val) strncpy(out, default_val, out_size - 1);
        else out[0] = '\0';
    } else {
        if (default_val) strncpy(out, default_val, out_size - 1);
        else out[0] = '\0';
    }
    out[out_size - 1] = '\0';
    sqlite3_finalize(stmt);
}

void db_set_setting_str(const char *key, const char *value)
{
    if (!g_db) return;

    const char *sql =
        "INSERT OR REPLACE INTO settings (key, value) VALUES (?1, ?2);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_queue_event(int key_code, const char *key_name, const char *app)
{
    if (!g_csInitialized || !g_writerRunning) return;
    EnterCriticalSection(&g_eventCs);
    if (g_eventCount < EVENT_BUF_SIZE) {
        KeyEvent *e = &g_eventBuf[g_eventWrite];
        e->key_code = key_code;
        strncpy(e->key_name, key_name, 63);
        e->key_name[63] = '\0';
        if (app && app[0])
            strncpy(e->app, app, 255);
        else
            e->app[0] = '\0';
        e->app[255] = '\0';
        g_eventWrite = (g_eventWrite + 1) % EVENT_BUF_SIZE;
        g_eventCount++;
    }
    LeaveCriticalSection(&g_eventCs);
    if (g_eventSignal) SetEvent(g_eventSignal);
}

void db_flush_events(void)
{
    if (!g_csInitialized || !g_eventSignal) return;
    EnterCriticalSection(&g_eventCs);
    db_drain_events_locked();
    LeaveCriticalSection(&g_eventCs);
}

void db_reset_stats(void)
{
    if (!g_db) return;
    if (g_csInitialized) {
        EnterCriticalSection(&g_eventCs);
        g_eventCount = 0;
        g_eventWrite = 0;
        g_eventRead = 0;
        sqlite3_exec(g_db,
            "DELETE FROM key_counts; DELETE FROM key_daily;",
            nullptr, nullptr, nullptr);
        LeaveCriticalSection(&g_eventCs);
    } else {
        sqlite3_exec(g_db,
            "DELETE FROM key_counts; DELETE FROM key_daily;",
            nullptr, nullptr, nullptr);
    }
}

int64_t db_get_today_count(void)
{
    if (!g_db) return 0;
    char today[16];
    get_today_date(today, sizeof(today));
    const char *sql =
        "SELECT COALESCE(SUM(count), 0) FROM key_daily WHERE date = ?1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, today, -1, SQLITE_STATIC);
    int64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return total;
}

int db_backup_safe(const char *destPath)
{
    sqlite3 *backupDb = nullptr;
    sqlite3_backup *bk = nullptr;
    int rc;

    if (!g_db || !destPath) return 0;

    db_flush_events();

    rc = sqlite3_open_v2(destPath, &backupDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        if (backupDb) sqlite3_close(backupDb);
        return 0;
    }

    bk = sqlite3_backup_init(backupDb, "main", g_db, "main");
    if (!bk) { sqlite3_close(backupDb); return 0; }

    do {
        rc = sqlite3_backup_step(bk, -1);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
            Sleep(10);
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    sqlite3_backup_finish(bk);
    sqlite3_close(backupDb);

    return (rc == SQLITE_DONE) ? 1 : 0;
}
