#include "ksc_private.h"

[[nodiscard]] bool json_find_str_buf(const char *json, const char *key,
                                     char *out, int out_sz)
{
    if (!out || out_sz <= 0) return false;
    out[0] = '\0';
    if (!json || !key) return false;

    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return false;
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return false;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != '"') return false;
    pos++;
    const char *end = pos;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1] != '\0') end += 2;
        else end++;
    }
    if (*end != '"') return false;
    int len = (int)(end - pos);
    if (len <= 0) return false;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, pos, (size_t)len);
    out[len] = '\0';
    return true;
}

[[nodiscard]] char *json_find_str(const char *json, const char *key)
{
    static _Thread_local char buf[1024];
    if (!json_find_str_buf(json, key, buf, sizeof(buf)))
        return nullptr;
    return buf;
}

int json_find_int(const char *json, const char *key, int def)
{
    if (!json || !key) return def;
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return def;
    pos = strstr(pos + strlen(search), ":");
    if (!pos) return def;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    return atoi(pos);
}
