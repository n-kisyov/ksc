#ifndef SSH_SYNC_H
#define SSH_SYNC_H

int  ssh_sync_is_configured(void);
int  ssh_sync_upload(const char *localPath, const char *remoteName);
void ssh_sync_test(HWND hParent);
void ssh_sync_load_config(HWND hHost, HWND hPort,
                           HWND hUser, HWND hPass);
void ssh_sync_save_config(HWND hHost, HWND hPort,
                           HWND hUser, HWND hPass);
void ssh_sync_init(void);

#endif
