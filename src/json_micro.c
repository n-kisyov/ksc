#include "ksc_private.h"

static const char *find_json_key_value(const char *json, const char *key)
{
    if (!json || !key || !key[0]) return nullptr;
    char search[128];
    int sn = snprintf(search, sizeof(search), "\"%s\"", key);
    if (sn <= 0 || sn >= (int)sizeof(search)) return nullptr;

    const char *cur = json;
    while ((cur = strstr(cur, search)) != nullptr) {
        const char *after = cur + (size_t)sn;
        const char *ws = after;
        while (*ws == ' ' || *ws == '\t' || *ws == '\r' || *ws == '\n') ws++;
        if (*ws == ':') {
            ws++;
            while (*ws == ' ' || *ws == '\t' || *ws == '\r' || *ws == '\n') ws++;
            return ws;
        }
        cur = after;
    }
    return nullptr;
}

[[nodiscard]] bool json_find_str_buf(const char *json, const char *key,
                                     char *out, int out_sz)
{
    if (!out || out_sz <= 0) return false;
    out[0] = '\0';

    const char *pos = find_json_key_value(json, key);
    if (!pos || *pos != '"') return false;
    pos++;

    const char *end = pos;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1] != '\0') end += 2;
        else end++;
    }
    if (*end != '"') return false;

    int w = 0;
    for (const char *p = pos; p < end && w < out_sz - 1; ) {
        if (*p == '\\' && p + 1 < end) {
            char esc = p[1];
            switch (esc) {
            case '"':  out[w++] = '"';  break;
            case '\\': out[w++] = '\\'; break;
            case '/':  out[w++] = '/';  break;
            case 'n':  out[w++] = '\n'; break;
            case 'r':  out[w++] = '\r'; break;
            case 't':  out[w++] = '\t'; break;
            case 'b':  out[w++] = '\b'; break;
            case 'f':  out[w++] = '\f'; break;
            default:   out[w++] = esc;  break;
            }
            p += 2;
        } else {
            out[w++] = *p++;
        }
    }
    out[w] = '\0';
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
    const char *pos = find_json_key_value(json, key);
    if (!pos) return def;
    return atoi(pos);
}
