#ifndef DATABASE_H
#define DATABASE_H

#include <stdint.h>

typedef struct {
    int key_code;
    char key_name[64];
    int64_t count;
} KeyStat;

int db_init(void);
void db_close(void);
void db_increment_key(int key_code, const char *key_name, const char *app);
int db_get_stats(KeyStat **out_stats);
int db_get_date_range_stats(const char *from, const char *to,
                             const char *app, KeyStat **out_stats);
void db_free_stats(KeyStat *stats);
int db_get_distinct_apps(char ***out_apps, int *out_count);
void db_free_apps(char **apps, int count);

int  db_get_setting_int(const char *key, int default_val);
void db_set_setting_int(const char *key, int value);

#endif
