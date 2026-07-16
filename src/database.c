#include "database.h"
#include "ksc_private.h"
#include "sqlite3.h"

static sqlite3 *g_db = NULL;

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
static HANDLE g_eventSignal = NULL;
static HANDLE g_writerThread = NULL;
static volatile BOOL g_writerRunning = FALSE;

static DWORD WINAPI db_writer_thread(LPVOID param)
{
    (void)param;
    while (g_writerRunning) {
        WaitForSingleObject(g_eventSignal, 100);

        EnterCriticalSection(&g_eventCs);
        if (g_eventCount > 0 && g_db) {
            sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
            while (g_eventCount > 0) {
                const KeyEvent *e = &g_eventBuf[g_eventRead];
                db_increment_key(e->key_code, e->key_name, e->app);
                g_eventRead = (g_eventRead + 1) % EVENT_BUF_SIZE;
                g_eventCount--;
            }
            sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL);
        }
        LeaveCriticalSection(&g_eventCs);
    }
    return 0;
}

static void get_today_date(char *buf, int bufsize)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    sprintf(buf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

static void get_cutoff_date(char *buf, int bufsize, int days_back)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart -= (ULONGLONG)days_back * 24ULL * 60ULL * 60ULL * 10000000ULL;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    sprintf(buf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

static void ensure_db_dir(void)
{
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0) {
        char dir[MAX_PATH];
        sprintf(dir, "%s\\KSC", appdata);
        CreateDirectory(dir, NULL);
    }
}

int db_init(void)
{
    ensure_db_dir();

    char db_path[MAX_PATH];
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0) {
        sprintf(db_path, "%s\\KSC\\ksc.db", appdata);
    } else {
        strcpy(db_path, "ksc.db");
    }

    int rc = sqlite3_open_v2(db_path, &g_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) return 0;

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

    char *err = NULL;
    rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return 0;
    }

    InitializeCriticalSection(&g_eventCs);
    g_eventSignal = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_writerRunning = TRUE;
    g_writerThread = CreateThread(NULL, 0, db_writer_thread,
                                   NULL, 0, NULL);

    return 1;
}

void db_close(void)
{
    if (g_writerRunning) {
        g_writerRunning = FALSE;
        SetEvent(g_eventSignal);
        if (g_writerThread) {
            DWORD wr = WaitForSingleObject(g_writerThread, 5000);
            if (wr == WAIT_TIMEOUT) {
                TerminateThread(g_writerThread, 0);
            }
            CloseHandle(g_writerThread);
            g_writerThread = NULL;
        }
    }
    if (g_eventSignal) {
        CloseHandle(g_eventSignal);
        g_eventSignal = NULL;
    }
    DeleteCriticalSection(&g_eventCs);

    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
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

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

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

    sqlite3_stmt *dst = NULL;
    if (sqlite3_prepare_v2(g_db, dailySql, -1, &dst, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(dst, 1, key_code);
    sqlite3_bind_text(dst, 2, today, -1, SQLITE_STATIC);
    sqlite3_bind_text(dst, 3, appName, -1, SQLITE_STATIC);
    sqlite3_step(dst);
    sqlite3_finalize(dst);
}

int db_get_stats(KeyStat **out_stats)
{
    if (!g_db || !out_stats) return 0;

    const char *sql =
        "SELECT key_code, key_name, count FROM key_counts ORDER BY count DESC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

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
        strncpy(stats[count].key_name,
                (const char *)sqlite3_column_text(stmt, 1), 63);
        stats[count].key_name[63] = '\0';
        stats[count].app[0] = '\0';
        stats[count].count = sqlite3_column_int64(stmt, 2);
        count++;
    }

    sqlite3_finalize(stmt);
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

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
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
            char buf[16];
            sprintf(buf, "Key 0x%02X", stats[count].key_code);
            strncpy(stats[count].key_name, buf, 63);
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
    *out_stats = stats;
    return count;
}

int db_get_distinct_apps(char ***out_apps, int *out_count)
{
    if (!g_db || !out_apps || !out_count) return 0;

    const char *sql =
        "SELECT DISTINCT app FROM key_daily WHERE app != '' ORDER BY app ASC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
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
                free(apps);
                sqlite3_finalize(stmt);
                return 0;
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
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
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
    sprintf(buf, "%d", value);

    const char *sql =
        "INSERT OR REPLACE INTO settings (key, value) VALUES (?1, ?2);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, buf, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_get_setting_str(const char *key, const char *default_val,
                         char *out, int out_size)
{
    if (!g_db) {
        if (default_val) strncpy(out, default_val, out_size - 1);
        else out[0] = '\0';
        out[out_size - 1] = '\0';
        return;
    }

    const char *sql = "SELECT value FROM settings WHERE key = ?1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
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
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_queue_event(int key_code, const char *key_name, const char *app)
{
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
    SetEvent(g_eventSignal);
}

void db_flush_events(void)
{
    if (!g_eventSignal) return;
    EnterCriticalSection(&g_eventCs);
    if (g_eventCount > 0 && g_db) {
        sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        while (g_eventCount > 0) {
            const KeyEvent *e = &g_eventBuf[g_eventRead];
            db_increment_key(e->key_code, e->key_name, e->app);
            g_eventRead = (g_eventRead + 1) % EVENT_BUF_SIZE;
            g_eventCount--;
        }
        sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL);
    }
    LeaveCriticalSection(&g_eventCs);
}

void db_reset_stats(void)
{
    if (!g_db) return;
    sqlite3_exec(g_db,
        "DELETE FROM key_counts; DELETE FROM key_daily;",
        NULL, NULL, NULL);
}

int64_t db_get_today_count(void)
{
    if (!g_db) return 0;
    char today[16];
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        sprintf(today, "%04d-%02d-%02d",
                st.wYear, st.wMonth, st.wDay);
    }
    const char *sql =
        "SELECT SUM(count) FROM key_daily WHERE date = ?1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, today, -1, SQLITE_STATIC);
    int64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return total;
}

void db_get_db_dir(char *buf, int bufsize)
{
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0)
        sprintf(buf, "%s\\KSC", appdata);
    else
        strcpy(buf, ".");
}
