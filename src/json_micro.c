#include "ksc_private.h"

static char *json_find_str(const char *json, const char *key)
{
    char search[128];
    sprintf(search, "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strstr(pos + strlen(search), "\"");
    if (!pos) return NULL;
    pos++;
    const char *end = strchr(pos, '"');
    if (!end) return NULL;
    int len = (int)(end - pos);
    if (len <= 0 || len > 1023) return NULL;
    static char buf[1024];
    memcpy(buf, pos, len);
    buf[len] = '\0';
    return buf;
}

static int json_find_int(const char *json, const char *key, int def)
{
    char search[128];
    sprintf(search, "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return def;
    pos = strstr(pos + strlen(search), ":");
    if (!pos) return def;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    return atoi(pos);
}
