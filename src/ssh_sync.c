#include <winsock2.h>
#include <ws2tcpip.h>
#include "ssh_sync.h"
#include "database.h"
#include "ksc_private.h"
#include <dpapi.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

static WSADATA g_wsaData;
static int g_wsaInit = 0;

static void ensure_winsock(void)
{
    if (!g_wsaInit) {
        WSAStartup(MAKEWORD(2, 2), &g_wsaData);
        libssh2_init(0);
        g_wsaInit = 1;
    }
}

static char g_sshHost[256];
static char g_sshUser[128];
static int  g_sshPort = 22;

bool ssh_sync_is_configured(void)
{
    return g_sshHost[0] != '\0';
}

void ssh_sync_init(void)
{
    ensure_winsock();
    db_get_setting_str("ssh_host", "", g_sshHost, sizeof(g_sshHost));
    db_get_setting_str("ssh_user", "", g_sshUser, sizeof(g_sshUser));
    g_sshPort = db_get_setting_int("ssh_port", 22);
}

static void set_ssh_preferences(LIBSSH2_SESSION *session)
{
    libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX,
        "curve25519-sha256,curve25519-sha256@libssh.org,"
        "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
        "diffie-hellman-group16-sha512,diffie-hellman-group14-sha256,"
        "diffie-hellman-group14-sha1,diffie-hellman-group1-sha1");

    libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS,
        "chacha20-poly1305@openssh.com,"
        "aes256-gcm@openssh.com,aes128-gcm@openssh.com,"
        "aes256-ctr,aes192-ctr,aes128-ctr");

    libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC,
        "chacha20-poly1305@openssh.com,"
        "aes256-gcm@openssh.com,aes128-gcm@openssh.com,"
        "aes256-ctr,aes192-ctr,aes128-ctr");

    libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_CS,
        "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"
        "hmac-sha2-256,hmac-sha2-512,hmac-sha1");

    libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_SC,
        "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"
        "hmac-sha2-256,hmac-sha2-512,hmac-sha1");
}

static void get_ssh_pass_path(char *buf, int bufsz)
{
    char dir[MAX_PATH];
    db_get_db_dir(dir, sizeof(dir));
    snprintf(buf, (size_t)bufsz, "%s\\ssh_pass.bin", dir);
}

static int read_ssh_password(char *password, int passSize)
{
    password[0] = '\0';
    char passPath[MAX_PATH];
    get_ssh_pass_path(passPath, sizeof(passPath));

    HANDLE hf = CreateFile(passPath, GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return 0;

    DWORD sz = GetFileSize(hf, nullptr);
    if (sz > 0 && sz < 4096) {
        BYTE *enc = malloc(sz);
        if (enc) {
            DWORD rd = 0;
            if (ReadFile(hf, enc, sz, &rd, nullptr) && rd > 0) {
                DATA_BLOB in  = { rd, enc };
                DATA_BLOB out = {};
                if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                       CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                    strncpy(password, (char *)out.pbData, (size_t)(passSize - 1));
                    password[passSize - 1] = '\0';
                    LocalFree(out.pbData);
                }
            }
            free(enc);
        }
    }
    CloseHandle(hf);
    return password[0] != '\0';
}

static int ssh_connect_and_auth(SOCKET *outSock, LIBSSH2_SESSION **outSession,
                                const char **outErrMsg)
{
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_SESSION *session = nullptr;
    const char *err = "Connection failed: unknown error";

    *outSock = INVALID_SOCKET;
    *outSession = nullptr;
    if (outErrMsg) *outErrMsg = err;

    ensure_winsock();

    struct addrinfo hints = {}, *ai = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", g_sshPort);
    if (getaddrinfo(g_sshHost, portStr, &hints, &ai) != 0) {
        err = "Connection failed: host not found";
        goto fail;
    }

    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(ai);
        err = "Connection failed: socket error";
        goto fail;
    }

    {
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
        connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
        fd_set wfds, efds;
        struct timeval tv = {10, 0};
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(sock, &wfds);
        FD_SET(sock, &efds);
        int sel = select(0, nullptr, &wfds, &efds, &tv);
        mode = 0;
        ioctlsocket(sock, FIONBIO, &mode);
        freeaddrinfo(ai);
        ai = nullptr;

        if (sel <= 0 || FD_ISSET(sock, &efds)) {
            err = "Connection failed: timeout or refused";
            goto fail;
        }
        int soErr = 0;
        int soLen = (int)sizeof(soErr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&soErr, &soLen) != 0 ||
            soErr != 0) {
            err = "Connection failed: unreachable or refused";
            goto fail;
        }
    }

    session = libssh2_session_init();
    if (!session) {
        err = "Connection failed: libssh2 init";
        goto fail;
    }

    set_ssh_preferences(session);

    if (libssh2_session_handshake(session, sock) != 0) {
        err = "Connection failed: SSH handshake";
        goto fail;
    }

    {
        char password[256] = "";
        if (!read_ssh_password(password, sizeof(password))) {
            err = "Connection failed: no password set";
            goto fail;
        }
        if (libssh2_userauth_password(session, g_sshUser, password) != 0) {
            SecureZeroMemory(password, sizeof(password));
            err = "Connection failed: auth rejected";
            goto fail;
        }
        SecureZeroMemory(password, sizeof(password));
    }

    *outSock = sock;
    *outSession = session;
    if (outErrMsg) *outErrMsg = "Connection OK";
    return 1;

