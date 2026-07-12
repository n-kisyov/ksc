#include "ssh_sync.h"
#include "ksc_private.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dpapi.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

static char g_sshHost[256];
static char g_sshUser[128];
static int  g_sshPort = 22;

int ssh_sync_is_configured(void)
{
    return g_sshHost[0] != '\0';
}

static void get_ssh_dir(char *buf, int bufsz)
{
    char appdata[MAX_PATH];
    if (GetEnvironmentVariable("APPDATA", appdata, MAX_PATH) > 0)
        sprintf(buf, "%s\\KSC", appdata);
    else
        strcpy(buf, ".");
}

static void get_ssh_pass_path(char *buf, int bufsz)
{
    get_ssh_dir(buf, bufsz);
    strcat(buf, "\\ssh_pass.bin");
}

int ssh_sync_upload(const char *localPath, const char *remoteName)
{
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_SFTP *sftp = NULL;
    LIBSSH2_SFTP_HANDLE *handle = NULL;
    int result = 0;

    /* resolve host */
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    sprintf(portStr, "%d", g_sshPort);
    if (getaddrinfo(g_sshHost, portStr, &hints, &ai) != 0) goto done;

    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock == INVALID_SOCKET) { freeaddrinfo(ai); goto done; }

    /* set connect timeout via non-blocking + select */
    {
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
        connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
        fd_set wfds;
        struct timeval tv = {10, 0};
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        int sel = select(0, NULL, &wfds, NULL, &tv);
        mode = 0;
        ioctlsocket(sock, FIONBIO, &mode);
        if (sel <= 0) { closesocket(sock); freeaddrinfo(ai); goto done; }
    }
    freeaddrinfo(ai);

    session = libssh2_session_init();
    if (!session) { closesocket(sock); goto done; }

    if (libssh2_session_handshake(session, sock) != 0)
        goto done;

    /* accept any host key */
    libssh2_knownhost_init(session);

    /* authenticate with password */
    {
        char passPath[MAX_PATH];
        get_ssh_pass_path(passPath, sizeof(passPath));
        HANDLE hf = CreateFile(passPath, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        char password[256] = "";
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(hf, NULL);
            if (sz > 0 && sz < 4096) {
                BYTE *enc = malloc(sz);
                DWORD rd;
                ReadFile(hf, enc, sz, &rd, NULL);
                DATA_BLOB in  = { sz, enc };
                DATA_BLOB out = {0};
                if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                                       CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                    strncpy(password, (char *)out.pbData, 255);
                    password[255] = '\0';
                    LocalFree(out.pbData);
                }
                free(enc);
            }
            CloseHandle(hf);
        }
        if (password[0] == '\0') goto done;

        if (libssh2_userauth_password(session, g_sshUser, password) != 0)
            goto done;
    }

    sftp = libssh2_sftp_init(session);
    if (!sftp) goto done;

    /* create ksc-backups directory (ignore error if exists) */
    libssh2_sftp_mkdir(sftp, "ksc-backups",
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP |
        LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH);

    /* construct remote path */
    char remotePath[512];
    sprintf(remotePath, "ksc-backups/%s", remoteName);

    /* open local file */
    HANDLE hLocal = CreateFile(localPath, GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLocal == INVALID_HANDLE_VALUE) goto done;

    handle = libssh2_sftp_open(sftp, remotePath,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) { CloseHandle(hLocal); goto done; }

    {
        char buf[32768];
        DWORD rd = 0;
        int uploaded = 0;
        while (ReadFile(hLocal, buf, sizeof(buf), &rd, NULL) && rd > 0) {
            int written = 0;
            while (written < (int)rd) {
                int rc = libssh2_sftp_write(handle, buf + written,
                                            rd - written);
                if (rc < 0) { CloseHandle(hLocal); goto done; }
                written += rc;
                uploaded += rc;
            }
        }
    }

    CloseHandle(hLocal);
    result = 1;

done:
    if (handle)  libssh2_sftp_close(handle);
    if (sftp)    libssh2_sftp_shutdown(sftp);
    if (session) { libssh2_session_disconnect(session, "bye");
                   libssh2_session_free(session); }
    if (sock != INVALID_SOCKET) closesocket(sock);
    return result;
}

