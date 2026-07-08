#include "database.h"
#include "ksc_private.h"
#include "sqlite3.h"

static sqlite3 *g_db = NULL;

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
        ");";

    char *err = NULL;
    rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return 0;
    }

    return 1;
}

void db_close(void)
{
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

void db_increment_key(int key_code, const char *key_name)
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