fail:
    if (session) {
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session);
    }
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    if (outErrMsg) *outErrMsg = err;
    return 0;
}

int ssh_sync_upload(const char *localPath, const char *remoteName)
{
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_SESSION *session = nullptr;
    LIBSSH2_SFTP *sftp = nullptr;
    LIBSSH2_SFTP_HANDLE *handle = nullptr;
    HANDLE hLocal = INVALID_HANDLE_VALUE;
    int result = 0;

    if (!ssh_connect_and_auth(&sock, &session, nullptr))
        return 0;

    sftp = libssh2_sftp_init(session);
    if (!sftp) goto done;

    /* create ksc-backups directory (ignore error if exists) */
    libssh2_sftp_mkdir(sftp, "ksc-backups",
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP |
        LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH);

    char remotePath[512];
    snprintf(remotePath, sizeof(remotePath), "ksc-backups/%s", remoteName);

    hLocal = CreateFile(localPath, GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLocal == INVALID_HANDLE_VALUE) goto done;

    handle = libssh2_sftp_open(sftp, remotePath,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) goto done;

    {
        char buf[32768];
        DWORD rd = 0;
        while (ReadFile(hLocal, buf, sizeof(buf), &rd, nullptr) && rd > 0) {
            int written = 0;
            while (written < (int)rd) {
                int rc = libssh2_sftp_write(handle, buf + written,
                                            rd - (size_t)written);
                if (rc < 0) goto done;
                written += rc;
            }
        }
    }

    result = 1;

done:
    if (hLocal != INVALID_HANDLE_VALUE) CloseHandle(hLocal);
    if (handle)  libssh2_sftp_close(handle);
    if (sftp)    libssh2_sftp_shutdown(sftp);
    if (session) {
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session);
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    return result;
}

static DWORD WINAPI ssh_test_thread(LPVOID param)
{
    HWND hParent = (HWND)param;
    SOCKET sock = INVALID_SOCKET;
    LIBSSH2_SESSION *session = nullptr;
    const char *msg = "Connection failed: unknown error";

    int ok = ssh_connect_and_auth(&sock, &session, &msg);
    if (session) {
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session);
    }
    if (sock != INVALID_SOCKET) closesocket(sock);

    MessageBox(hParent, msg, "SSH Test",
               MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    return 0;
}

void ssh_sync_test(HWND hParent)
{
    HANDLE h = CreateThread(nullptr, 0, ssh_test_thread,
                             (LPVOID)hParent, 0, nullptr);
    if (h) CloseHandle(h);
}

void ssh_sync_load_config(HWND hHostEdit, HWND hPortEdit,
                           HWND hUserEdit, [[maybe_unused]] HWND hPassEdit)
{
    if (!g_sshHost[0])
        db_get_setting_str("ssh_host", "", g_sshHost, sizeof(g_sshHost));
    if (!g_sshUser[0])
        db_get_setting_str("ssh_user", "", g_sshUser, sizeof(g_sshUser));
    if (g_sshPort == 0)
        g_sshPort = db_get_setting_int("ssh_port", 22);

    SetWindowText(hHostEdit, g_sshHost);
    SetWindowText(hUserEdit, g_sshUser);
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", g_sshPort);
    SetWindowText(hPortEdit, portStr);
}

void ssh_sync_save_config(HWND hHostEdit, HWND hPortEdit,
                           HWND hUserEdit, HWND hPassEdit)
{
    GetWindowText(hHostEdit, g_sshHost, sizeof(g_sshHost));
    GetWindowText(hUserEdit, g_sshUser, sizeof(g_sshUser));

    char portStr[32];
    GetWindowText(hPortEdit, portStr, sizeof(portStr));
    g_sshPort = atoi(portStr);
    if (g_sshPort <= 0 || g_sshPort > 65535) g_sshPort = 22;

    db_set_setting_str("ssh_host", g_sshHost);
    db_set_setting_str("ssh_user", g_sshUser);
    db_set_setting_int("ssh_port", g_sshPort);

    /* save password with DPAPI */
    char pass[256];
    GetWindowText(hPassEdit, pass, sizeof(pass));
    if (pass[0]) {
        char passPath[MAX_PATH];
        get_ssh_pass_path(passPath, sizeof(passPath));
        DATA_BLOB in  = { (DWORD)strlen(pass) + 1, (BYTE *)pass };
        DATA_BLOB out = {};
        if (CryptProtectData(&in, L"ksc ssh pass", nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            HANDLE hf = CreateFile(passPath, GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteFile(hf, out.pbData, out.cbData, &written, nullptr);
                CloseHandle(hf);
            }
            LocalFree(out.pbData);
        }
        SecureZeroMemory(pass, sizeof(pass));
    }

    MessageBox(GetParent(hHostEdit), "SSH config saved.",
               "SSH Config", MB_OK | MB_ICONINFORMATION);
}
