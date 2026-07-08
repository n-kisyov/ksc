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
void db_increment_key(int key_code, const char *key_name);
int db_get_stats(KeyStat **out_stats);
void db_free_stats(KeyStat *stats);

#endif
