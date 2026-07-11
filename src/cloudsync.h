#ifndef CLOUDSYNC_H
#define CLOUDSYNC_H

#include <windows.h>

typedef struct {
    char ts[32];
    char files[256];
    int  size;
    char status[16];
} CloudSyncEntry;

int  cloudsync_is_logged_in(void);
void cloudsync_get_email(char *buf, int bufsize);
void cloudsync_login(HWND hParent);
void cloudsync_logout(void);
void cloudsync_backup_trigger(HWND hCloudWnd);
int  cloudsync_load_history(CloudSyncEntry **out, int *count);
void cloudsync_free_history(CloudSyncEntry *e);
int  cloudsync_get_schedule(void);
void cloudsync_set_schedule(int value);
void cloudsync_init(void);

#endif