static DWORD WINAPI ssh_test_thread(LPVOID param)
{
    HWND hParent = (HWND)param;
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_SESSION *session = NULL;
    char *msg = "Connection failed: unknown error";

    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    sprintf(portStr, "%d", g_sshPort);
    if (getaddrinfo(g_sshHost, portStr, &hints, &ai) != 0) {
        msg = "Connection failed: host not found";
        goto fail;
    }
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock == INVALID_SOCKET) { freeaddrinfo(ai);
        msg = "Connection failed: socket error"; goto fail; }

    {
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
        connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
        fd_set wfds;
        struct timeval tv = {10, 0};
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        int sel = select(0, NULL, &wfds, NULL, &tv);
        mode = 0;
        ioctlsocket(sock, FIONBIO, &mode);
        if (sel <= 0) {
            closesocket(sock); freeaddrinfo(ai);
            msg = "Connection failed: timeout"; goto fail;
        }
    }
    freeaddrinfo(ai);

    session = libssh2_session_init();
    if (!session) { closesocket(sock);
        msg = "Connection failed: libssh2 init"; goto fail; }

    if (libssh2_session_handshake(session, sock) != 0) {
        msg = "Connection failed: SSH handshake"; goto fail;
    }

    {
        char passPath[MAX_PATH];
        get_ssh_pass_path(passPath, sizeof(passPath));
        HANDLE hf = CreateFile(passPath, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        char password[256] = "";
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(hf, NULL);
            if (sz > 0 && sz < 4096) {
                BYTE *enc = malloc(sz);
                DWORD rd;
                ReadFile(hf, enc, sz, &rd, NULL);
                DATA_BLOB in  = { sz, enc };
                DATA_BLOB out = {0};
                if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                                       CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                    strncpy(password, (char *)out.pbData, 255);
                    password[255] = '\0';
                    LocalFree(out.pbData);
                }
                free(enc);
            }
            CloseHandle(hf);
        }
        if (password[0] == '\0') {
            msg = "Connection failed: no password set"; goto fail;
        }
        if (libssh2_userauth_password(session, g_sshUser, password) != 0) {
            msg = "Connection failed: auth rejected"; goto fail;
        }
    }

    msg = "Connection OK";

fail:
    if (session) { libssh2_session_disconnect(session, "bye");
                   libssh2_session_free(session); }
    if (sock != INVALID_SOCKET) closesocket(sock);
    MessageBox(hParent, msg, "SSH Test", MB_OK | MB_ICONINFORMATION);
    return 0;
}

void ssh_sync_test(HWND hParent)
{
    CreateThread(NULL, 0, ssh_test_thread,
                  (LPVOID)hParent, 0, NULL);
}

void ssh_sync_load_config(HWND hHostEdit, HWND hPortEdit,
                           HWND hUserEdit, HWND hPassEdit)
{
    extern int db_get_setting_int(const char *k, int d);
    extern void db_get_db_dir(char *b, int s);

    /* load settings from DB (or use cached defaults) */
    char dir[MAX_PATH];
    db_get_db_dir(dir, sizeof(dir));

    /* load cached values */
    if (g_sshHost[0]) SetWindowText(hHostEdit, g_sshHost);
    if (g_sshUser[0]) SetWindowText(hUserEdit, g_sshUser);

    if (g_sshPort == 0) g_sshPort = db_get_setting_int("ssh_port", 22);
    char portStr[16];
    sprintf(portStr, "%d", g_sshPort);
    SetWindowText(hPortEdit, portStr);

    /* password field stays empty unless explicitly loaded by user */
}

void ssh_sync_save_config(HWND hHostEdit, HWND hPortEdit,
                           HWND hUserEdit, HWND hPassEdit)
{
    extern void db_set_setting_int(const char *k, int v);

    GetWindowText(hHostEdit, g_sshHost, sizeof(g_sshHost));
    GetWindowText(hUserEdit, g_sshUser, sizeof(g_sshUser));
    g_sshPort = GetDlgItemInt(GetParent(hHostEdit),
        GetWindowLong(hHostEdit, GWL_ID), NULL, FALSE);
    (void)hPortEdit; (void)hPortEdit;
    /* port is read directly below */

    char portStr[32];
    GetWindowText(hPortEdit, portStr, sizeof(portStr));
    g_sshPort = atoi(portStr);
    if (g_sshPort == 0) g_sshPort = 22;

    db_set_setting_int("ssh_port", g_sshPort);

    /* save password with DPAPI */
    char pass[256];
    GetWindowText(hPassEdit, pass, sizeof(pass));
    if (pass[0]) {
        char passPath[MAX_PATH];
        get_ssh_pass_path(passPath, sizeof(passPath));
        DATA_BLOB in  = { (DWORD)strlen(pass) + 1, (BYTE *)pass };
        DATA_BLOB out = {0};
        if (CryptProtectData(&in, L"ksc ssh pass", NULL, NULL, NULL,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            HANDLE hf = CreateFile(passPath, GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteFile(hf, out.pbData, out.cbData, &written, NULL);
                CloseHandle(hf);
            }
            LocalFree(out.pbData);
        }
    }

    MessageBox(GetParent(hHostEdit), "SSH config saved.",
               "SSH Config", MB_OK | MB_ICONINFORMATION);
}
