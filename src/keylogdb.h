#ifndef KEYLOGDB_H
#define KEYLOGDB_H

#include <stdint.h>

typedef struct {
    int id;
    char timestamp[32];
    char key_name[64];
    int vk_code;
    char app[256];
} KeylogEntry;

int  keylog_open(void);
void keylog_close(void);
void keylog_insert(const char *key_name, int vk_code, const char *app);
int  keylog_query(const char *from, const char *to, const char *app,
                   KeylogEntry **out_entries);
int  keylog_get_apps(char ***out_apps, int *out_count);
void keylog_free_entries(KeylogEntry *entries);
void keylog_free_apps(char **apps, int count);
void keylog_delete_db(void);
int  keylog_is_open(void);
void keylog_queue_event(const char *key_name, int vk_code, const char *app);
void keylog_flush_events(void);

#endif
