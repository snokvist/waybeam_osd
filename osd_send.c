/*
 * waybeam.c - bare-bones UDP OSD sender + ini file watcher
 *
 * Contract (updated semantics):
 *   - UDP datagram containing JSON object:
 *       { "values":[...], "texts":[...] }
 *
 *   - Arrays are positional. Backend behavior:
 *       * null entries are ignored (slot keeps previous)
 *       * omitted trailing indices keep previous content
 *       * empty string "" clears:
 *           - text slot cleared to ""
 *           - numeric slot cleared to 0 (backend handles this)
 *
 *   - To update index N without touching 0..N-1, send null placeholders:
 *       {"values":[null,null,0.9]}
 *
 * Implementation notes:
 *   - We build sparse arrays up to highest index that is being updated in THIS packet.
 *   - For indices inside the array that are not explicitly set, we emit null (ignore).
 *   - For values: we can emit number, null, or "" (empty string) for clear.
 *   - For texts: we can emit string (possibly ""), or null.
 *
 * Defaults:
 *   --ini  /tmp/aalink_ext.msg
 *   --dest 127.0.0.1
 *   --port 7777
 *
 * Flags:
 *   --values "i=v,..."   where v can be:
 *       - literal number (e.g. 81)
 *       - @ini_key (e.g. @used_rssi)
 *       - null (ignored)
 *       - empty (i=) => "" (clear numeric slot to 0 on backend)
 *
 *   --texts  "i=s,..."   where s can be:
 *       - literal text
 *       - @ini_key
 *       - null (ignored)
 *       - empty (i=) => "" (clear text slot)
 *
 * Missing @ini_key handling (send & watch):
 *   - missing key => null (ignored) (script-friendly)
 *
 * Watch mode:
 *   - Poll ini file every --interval ms (default 64)
 *   - Sends initial baseline for all watched indices
 *   - On change: sends only changed indices (positional arrays with null padding)
 *   - If ini key disappears => null (ignored)
 *   - If ini key becomes empty (key=) => "" (clear)
 *
 * Verbose:
 *   - --verbose / -v prints details about what is being sent and why.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o waybeam waybeam.c
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>


#define MAX_PAYLOAD      1280
#define BUILD_BUF        1900

#define MAX_TEXT_LEN     96

#define INI_MAX_KV       512
#define INI_KEY_MAX      64
#define INI_VAL_MAX      256

#define MAX_INI_PATHS    32
#define MAX_CPU_CORES    128
#define INI_PATH_MAX     256
#define DEFAULT_CFG_PATH "osd_send.json"
#define DEFAULT_DEST_IP  "127.0.0.1"
#define DEFAULT_PORT     7777
#define DEFAULT_INTERVAL 64
#define DEFAULT_RETRY_MS 5000

typedef struct {
    char key[INI_KEY_MAX];
    char val[INI_VAL_MAX];
} IniKV;

typedef struct {
    IniKV kv[INI_MAX_KV];
    int count;
    int loaded;
} IniStore;

typedef struct {
    uint64_t total;
    uint64_t idle;
} CpuSample;

typedef struct {
    int valid;
    int core_count;
    CpuSample total_prev;
    CpuSample core_prev[MAX_CPU_CORES];
} CpuStatsState;

typedef struct {
    int enabled;
    int paths_count;
    char paths[MAX_INI_PATHS][INI_PATH_MAX];
} IniSourceConfig;

typedef struct {
    int enabled;
    char iface[64];
    char sta[64];
} HostapdSourceConfig;

typedef struct {
    int enabled;
    char iface[64];
} IfaceSourceConfig;

typedef struct {
    int enabled;
} CpuSourceConfig;

typedef struct {
    int enabled;
    char url[256];
} VencSourceConfig;

typedef struct {
    char dest[64];
    char port[64];
} NetworkConfig;

typedef struct {
    int interval_ms;
    int retry_ms;
} WatchConfig;

typedef struct {
    int verbose;
    int print_json;
} RuntimeConfig;

typedef struct {
    int value_used[8];
    char value_rhs[8][INI_VAL_MAX];
    int text_used[8];
    char text_rhs[8][INI_VAL_MAX];
} PayloadConfig;

typedef struct {
    NetworkConfig network;
    WatchConfig watch;
    RuntimeConfig runtime;
    IniSourceConfig ini;
    HostapdSourceConfig hostapd;
    IfaceSourceConfig wpa;
    IfaceSourceConfig rtl8812eu;
    CpuSourceConfig cpu;
    VencSourceConfig venc;
    PayloadConfig payload;
} OsdSendConfig;

/* Values can be: absent (not included), null (ignored), number, or "" (clear-to-0 on backend) */
typedef enum {
    VS_ABSENT = 0,
    VS_NULL   = 1,
    VS_NUM    = 2,
    VS_EMPTY  = 3, /* emit "" (JSON string) */
} ValueState;

/* Texts can be: absent, null (ignored), or string (possibly empty) */
typedef enum {
    TS_ABSENT = 0,
    TS_NULL   = 1,
    TS_STR    = 2,
} TextState;

typedef struct {
    ValueState values_state[8];
    double values[8];

    TextState texts_state[8];
    char texts[8][MAX_TEXT_LEN + 1];
} Payload;

static void usage_main(const char *prog);

/* ------------------------- helpers ------------------------- */

static char *trim(char *s)
{
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

static void copy_cstr(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int parse_int(const char *s, int *out)
{
    if (!s) return 0;
    char tmp[64];
    copy_cstr(tmp, sizeof(tmp), s);
    char *t = trim(tmp);

    if (!*t) return 0;
    char *end = NULL;
    long v = strtol(t, &end, 0);
    if (*end != '\0') return 0;
    if (v < -2147483648L || v > 2147483647L) return 0;
    *out = (int)v;
    return 1;
}

static int parse_double(const char *s, double *out)
{
    if (!s) return 0;
    char tmp[128];
    copy_cstr(tmp, sizeof(tmp), s);
    char *t = trim(tmp);

    if (!*t) return 0;
    char *end = NULL;
    double v = strtod(t, &end);
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

static int is_literal_null(const char *s)
{
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    return (!strcasecmp(s, "null"));
}

static void clamp_textN(const char *in, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (!in) { out[0] = '\0'; return; }
    size_t n = strlen(in);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

/* Minimal JSON escape */
static void json_escape(const char *in, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    size_t w = 0;
    if (!in) { out[0] = '\0'; return; }

    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        const char *rep = NULL;
        char tmp[7];

        if (c == '\\') rep = "\\\\";
        else if (c == '\"') rep = "\\\"";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
            rep = tmp;
        }

        if (rep) {
            size_t rl = strlen(rep);
            if (w + rl + 1 >= out_sz) break;
            memcpy(out + w, rep, rl);
            w += rl;
        } else {
            if (w + 2 >= out_sz) break;
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

static int appendf(char *buf, size_t cap, int *len, const char *fmt, ...)
{
    if (*len < 0) return 0;
    if ((size_t)*len >= cap) return 0;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - (size_t)*len, fmt, ap);
    va_end(ap);

    if (n < 0) return 0;
    if (*len + n >= (int)cap) return 0;
    *len += n;
    return 1;
}

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static int read_file_all(const char *path, char **out_buf)
{
    if (!path || !out_buf) return 0;
    *out_buf = NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return 0;
    }

    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        return 0;
    }
    buf[got] = '\0';
    *out_buf = buf;
    return 1;
}

static const char *json_skip_ws_range(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *json_find_matching(const char *start, const char *end, char open_ch, char close_ch)
{
    int depth = 0;
    int in_str = 0;
    int esc = 0;

    for (const char *p = start; p < end; p++) {
        char c = *p;
        if (in_str) {
            if (esc) {
                esc = 0;
                continue;
            }
            if (c == '\\') {
                esc = 1;
                continue;
            }
            if (c == '"') in_str = 0;
            continue;
        }

        if (c == '"') {
            in_str = 1;
            continue;
        }
        if (c == open_ch) {
            depth++;
            continue;
        }
        if (c == close_ch) {
            depth--;
            if (depth == 0) return p;
        }
    }
    return NULL;
}

static int json_parse_string_token(const char **pp, const char *end, char *out, size_t out_sz);

static const char *json_skip_value(const char *p, const char *end)
{
    p = json_skip_ws_range(p, end);
    if (p >= end) return NULL;

    if (*p == '"') {
        char tmp[2];
        const char *q = p;
        if (!json_parse_string_token(&q, end, tmp, sizeof(tmp))) return NULL;
        return q;
    }
    if (*p == '{') {
        const char *m = json_find_matching(p, end, '{', '}');
        if (!m) return NULL;
        return m + 1;
    }
    if (*p == '[') {
        const char *m = json_find_matching(p, end, '[', ']');
        if (!m) return NULL;
        return m + 1;
    }

    const char *q = p;
    while (q < end && !strchr(",}] \t\r\n", *q)) q++;
    if (q == p) return NULL;
    return q;
}

static const char *json_find_key_value_start(const char *start, const char *end, const char *key)
{
    if (!start || !end || !key || start >= end) return NULL;

    const char *p = json_skip_ws_range(start, end);
    while (p < end) {
        p = json_skip_ws_range(p, end);
        if (p >= end) break;
        if (*p == ',') {
            p = json_skip_ws_range(p + 1, end);
            continue;
        }
        if (*p != '"') return NULL;

        char kbuf[128];
        if (!json_parse_string_token(&p, end, kbuf, sizeof(kbuf))) return NULL;
        p = json_skip_ws_range(p, end);
        if (p >= end || *p != ':') return NULL;
        p++;
        p = json_skip_ws_range(p, end);

        const char *val_start = p;
        const char *val_end = json_skip_value(p, end);
        if (!val_end) return NULL;

        if (!strcmp(kbuf, key)) return val_start;

        p = json_skip_ws_range(val_end, end);
        if (p < end && *p == ',') p++;
    }
    return NULL;
}

static int json_key_allowed(const char *key, const char *const *allowed, int allowed_count)
{
    for (int i = 0; i < allowed_count; i++) {
        if (!strcmp(key, allowed[i])) return 1;
    }
    return 0;
}

static int json_validate_object_keys(const char *start, const char *end,
    const char *obj_name, const char *const *allowed, int allowed_count)
{
    const char *p = start;
    while (p < end) {
        p = json_skip_ws_range(p, end);
        if (p >= end) break;
        if (*p == ',') {
            p = json_skip_ws_range(p + 1, end);
            continue;
        }
        if (*p != '"') {
            fprintf(stderr, "Error: invalid JSON object format in %s\n", obj_name);
            return 0;
        }

        char kbuf[128];
        if (!json_parse_string_token(&p, end, kbuf, sizeof(kbuf))) {
            fprintf(stderr, "Error: invalid key string in %s\n", obj_name);
            return 0;
        }
        if (!json_key_allowed(kbuf, allowed, allowed_count)) {
            fprintf(stderr, "Error: unknown key '%s' in %s\n", kbuf, obj_name);
            return 0;
        }

        p = json_skip_ws_range(p, end);
        if (p >= end || *p != ':') {
            fprintf(stderr, "Error: invalid key/value separator in %s\n", obj_name);
            return 0;
        }
        p++;
        const char *next = json_skip_value(p, end);
        if (!next) {
            fprintf(stderr, "Error: invalid value in %s.%s\n", obj_name, kbuf);
            return 0;
        }

        p = json_skip_ws_range(next, end);
        if (p < end && *p == ',') p++;
    }

    return 1;
}

static int json_get_object_range(const char *start, const char *end, const char *key, const char **obj_start, const char **obj_end)
{
    const char *p = json_find_key_value_start(start, end, key);
    if (!p || *p != '{') return -1;
    const char *m = json_find_matching(p, end, '{', '}');
    if (!m) return -1;
    *obj_start = p + 1;
    *obj_end = m;
    return 0;
}

static int json_get_array_range(const char *start, const char *end, const char *key, const char **arr_start, const char **arr_end)
{
    const char *p = json_find_key_value_start(start, end, key);
    if (!p || *p != '[') return -1;
    const char *m = json_find_matching(p, end, '[', ']');
    if (!m) return -1;
    *arr_start = p + 1;
    *arr_end = m;
    return 0;
}

static int json_parse_string_token(const char **pp, const char *end, char *out, size_t out_sz)
{
    if (!pp || !*pp || !out || out_sz == 0) return 0;
    const char *p = *pp;
    if (p >= end || *p != '"') return 0;
    p++;

    size_t w = 0;
    while (p < end) {
        char c = *p++;
        if (c == '"') {
            out[w] = '\0';
            *pp = p;
            return 1;
        }
        if (c == '\\') {
            if (p >= end) return 0;
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == '"' || e == '\\' || e == '/') c = e;
            else c = e;
        }
        if (w + 1 < out_sz) out[w++] = c;
    }
    return 0;
}

static int json_get_bool_range(const char *start, const char *end, const char *key, int *out, int *found)
{
    if (found) *found = 0;
    const char *p = json_find_key_value_start(start, end, key);
    if (!p) return -1;
    if (found) *found = 1;
    if ((end - p) >= 4 && !memcmp(p, "true", 4)) {
        const char *q = p + 4;
        if (q >= end || *q == ',' || *q == '}' || *q == ']' || isspace((unsigned char)*q)) {
            *out = 1;
            return 0;
        }
    }
    if ((end - p) >= 5 && !memcmp(p, "false", 5)) {
        const char *q = p + 5;
        if (q >= end || *q == ',' || *q == '}' || *q == ']' || isspace((unsigned char)*q)) {
            *out = 0;
            return 0;
        }
    }
    return -1;
}

static int json_get_int_range_cfg(const char *start, const char *end, const char *key, int *out, int *found)
{
    if (found) *found = 0;
    const char *p = json_find_key_value_start(start, end, key);
    if (!p) return -1;
    if (found) *found = 1;

    const char *q = p;
    while (q < end && !strchr(",}] \t\r\n", *q)) q++;
    size_t n = (size_t)(q - p);
    if (n == 0 || n >= 64) return -1;

    char tmp[64];
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    return parse_int(tmp, out) ? 0 : -1;
}

static int json_get_string_range_cfg(const char *start, const char *end, const char *key, char *out, size_t out_sz, int *found)
{
    if (found) *found = 0;
    const char *p = json_find_key_value_start(start, end, key);
    if (!p) return -1;
    if (found) *found = 1;
    if (*p != '"') return -1;
    return json_parse_string_token(&p, end, out, out_sz) ? 0 : -1;
}

static int json_get_scalar_as_string_range(const char *start, const char *end, const char *key, char *out, size_t out_sz, int *found)
{
    if (found) *found = 0;
    const char *p = json_find_key_value_start(start, end, key);
    if (!p) return -1;
    if (found) *found = 1;

    if (*p == '"') {
        return json_parse_string_token(&p, end, out, out_sz) ? 0 : -1;
    }

    const char *q = p;
    while (q < end && !strchr(",}] \t\r\n", *q)) q++;
    size_t n = (size_t)(q - p);
    if (n == 0 || n >= out_sz) return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int json_get_string_array_range_cfg(const char *start, const char *end, const char *key,
    char out[][INI_PATH_MAX], int max_count, int *out_count, int *found)
{
    if (found) *found = 0;
    if (out_count) *out_count = 0;

    const char *arr_s = NULL;
    const char *arr_e = NULL;
    if (json_get_array_range(start, end, key, &arr_s, &arr_e) != 0) return -1;
    if (found) *found = 1;

    int idx = 0;
    const char *p = arr_s;
    while (p < arr_e) {
        p = json_skip_ws_range(p, arr_e);
        if (p >= arr_e) break;
        if (*p == ',') { p++; continue; }

        if (idx >= max_count) return -1;
        if (*p != '"') return -1;
        if (!json_parse_string_token(&p, arr_e, out[idx], INI_PATH_MAX)) return -1;
        idx++;

        p = json_skip_ws_range(p, arr_e);
        if (p < arr_e && *p == ',') p++;
    }

    if (out_count) *out_count = idx;
    return 0;
}


static int json_get_payload_rhs_array(const char *start, const char *end, const char *key,
    int used[8], char rhs[8][INI_VAL_MAX], int *found)
{
    if (found) *found = 0;
    for (int i = 0; i < 8; i++) {
        used[i] = 0;
        rhs[i][0] = '\0';
    }

    const char *arr_s = NULL;
    const char *arr_e = NULL;
    if (json_get_array_range(start, end, key, &arr_s, &arr_e) != 0) return -1;
    if (found) *found = 1;

    int idx = 0;
    const char *p = arr_s;
    while (p < arr_e) {
        p = json_skip_ws_range(p, arr_e);
        if (p >= arr_e) break;
        if (*p == ',') { p++; continue; }
        if (idx >= 8) return -1;

        if (!strncmp(p, "null", 4)) {
            p += 4;
            used[idx] = 0;
            rhs[idx][0] = '\0';
        } else if (*p == '"') {
            if (!json_parse_string_token(&p, arr_e, rhs[idx], INI_VAL_MAX)) return -1;
            used[idx] = 1;
        } else {
            const char *q = p;
            while (q < arr_e && !strchr(",] \t\r\n", *q)) q++;
            size_t n = (size_t)(q - p);
            if (n == 0 || n >= INI_VAL_MAX) return -1;
            memcpy(rhs[idx], p, n);
            rhs[idx][n] = '\0';
            used[idx] = 1;
            p = q;
        }

        idx++;
        p = json_skip_ws_range(p, arr_e);
        if (p < arr_e && *p == ',') p++;
    }

    return 0;
}

static void cfg_defaults(OsdSendConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    copy_cstr(cfg->network.dest, sizeof(cfg->network.dest), DEFAULT_DEST_IP);
    snprintf(cfg->network.port, sizeof(cfg->network.port), "%d", DEFAULT_PORT);

    cfg->watch.interval_ms = DEFAULT_INTERVAL;
    cfg->watch.retry_ms = DEFAULT_RETRY_MS;

    cfg->runtime.verbose = 0;
    cfg->runtime.print_json = 0;

    copy_cstr(cfg->venc.url, sizeof(cfg->venc.url), "http://127.0.0.1/metrics");
}

static int cfg_parse_file(const char *path, OsdSendConfig *cfg)
{
    if (!path || !cfg) return 0;
    cfg_defaults(cfg);

    char *json = NULL;
    if (!read_file_all(path, &json)) {
        fprintf(stderr, "Error: cannot read config %s\n", path);
        return 0;
    }

    const char *doc_s = json;
    const char *doc_e = json + strlen(json);
    doc_s = json_skip_ws_range(doc_s, doc_e);
    if (doc_s >= doc_e || *doc_s != '{') {
        fprintf(stderr, "Error: config root must be a JSON object\n");
        free(json);
        return 0;
    }
    const char *doc_obj_end = json_find_matching(doc_s, doc_e, '{', '}');
    if (!doc_obj_end) {
        fprintf(stderr, "Error: invalid JSON object in config\n");
        free(json);
        return 0;
    }
    const char *doc_after = json_skip_ws_range(doc_obj_end + 1, doc_e);
    if (doc_after != doc_e) {
        fprintf(stderr, "Error: trailing content after JSON object in config\n");
        free(json);
        return 0;
    }

    const char *root_s = doc_s + 1;
    const char *root_e = doc_obj_end;

    static const char *const root_allowed[] = {
        "network", "runtime", "watch", "sources", "payload"
    };
    if (!json_validate_object_keys(root_s, root_e, "root", root_allowed, (int)(sizeof(root_allowed) / sizeof(root_allowed[0])))) {
        free(json);
        return 0;
    }

    const char *network_s = NULL, *network_e = NULL;
    if (json_get_object_range(root_s, root_e, "network", &network_s, &network_e) == 0) {
        static const char *const network_allowed[] = { "dest", "port" };
        if (!json_validate_object_keys(network_s, network_e, "network", network_allowed, 2)) {
            free(json);
            return 0;
        }
        int found = 0;
        if (json_get_scalar_as_string_range(network_s, network_e, "dest", cfg->network.dest, sizeof(cfg->network.dest), &found) == 0 && found) {
            /* parsed */
        } else if (found) {
            fprintf(stderr, "Error: invalid network.dest\n");
            free(json);
            return 0;
        }

        found = 0;
        if (json_get_scalar_as_string_range(network_s, network_e, "port", cfg->network.port, sizeof(cfg->network.port), &found) == 0 && found) {
            /* parsed */
        } else if (found) {
            fprintf(stderr, "Error: invalid network.port\n");
            free(json);
            return 0;
        }
    }

    const char *runtime_s = NULL, *runtime_e = NULL;
    if (json_get_object_range(root_s, root_e, "runtime", &runtime_s, &runtime_e) == 0) {
        static const char *const runtime_allowed[] = { "verbose", "print_json" };
        if (!json_validate_object_keys(runtime_s, runtime_e, "runtime", runtime_allowed, 2)) {
            free(json);
            return 0;
        }
        int found = 0;
        int b = 0;
        if (json_get_bool_range(runtime_s, runtime_e, "verbose", &b, &found) == 0 && found) cfg->runtime.verbose = b;
        else if (found) {
            fprintf(stderr, "Error: invalid runtime.verbose\n");
            free(json);
            return 0;
        }
        found = 0;
        if (json_get_bool_range(runtime_s, runtime_e, "print_json", &b, &found) == 0 && found) cfg->runtime.print_json = b;
        else if (found) {
            fprintf(stderr, "Error: invalid runtime.print_json\n");
            free(json);
            return 0;
        }
    }

    const char *watch_s = NULL, *watch_e = NULL;
    if (json_get_object_range(root_s, root_e, "watch", &watch_s, &watch_e) == 0) {
        static const char *const watch_allowed[] = { "interval_ms", "retry_ms" };
        if (!json_validate_object_keys(watch_s, watch_e, "watch", watch_allowed, 2)) {
            free(json);
            return 0;
        }
        int found = 0;
        int v = 0;
        if (json_get_int_range_cfg(watch_s, watch_e, "interval_ms", &v, &found) == 0 && found) cfg->watch.interval_ms = v;
        else if (found) {
            fprintf(stderr, "Error: invalid watch.interval_ms\n");
            free(json);
            return 0;
        }
        found = 0;
        if (json_get_int_range_cfg(watch_s, watch_e, "retry_ms", &v, &found) == 0 && found) cfg->watch.retry_ms = v;
        else if (found) {
            fprintf(stderr, "Error: invalid watch.retry_ms\n");
            free(json);
            return 0;
        }
    }

    const char *sources_s = NULL, *sources_e = NULL;
    if (json_get_object_range(root_s, root_e, "sources", &sources_s, &sources_e) == 0) {
        static const char *const sources_allowed[] = { "ini", "hostapd", "wpa_cli", "rtl8812eu", "cpu", "venc" };
        if (!json_validate_object_keys(sources_s, sources_e, "sources", sources_allowed, (int)(sizeof(sources_allowed) / sizeof(sources_allowed[0])))) {
            free(json);
            return 0;
        }

        const char *ini_s = NULL, *ini_e = NULL;
        if (json_get_object_range(sources_s, sources_e, "ini", &ini_s, &ini_e) == 0) {
            static const char *const ini_allowed[] = { "enabled", "paths" };
            if (!json_validate_object_keys(ini_s, ini_e, "sources.ini", ini_allowed, 2)) {
                free(json);
                return 0;
            }
            int found = 0;
            int found_enabled = 0;
            int b = 0;
            if (json_get_bool_range(ini_s, ini_e, "enabled", &b, &found_enabled) == 0 && found_enabled) cfg->ini.enabled = b;
            else if (found_enabled) {
                fprintf(stderr, "Error: invalid sources.ini.enabled\n");
                free(json);
                return 0;
            }

            found = 0;
            if (json_get_string_array_range_cfg(ini_s, ini_e, "paths", cfg->ini.paths, MAX_INI_PATHS, &cfg->ini.paths_count, &found) != 0 && found) {
                fprintf(stderr, "Error: invalid sources.ini.paths\n");
                free(json);
                return 0;
            }

            if (!found_enabled && cfg->ini.paths_count > 0) {
                cfg->ini.enabled = 1;
            }
        }

        const char *host_s = NULL, *host_e = NULL;
        if (json_get_object_range(sources_s, sources_e, "hostapd", &host_s, &host_e) == 0) {
            static const char *const host_allowed[] = { "enabled", "iface", "sta" };
            if (!json_validate_object_keys(host_s, host_e, "sources.hostapd", host_allowed, 3)) {
                free(json);
                return 0;
            }
            int found = 0;
            int b = 0;
            if (json_get_bool_range(host_s, host_e, "enabled", &b, &found) == 0 && found) cfg->hostapd.enabled = b;
            else if (found) {
                fprintf(stderr, "Error: invalid sources.hostapd.enabled\n");
                free(json);
                return 0;
            }

            found = 0;
            if (json_get_string_range_cfg(host_s, host_e, "iface", cfg->hostapd.iface, sizeof(cfg->hostapd.iface), &found) != 0 && found) {
                fprintf(stderr, "Error: invalid sources.hostapd.iface\n");
                free(json);
                return 0;
            }
            found = 0;
            if (json_get_string_range_cfg(host_s, host_e, "sta", cfg->hostapd.sta, sizeof(cfg->hostapd.sta), &found) != 0 && found) {
                fprintf(stderr, "Error: invalid sources.hostapd.sta\n");
                free(json);
                return 0;
            }
        }

        const char *wpa_s = NULL, *wpa_e = NULL;
        if (json_get_object_range(sources_s, sources_e, "wpa_cli", &wpa_s, &wpa_e) == 0) {
            static const char *const wpa_allowed[] = { "enabled", "iface" };
            if (!json_validate_object_keys(wpa_s, wpa_e, "sources.wpa_cli", wpa_allowed, 2)) {
                free(json);
                return 0;
            }
            int found = 0;
            int b = 0;
            if (json_get_bool_range(wpa_s, wpa_e, "enabled", &b, &found) == 0 && found) cfg->wpa.enabled = b;
            else if (found) {
                fprintf(stderr, "Error: invalid sources.wpa_cli.enabled\n");
                free(json);
                return 0;
            }
            found = 0;
            if (json_get_string_range_cfg(wpa_s, wpa_e, "iface", cfg->wpa.iface, sizeof(cfg->wpa.iface), &found) != 0 && found) {
                fprintf(stderr, "Error: invalid sources.wpa_cli.iface\n");
                free(json);
                return 0;
            }
        }

        const char *rtl_s = NULL, *rtl_e = NULL;
        if (json_get_object_range(sources_s, sources_e, "rtl8812eu", &rtl_s, &rtl_e) == 0) {
            static const char *const rtl_allowed[] = { "enabled", "iface" };
            if (!json_validate_object_keys(rtl_s, rtl_e, "sources.rtl8812eu", rtl_allowed, 2)) {
                free(json);
                return 0;
            }
            int found = 0;
            int b = 0;
            if (json_get_bool_range(rtl_s, rtl_e, "enabled", &b, &found) == 0 && found) cfg->rtl8812eu.enabled = b;
            else if (found) {
                fprintf(stderr, "Error: invalid sources.rtl8812eu.enabled\n");
                free(json);
                return 0;
            }
            found = 0;
            if (json_get_string_range_cfg(rtl_s, rtl_e, "iface", cfg->rtl8812eu.iface, sizeof(cfg->rtl8812eu.iface), &found) != 0 && found) {
                fprintf(stderr, "Error: invalid sources.rtl8812eu.iface\n");
                free(json);
                return 0;
            }
        }

        const char *cpu_s = NULL, *cpu_e = NULL;
        if (json_get_object_range(sources_s, sources_e, "cpu", &cpu_s, &cpu_e) == 0) {
            static const char *const cpu_allowed[] = { "enabled" };
            if (!json_validate_object_keys(cpu_s, cpu_e, "sources.cpu", cpu_allowed, 1)) {
                free(json);
                return 0;
            }
            int found = 0;
            int b = 0;
            if (json_get_bool_range(cpu_s, cpu_e, "enabled", &b, &found) == 0 && found) cfg->cpu.enabled = b;
            else if (found) {
                fprintf(stderr, "Error: invalid sources.cpu.enabled\n");
                free(json);
                return 0;
            }
        }

        const char *venc_s = NULL, *venc_e = NULL;
        if (json_get_object_range(sources_s, sources_e, "venc", &venc_s, &venc_e) == 0) {
            static const char *const venc_allowed[] = { "enabled", "url" };
            if (!json_validate_object_keys(venc_s, venc_e, "sources.venc", venc_allowed, 2)) {
                free(json);
                return 0;
            }
            int found = 0;
            int b = 0;
            if (json_get_bool_range(venc_s, venc_e, "enabled", &b, &found) == 0 && found) cfg->venc.enabled = b;
            else if (found) {
                fprintf(stderr, "Error: invalid sources.venc.enabled\n");
                free(json);
                return 0;
            }

            found = 0;
            if (json_get_string_range_cfg(venc_s, venc_e, "url", cfg->venc.url, sizeof(cfg->venc.url), &found) != 0 && found) {
                fprintf(stderr, "Error: invalid sources.venc.url\n");
                free(json);
                return 0;
            }
        }
    }

    const char *payload_s = NULL, *payload_e = NULL;
    if (json_get_object_range(root_s, root_e, "payload", &payload_s, &payload_e) != 0) {
        fprintf(stderr, "Error: payload object is required\n");
        free(json);
        return 0;
    }
    static const char *const payload_allowed[] = { "values", "texts" };
    if (!json_validate_object_keys(payload_s, payload_e, "payload", payload_allowed, 2)) {
        free(json);
        return 0;
    }

    int found_values = 0, found_texts = 0;
    if (json_get_payload_rhs_array(payload_s, payload_e, "values", cfg->payload.value_used, cfg->payload.value_rhs, &found_values) != 0 && found_values) {
        fprintf(stderr, "Error: invalid payload.values\n");
        free(json);
        return 0;
    }
    if (json_get_payload_rhs_array(payload_s, payload_e, "texts", cfg->payload.text_used, cfg->payload.text_rhs, &found_texts) != 0 && found_texts) {
        fprintf(stderr, "Error: invalid payload.texts\n");
        free(json);
        return 0;
    }

    if (!found_values && !found_texts) {
        fprintf(stderr, "Error: payload.values and/or payload.texts is required\n");
        free(json);
        return 0;
    }

    if (cfg->watch.interval_ms < 5) cfg->watch.interval_ms = 5;
    if (cfg->watch.retry_ms < 100) cfg->watch.retry_ms = 100;

    if (cfg->hostapd.enabled && !cfg->hostapd.sta[0]) {
        fprintf(stderr, "Error: sources.hostapd.enabled requires sources.hostapd.sta\n");
        free(json);
        return 0;
    }
    if (cfg->wpa.enabled && !cfg->wpa.iface[0]) {
        fprintf(stderr, "Error: sources.wpa_cli.enabled requires sources.wpa_cli.iface\n");
        free(json);
        return 0;
    }
    if (cfg->rtl8812eu.enabled && !cfg->rtl8812eu.iface[0]) {
        fprintf(stderr, "Error: sources.rtl8812eu.enabled requires sources.rtl8812eu.iface\n");
        free(json);
        return 0;
    }
    if (cfg->ini.enabled && cfg->ini.paths_count <= 0) {
        fprintf(stderr, "Error: sources.ini.enabled requires sources.ini.paths\n");
        free(json);
        return 0;
    }
    if (cfg->venc.enabled && !cfg->venc.url[0]) {
        fprintf(stderr, "Error: sources.venc.enabled requires sources.venc.url\n");
        free(json);
        return 0;
    }

    free(json);
    return 1;
}

static int parse_config_arg(int argc, char **argv, const char *prog, char *cfg_path, size_t cfg_path_sz)
{
    copy_cstr(cfg_path, cfg_path_sz, DEFAULT_CFG_PATH);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage_main(prog);
            return 0;
        }
        if (!strcmp(a, "--config")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --config needs a file path\n");
                return -1;
            }
            copy_cstr(cfg_path, cfg_path_sz, argv[++i]);
            continue;
        }
        if (!strncmp(a, "--config=", 9)) {
            copy_cstr(cfg_path, cfg_path_sz, a + 9);
            continue;
        }

        fprintf(stderr, "Error: unknown argument '%s'\n", a);
        usage_main(prog);
        return -1;
    }

    return 1;
}

static uint64_t now_mono_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    }
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

/* ------------------------- INI ------------------------- */

static void ini_init(IniStore *ini) { memset(ini, 0, sizeof(*ini)); }

static int ini_set(IniStore *ini, const char *k, const char *v)
{
    if (!k || !*k) return 0;

    for (int i = 0; i < ini->count; i++) {
        if (!strcmp(ini->kv[i].key, k)) {
            copy_cstr(ini->kv[i].val, sizeof(ini->kv[i].val), v ? v : "");
            return 1;
        }
    }

    if (ini->count >= INI_MAX_KV) return 0;
    copy_cstr(ini->kv[ini->count].key, sizeof(ini->kv[ini->count].key), k);
    copy_cstr(ini->kv[ini->count].val, sizeof(ini->kv[ini->count].val), v ? v : "");
    ini->count++;
    return 1;
}

static const char *ini_get(const IniStore *ini, const char *k)
{
    if (!ini || !ini->loaded || !k || !*k) return NULL;
    for (int i = 0; i < ini->count; i++) {
        if (!strcmp(ini->kv[i].key, k)) return ini->kv[i].val;
    }
    return NULL;
}

static void strip_quotes_inplace(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    if (n >= 2) {
        if ((s[0] == '"' && s[n-1] == '"') || (s[0] == '\'' && s[n-1] == '\'')) {
            memmove(s, s + 1, n - 2);
            s[n - 2] = '\0';
        }
    }
}

static int ini_parse_stream(IniStore *ini, FILE *fp)
{
    if (!ini || !fp) return 0;

    ini->loaded = 1;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *t = trim(line);
        if (!*t) continue;
        if (*t == '#' || *t == ';') continue;
        if (*t == '[') continue;

        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(t);
        char *v = trim(eq + 1);
        strip_quotes_inplace(v);
        (void)ini_set(ini, k, v);
    }
    return 1;
}

static int ini_add_file(IniStore *ini, const char *path)
{
    if (!ini || !path || !*path) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    int ret = ini_parse_stream(ini, fp);
    fclose(fp);
    return ret;
}

/* For @key: if missing -> return 0. Non-@ -> copies. */
static int resolve_ini_ref(const IniStore *ini, const char *in, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    if (!in) return 0;

    if (in[0] == '@') {
        const char *k = in + 1;
        const char *v = ini_get(ini, k);
        if (!v) return 0;
        copy_cstr(out, out_sz, v);
        return 1;
    }

    copy_cstr(out, out_sz, in);
    return 1;
}

static void ini_merge(IniStore *dst, const IniStore *src)
{
    if (!dst || !src || !src->loaded) return;
    for (int i = 0; i < src->count; i++) {
        (void)ini_set(dst, src->kv[i].key, src->kv[i].val);
    }
    dst->loaded = 1;
}

static int ini_parse_kv_buffer(IniStore *ini, const char *buf)
{
    if (!ini || !buf) return 0;

    int added = 0;
    const char *p = buf;
    while (*p) {
        char line[512];
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '\n' && p[n] != '\r' && n + 1 < sizeof(line)) {
            line[n] = p[n];
            n++;
        }
        line[n] = '\0';

        while (p[n] == '\n' || p[n] == '\r') n++;
        p += n;

        char *t = trim(line);
        if (!*t) continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(t);
        char *v = trim(eq + 1);
        if (ini_set(ini, k, v)) added++;
    }
    return added;
}

static int build_local_ctrl(char *path, size_t path_sz)
{
    static int counter = 0;
    if (!path || path_sz == 0) return 0;
    int n = snprintf(path, path_sz, "/tmp/waybeam_ctrl_%ld_%d", (long)getpid(), counter++);
    if (n <= 0 || (size_t)n >= path_sz) return 0;
    return 1;
}

static int ctrl_request_unix(const char *dst_path, const char *cmd, char *out, size_t out_sz, int timeout_ms, int verbose)
{
    if (!dst_path || !cmd || !out || out_sz == 0) return 0;

    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) {
        if (verbose) perror("socket(AF_UNIX)");
        return 0;
    }

    char local_path[108];
    struct sockaddr_un local;
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    if (!build_local_ctrl(local_path, sizeof(local_path))) {
        close(s);
        return 0;
    }
    copy_cstr(local.sun_path, sizeof(local.sun_path), local_path);
    unlink(local.sun_path);
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        if (verbose) perror("bind(ctrl local)");
        close(s);
        unlink(local.sun_path);
        return 0;
    }

    struct sockaddr_un dst;
    memset(&dst, 0, sizeof(dst));
    dst.sun_family = AF_UNIX;
    copy_cstr(dst.sun_path, sizeof(dst.sun_path), dst_path);

    if (connect(s, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        if (verbose) perror("connect(ctrl)");
        close(s);
        unlink(local.sun_path);
        return 0;
    }

    if (timeout_ms < 0) timeout_ms = 1000;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        if (verbose) perror("setsockopt(SO_RCVTIMEO)");
    }

    size_t cmd_len = strlen(cmd);
    if (send(s, cmd, cmd_len, 0) != (ssize_t)cmd_len) {
        if (verbose) perror("send(ctrl)");
        close(s);
        unlink(local.sun_path);
        return 0;
    }

    ssize_t r = recv(s, out, out_sz - 1, 0);
    if (r < 0) {
        if (verbose) perror("recv(ctrl)");
        close(s);
        unlink(local.sun_path);
        return 0;
    }
    out[r] = '\0';

    close(s);
    unlink(local.sun_path);
    return 1;
}

static int ctrl_request_with_dirs(const char **dirs, const char *ifname, const char *cmd, char *out, size_t out_sz, int timeout_ms, int verbose)
{
    if (!dirs || !cmd || !out || out_sz == 0) return 0;

    char path[256];
    for (int i = 0; dirs[i]; i++) {
        if (ifname && *ifname) {
            int n = snprintf(path, sizeof(path), "%s/%s", dirs[i], ifname);
            if (n <= 0 || (size_t)n >= sizeof(path)) continue;
            if (ctrl_request_unix(path, cmd, out, out_sz, timeout_ms, verbose)) return 1;
            continue;
        }

        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            int n = snprintf(path, sizeof(path), "%s/%s", dirs[i], de->d_name);
            if (n <= 0 || (size_t)n >= sizeof(path)) continue;
            if (ctrl_request_unix(path, cmd, out, out_sz, timeout_ms, verbose)) {
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    if (verbose) fprintf(stderr, "[ctrl] no control socket found for %s\n", ifname ? ifname : "(auto)");
    return 0;
}

static int load_hostapd_metrics(IniStore *out, const char *ifname, const char *sta_mac, int verbose)
{
    if (!out) return 0;
    ini_init(out);
    if (!sta_mac || !*sta_mac) return 0;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "STA %s", sta_mac);

    static const char *dirs[] = { "/run/hostapd", "/var/run/hostapd", NULL };
    char buf[2048];
    if (!ctrl_request_with_dirs(dirs, ifname, cmd, buf, sizeof(buf), 1000, verbose)) {
        if (verbose) fprintf(stderr, "[hostapd] control request failed\n");
        return 0;
    }

    out->loaded = 1;
    (void)ini_parse_kv_buffer(out, buf);
    if (verbose) fprintf(stderr, "[hostapd] parsed %d fields\n", out->count);
    return 1;
}

static int load_wpa_metrics(IniStore *out, const char *iface, int verbose)
{
    if (!out) return 0;
    ini_init(out);
    if (!iface || !*iface) return 0;

    static const char *dirs[] = { "/run/wpa_supplicant", "/var/run/wpa_supplicant", NULL };
    char buf[2048];
    int any = 0;

    /* STATUS works in both AP and station mode */
    if (ctrl_request_with_dirs(dirs, iface, "STATUS", buf, sizeof(buf), 1000, verbose)) {
        any += ini_parse_kv_buffer(out, buf);
    }

    /* SIGNAL_POLL adds RSSI/linkspeed/noise in station mode; returns FAIL in AP mode */
    if (ctrl_request_with_dirs(dirs, iface, "SIGNAL_POLL", buf, sizeof(buf), 1000, verbose)) {
        any += ini_parse_kv_buffer(out, buf);
    }

    /* STA-FIRST returns first connected client in AP mode (signal, rx/tx, etc.) */
    if (ctrl_request_with_dirs(dirs, iface, "STA-FIRST", buf, sizeof(buf), 1000, verbose)) {
        if (strncmp(buf, "FAIL", 4) != 0 && strncmp(buf, "UNKNOWN", 7) != 0) {
            any += ini_parse_kv_buffer(out, buf);
        }
    }

    out->loaded = (any > 0);
    if (verbose) fprintf(stderr, "[wpa] parsed %d fields\n", out->count);
    return out->loaded;
}

static int load_8812eu_metrics(IniStore *out, const char *iface, int verbose)
{
    if (!out) return 0;
    ini_init(out);
    if (!iface || !*iface) return 0;

    char path_a[256];
    char path_b[256];
    snprintf(path_a, sizeof(path_a), "/proc/net/rtl88x2eu/%s/rssi_a", iface);
    snprintf(path_b, sizeof(path_b), "/proc/net/rtl88x2eu/%s/rssi_b", iface);

    FILE *fa = fopen(path_a, "r");
    FILE *fb = fopen(path_b, "r");
    if (!fa && !fb) {
        if (verbose) fprintf(stderr, "[8812eu] rssi files missing for %s\n", iface);
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }

    char buf[64];
    if (fa) {
        if (fgets(buf, sizeof(buf), fa)) {
            char *t = trim(buf);
            (void)ini_set(out, "rssi_a", t);
        }
        fclose(fa);
    }
    if (fb) {
        if (fgets(buf, sizeof(buf), fb)) {
            char *t = trim(buf);
            (void)ini_set(out, "rssi_b", t);
        }
        fclose(fb);
    }

    out->loaded = 1;
    if (verbose) fprintf(stderr, "[8812eu] parsed rssi fields for %s (a=%s, b=%s)\n",
        iface,
        ini_get(out, "rssi_a") ? ini_get(out, "rssi_a") : "null",
        ini_get(out, "rssi_b") ? ini_get(out, "rssi_b") : "null");
    return 1;
}

static void cpustats_init(CpuStatsState *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
}

static int parse_cpu_stat_totals(const char *line, CpuSample *out)
{
    if (!line || !out) return 0;

    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;

    int n = sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
        &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (n < 4) return 0;

    uint64_t total = (uint64_t)user + (uint64_t)nice + (uint64_t)system + (uint64_t)idle;
    uint64_t idle_total = (uint64_t)idle;

    if (n >= 5) {
        total += (uint64_t)iowait;
        idle_total += (uint64_t)iowait;
    }
    if (n >= 6) total += (uint64_t)irq;
    if (n >= 7) total += (uint64_t)softirq;
    if (n >= 8) total += (uint64_t)steal;

    out->total = total;
    out->idle = idle_total;
    return 1;
}

static int read_cpu_samples(CpuSample *total_out, CpuSample cores_out[MAX_CPU_CORES], int *core_count_out)
{
    if (!total_out || !cores_out || !core_count_out) return 0;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;

    memset(total_out, 0, sizeof(*total_out));
    memset(cores_out, 0, sizeof(CpuSample) * MAX_CPU_CORES);
    *core_count_out = 0;

    int have_total = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (!strncmp(line, "cpu ", 4)) {
            if (parse_cpu_stat_totals(line, total_out)) have_total = 1;
            continue;
        }

        if (strncmp(line, "cpu", 3)) continue;
        if (!isdigit((unsigned char)line[3])) continue;

        char *idx_start = line + 3;
        char *idx_end = NULL;
        long idx = strtol(idx_start, &idx_end, 10);
        if (idx_end == idx_start || idx < 0 || idx >= MAX_CPU_CORES) continue;

        if (parse_cpu_stat_totals(line, &cores_out[idx])) {
            if (idx + 1 > *core_count_out) *core_count_out = (int)(idx + 1);
        }
    }

    fclose(fp);
    return have_total;
}

static int cpu_usage_percent(const CpuSample *prev, const CpuSample *cur, double *out_pct)
{
    if (!prev || !cur || !out_pct) return 0;
    if (cur->total <= prev->total) return 0;

    uint64_t total_delta = cur->total - prev->total;
    uint64_t idle_delta = 0;
    if (cur->idle >= prev->idle) idle_delta = cur->idle - prev->idle;
    if (idle_delta > total_delta) idle_delta = total_delta;

    double busy = (double)(total_delta - idle_delta);
    double pct = (busy * 100.0) / (double)total_delta;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    *out_pct = pct;
    return 1;
}

static int load_cpu_metrics(IniStore *out, CpuStatsState *state, int verbose)
{
    if (!out || !state) return 0;
    ini_init(out);

    CpuSample total_now;
    CpuSample cores_now[MAX_CPU_CORES];
    int core_count_now = 0;
    if (!read_cpu_samples(&total_now, cores_now, &core_count_now)) {
        if (verbose) fprintf(stderr, "[cpu] failed to read /proc/stat\n");
        return 0;
    }

    if (!state->valid) {
        state->total_prev = total_now;
        memcpy(state->core_prev, cores_now, sizeof(cores_now));
        state->core_count = core_count_now;
        state->valid = 1;

        usleep(100000);
        if (!read_cpu_samples(&total_now, cores_now, &core_count_now)) {
            if (verbose) fprintf(stderr, "[cpu] failed to read 2nd /proc/stat sample\n");
            return 0;
        }
    }

    char vbuf[64];
    double pct = 0.0;
    if (!cpu_usage_percent(&state->total_prev, &total_now, &pct)) pct = 0.0;
    snprintf(vbuf, sizeof(vbuf), "%.1f", pct);
    (void)ini_set(out, "cpu_total", vbuf);

    snprintf(vbuf, sizeof(vbuf), "%d", core_count_now);
    (void)ini_set(out, "cpu_cores", vbuf);

    int core_limit = core_count_now;
    if (core_limit > MAX_CPU_CORES) core_limit = MAX_CPU_CORES;
    for (int i = 0; i < core_limit; i++) {
        double core_pct = 0.0;
        if (i < state->core_count) {
            if (cpu_usage_percent(&state->core_prev[i], &cores_now[i], &core_pct)) {
                pct = core_pct;
            } else {
                pct = 0.0;
            }
        } else {
            pct = 0.0;
        }
        snprintf(vbuf, sizeof(vbuf), "%.1f", pct);

        char key[32];
        snprintf(key, sizeof(key), "cpu%d", i);
        (void)ini_set(out, key, vbuf);
    }

    state->total_prev = total_now;
    memcpy(state->core_prev, cores_now, sizeof(cores_now));
    state->core_count = core_count_now;
    state->valid = 1;

    out->loaded = (out->count > 0);
    if (verbose) fprintf(stderr, "[cpu] parsed total + %d core keys\n", core_limit);
    return out->loaded;
}

/* Cherry-picked metrics from majestic /metrics (Prometheus format).
 * Purely passive HTTP fetch - never touches MI API or encoder state.
 *
 * Exposed keys:
 *   venc_bitrate  - video encoder bitrate (kbps, computed from byte counter delta)
 *   venc_bytes    - raw venc0 received bytes counter
 *   isp_fps       - sensor framerate
 *   isp_exposure  - ISP exposure value
 *   isp_again     - analog gain
 *   isp_dgain     - digital gain
 *   soc_temp      - SoC temperature (celsius)
 *   load_1m       - 1-minute load average
 *   mem_used_pct  - memory used percent (derived from MemTotal/MemAvailable)
 */

/* State for computing bitrate from byte counter deltas + fetch cache */
static struct {
    int valid;
    double prev_bytes;
    uint64_t prev_ms;
    uint64_t last_fetch_ms;
    IniStore cache;
    int cache_valid;
} venc_rate_state;

#define VENC_FETCH_INTERVAL_MS 1000

static int cache_venc_result(const IniStore *src, uint64_t now_fetch)
{
    if (!src) return 0;
    memcpy(&venc_rate_state.cache, src, sizeof(venc_rate_state.cache));
    venc_rate_state.cache_valid = 1;
    venc_rate_state.last_fetch_ms = now_fetch;
    return src->loaded;
}

static int cache_empty_venc_result(uint64_t now_fetch)
{
    IniStore empty;
    ini_init(&empty);
    empty.loaded = 0;
    return cache_venc_result(&empty, now_fetch);
}

static int load_venc_metrics(IniStore *out, const char *url, int verbose)
{
    if (!out) return 0;
    ini_init(out);
    if (!url || !*url) return 0;

    /* Rate-limit HTTP fetches to once per second; return cached results otherwise */
    uint64_t now_fetch = now_mono_ms();
    if (venc_rate_state.cache_valid &&
        (now_fetch - venc_rate_state.last_fetch_ms) < VENC_FETCH_INTERVAL_MS) {
        memcpy(out, &venc_rate_state.cache, sizeof(*out));
        return out->loaded;
    }

    /* Minimal HTTP GET via TCP socket */
    char host[64] = "127.0.0.1";
    int port = 80;
    const char *path = "/metrics";

    /* Parse http://host:port/path */
    const char *p = url;
    if (!strncmp(p, "http://", 7)) p += 7;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        const char *pstart = colon + 1;
        const char *pend = slash ? slash : pstart + strlen(pstart);
        size_t plen = (size_t)(pend - pstart);
        if (plen > 0 && plen < 8) {
            char pbuf[8];
            memcpy(pbuf, pstart, plen);
            pbuf[plen] = '\0';
            int pv = 0;
            if (parse_int(pbuf, &pv) && pv > 0 && pv <= 65535) port = pv;
        }
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    }
    if (slash) path = slash;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (verbose) perror("[venc] socket");
        return cache_empty_venc_result(now_fetch);
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        if (verbose) fprintf(stderr, "[venc] invalid host: %s\n", host);
        close(fd);
        return cache_empty_venc_result(now_fetch);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (verbose) perror("[venc] connect");
        close(fd);
        return cache_empty_venc_result(now_fetch);
    }

    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (send(fd, req, (size_t)rlen, 0) != rlen) {
        if (verbose) perror("[venc] send");
        close(fd);
        return cache_empty_venc_result(now_fetch);
    }

    static char buf[12288];
    int total = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += (int)n;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    close(fd);
    buf[total] = '\0';

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        if (verbose) fprintf(stderr, "[venc] no HTTP body in response\n");
        return cache_empty_venc_result(now_fetch);
    }
    body += 4;

    /* Whitelist: prometheus_name -> output key.  Only these are extracted. */
#define PROM(name, key) { name, (int)(sizeof(name) - 1), key }
    static const struct { const char *prom; int plen; const char *key; } wanted[] = {
        PROM("venc0_rcvd_bytes",              "venc_bytes"),
        PROM("isp_fps",                       "isp_fps"),
        PROM("isp_exposure",                  "isp_exposure"),
        PROM("isp_again",                     "isp_again"),
        PROM("isp_dgain",                     "isp_dgain"),
        PROM("node_hwmon_temp_celsius",       "soc_temp"),
        PROM("node_load1",                    "load_1m"),
        PROM("node_memory_MemTotal_bytes",    "_mem_total"),
        PROM("node_memory_MemAvailable_bytes","_mem_avail"),
    };
#undef PROM
    static const int nwanted = (int)(sizeof(wanted) / sizeof(wanted[0]));

    double mem_total = 0, mem_avail = 0;
    double venc_bytes = 0;
    int got_venc_bytes = 0;
    int any = 0;
    char *line = body;

    while (*line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        if (line[0] == '#' || line[0] == '\0' || line[0] == '\r') {
            if (eol) { line = eol + 1; continue; }
            break;
        }

        /* Extract metric name (before space or '{') */
        char *sp = line;
        while (*sp && *sp != ' ' && *sp != '{') sp++;
        size_t klen = (size_t)(sp - line);

        /* Skip labels if present */
        char *val_start = sp;
        if (*sp == '{') {
            val_start = strchr(sp, '}');
            if (!val_start) { if (eol) { line = eol + 1; continue; } break; }
            val_start++;
        }
        while (*val_start == ' ') val_start++;

        /* Trim trailing whitespace from value */
        char *vend = val_start + strlen(val_start);
        while (vend > val_start && (vend[-1] == '\r' || vend[-1] == '\n' || vend[-1] == ' ')) vend--;
        *vend = '\0';

        /* Check against whitelist */
        for (int i = 0; i < nwanted && klen > 0 && *val_start; i++) {
            if ((int)klen == wanted[i].plen && !memcmp(line, wanted[i].prom, klen)) {
                /* Store raw value for internal keys prefixed with _ */
                if (wanted[i].key[0] == '_') {
                    double v = strtod(val_start, NULL);
                    if (!strcmp(wanted[i].key, "_mem_total")) mem_total = v;
                    else if (!strcmp(wanted[i].key, "_mem_avail")) mem_avail = v;
                } else {
                    (void)ini_set(out, wanted[i].key, val_start);
                    any = 1;
                    if (!strcmp(wanted[i].key, "venc_bytes")) {
                        venc_bytes = strtod(val_start, NULL);
                        got_venc_bytes = 1;
                    }
                }
                break;
            }
        }

        if (eol) line = eol + 1;
        else break;
    }

    /* Derived: venc_bitrate (kbps) from byte counter delta */
    if (got_venc_bytes) {
        if (venc_rate_state.valid && now_fetch > venc_rate_state.prev_ms) {
            double dt_s = (double)(now_fetch - venc_rate_state.prev_ms) / 1000.0;
            double delta = venc_bytes - venc_rate_state.prev_bytes;
            if (delta >= 0 && dt_s > 0) {
                double kbps = (delta * 8.0) / (dt_s * 1000.0);
                char vbuf[32];
                snprintf(vbuf, sizeof(vbuf), "%.1f", kbps);
                (void)ini_set(out, "venc_bitrate", vbuf);
                any = 1;
            }
        }
        venc_rate_state.prev_bytes = venc_bytes;
        venc_rate_state.prev_ms = now_fetch;
        venc_rate_state.valid = 1;
    }

    /* Derived: mem_used_pct from MemTotal and MemAvailable */
    if (mem_total > 0) {
        double pct = ((mem_total - mem_avail) / mem_total) * 100.0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "%.1f", pct);
        (void)ini_set(out, "mem_used_pct", vbuf);
        any = 1;
    }

    out->loaded = any;
    /* Cache result and timestamp, including empty parses. */
    cache_venc_result(out, now_fetch);

    if (verbose) fprintf(stderr, "[venc] fetched %d metrics from %s:%d%s\n", out->count, host, port, path);
    return any;
}

static void refresh_cli_store(IniStore *cli, const char *hostapd_iface, const char *hostapd_sta,
    const char *wpa_iface, const char *rtl8812_iface, int cpu_enable, CpuStatsState *cpu_state,
    const char *venc_url, int verbose)
{
    if (!cli) return;
    ini_init(cli);

    IniStore tmp;
    int any = 0;

    if (hostapd_sta && *hostapd_sta) {
        if (load_hostapd_metrics(&tmp, hostapd_iface, hostapd_sta, verbose)) {
            ini_merge(cli, &tmp);
            any = 1;
        }
    }

    if (wpa_iface && *wpa_iface) {
        if (load_wpa_metrics(&tmp, wpa_iface, verbose)) {
            ini_merge(cli, &tmp);
            any = 1;
        }
    }

    if (rtl8812_iface && *rtl8812_iface) {
        if (load_8812eu_metrics(&tmp, rtl8812_iface, verbose)) {
            ini_merge(cli, &tmp);
            any = 1;
        }
    }

    if (cpu_enable) {
        if (load_cpu_metrics(&tmp, cpu_state, verbose)) {
            ini_merge(cli, &tmp);
            any = 1;
        }
    }

    if (venc_url && *venc_url) {
        if (load_venc_metrics(&tmp, venc_url, verbose)) {
            ini_merge(cli, &tmp);
            any = 1;
        }
    }

    if (!any) ini_init(cli);
}


/* ------------------------- payload ------------------------- */

static void payload_init(Payload *p) { memset(p, 0, sizeof(*p)); }

static int set_value_num(Payload *p, int idx, double v)
{
    if (idx < 0 || idx > 7) return 0;
    p->values_state[idx] = VS_NUM;
    p->values[idx] = v;
    return 1;
}

static int set_value_null(Payload *p, int idx)
{
    if (idx < 0 || idx > 7) return 0;
    p->values_state[idx] = VS_NULL;
    return 1;
}

static int set_value_empty(Payload *p, int idx)
{
    if (idx < 0 || idx > 7) return 0;
    p->values_state[idx] = VS_EMPTY; /* emit "" */
    return 1;
}

static int set_text_str(Payload *p, int idx, const char *s)
{
    if (idx < 0 || idx > 7) return 0;
    p->texts_state[idx] = TS_STR;
    char clamped[MAX_TEXT_LEN + 1];
    clamp_textN(s ? s : "", clamped, sizeof(clamped));
    copy_cstr(p->texts[idx], sizeof(p->texts[idx]), clamped);
    return 1;
}

static int set_text_null(Payload *p, int idx)
{
    if (idx < 0 || idx > 7) return 0;
    p->texts_state[idx] = TS_NULL;
    p->texts[idx][0] = '\0';
    return 1;
}

static int any_values(const Payload *p)
{
    for (int i = 0; i < 8; i++) if (p->values_state[i] != VS_ABSENT) return 1;
    return 0;
}

static int any_texts(const Payload *p)
{
    for (int i = 0; i < 8; i++) if (p->texts_state[i] != TS_ABSENT) return 1;
    return 0;
}

static int serialize_payload(const Payload *p, char *out, size_t out_cap)
{
    int len = 0;
    if (!appendf(out, out_cap, &len, "{")) return -1;

    int first = 1;

    if (any_values(p)) {
        if (!appendf(out, out_cap, &len, "%s\"values\":[", first ? "" : ",")) return -1;
        first = 0;

        int max_idx = -1;
        for (int i = 0; i < 8; i++) if (p->values_state[i] != VS_ABSENT) max_idx = i;

        for (int i = 0; i <= max_idx; i++) {
            const char *sep = (i == 0) ? "" : ",";
            if (p->values_state[i] == VS_NUM) {
                if (!appendf(out, out_cap, &len, "%s%.1f", sep, p->values[i])) return -1;
            } else if (p->values_state[i] == VS_EMPTY) {
                if (!appendf(out, out_cap, &len, "%s\"\"", sep)) return -1;
            } else {
                /* VS_NULL or VS_ABSENT within range => null placeholder */
                if (!appendf(out, out_cap, &len, "%snull", sep)) return -1;
            }
        }
        if (!appendf(out, out_cap, &len, "]")) return -1;
    }

    if (any_texts(p)) {
        if (!appendf(out, out_cap, &len, "%s\"texts\":[", first ? "" : ",")) return -1;
        first = 0;

        int max_idx = -1;
        for (int i = 0; i < 8; i++) if (p->texts_state[i] != TS_ABSENT) max_idx = i;

        for (int i = 0; i <= max_idx; i++) {
            const char *sep = (i == 0) ? "" : ",";
            if (p->texts_state[i] == TS_STR) {
                char esc[800];
                json_escape(p->texts[i], esc, sizeof(esc));
                if (!appendf(out, out_cap, &len, "%s\"%s\"", sep, esc)) return -1;
            } else {
                /* TS_NULL or TS_ABSENT within range => null placeholder */
                if (!appendf(out, out_cap, &len, "%snull", sep)) return -1;
            }
        }
        if (!appendf(out, out_cap, &len, "]")) return -1;
    }

    if (!appendf(out, out_cap, &len, "}")) return -1;

    if (len > MAX_PAYLOAD) return -2;
    return len;
}

/* ------------------------- UDP ------------------------- */

static int open_udp_socket(void) { return socket(AF_INET, SOCK_DGRAM, 0); }

static int send_udp(int sock, const char *dest_ip, int port, const char *buf, size_t len)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, dest_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address: %s\n", dest_ip);
        errno = EINVAL;
        return -1;
    }

    ssize_t sent = sendto(sock, buf, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    return (sent < 0) ? -1 : 0;
}

/* ------------------------- usage ------------------------- */

static void usage_main(const char *prog)
{
    fprintf(stderr,
        "waybeam - bare UDP OSD sender + ini watcher\n"
        "\n"
        "Usage:\n"
        "  %s send  [--config <file>]\n"
        "  %s watch [--config <file>]\n"
        "\n"
        "Options:\n"
        "  --config <file>           JSON config file (default: %s)\n"
        "  -h, --help                show this help\n"
        "\n"
        "Config-driven fields include network, sources, payload maps, runtime,\n"
        "and watch timing/retry settings. See README.md and CONTRACT.md.\n"
        "\n"
        "Examples:\n"
        "  %s send --config osd_send.json\n"
        "  %s watch --config osd_send.json\n",
        prog, prog,
        DEFAULT_CFG_PATH,
        prog, prog);
}

/* ------------------------- watch spec ------------------------- */

typedef struct {
    char *value_rhs[8];
    char *text_rhs[8];
    int value_used[8];
    int text_used[8];

    ValueState last_v_state[8];
    double last_v[8];

    TextState last_t_state[8];
    char last_t[8][MAX_TEXT_LEN + 1];
} WatchSpec;

static void watchspec_init(WatchSpec *w) { memset(w, 0, sizeof(*w)); }

static void watchspec_free(WatchSpec *w)
{
    for (int i = 0; i < 8; i++) {
        free(w->value_rhs[i]);
        free(w->text_rhs[i]);
    }
}

static int watchspec_any(const WatchSpec *w)
{
    for (int i = 0; i < 8; i++) if (w->value_used[i] || w->text_used[i]) return 1;
    return 0;
}

typedef struct {
    int active;
    Payload payload;
    int pending_v[8];
    ValueState next_v_state[8];
    double next_v[8];
    int pending_t[8];
    TextState next_t_state[8];
    char next_t[8][MAX_TEXT_LEN + 1];
} PendingDelta;

static void pending_delta_init(PendingDelta *d)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
}

static void pending_delta_commit(const PendingDelta *d, WatchSpec *w)
{
    if (!d || !w) return;
    for (int i = 0; i < 8; i++) {
        if (d->pending_v[i]) {
            w->last_v_state[i] = d->next_v_state[i];
            if (d->next_v_state[i] == VS_NUM) w->last_v[i] = d->next_v[i];
        }
        if (d->pending_t[i]) {
            w->last_t_state[i] = d->next_t_state[i];
            if (d->next_t_state[i] == TS_STR) {
                copy_cstr(w->last_t[i], sizeof(w->last_t[i]), d->next_t[i]);
            } else {
                w->last_t[i][0] = '\0';
            }
        }
    }
}

typedef struct {
    IniStore store;
    FILE *fp;
    const char *path;
    time_t mtime;
    off_t size;
    ino_t inode;
} IniContext;

static const char *lookup_from_sources(const IniStore *cli, const IniContext *ctx, int ctx_count, const char *key)
{
    if (!key || !*key) return NULL;

    const char *v = NULL;
    if (cli && cli->loaded) v = ini_get(cli, key);
    if (v) return v;

    for (int i = ctx_count - 1; i >= 0; i--) {
        v = ini_get(&ctx[i].store, key);
        if (v) return v;
    }
    return NULL;
}

/* Watch resolve:
 *  - missing @key => null (ignored)
 *  - empty => "" (clear)
 *  - "null" => null
 *  - numeric parse -> number
 */

static int parse_port_value(const char *s, int *out)
{
    int port = 0;
    if (!parse_int(s, &port)) return 0;
    if (port <= 0 || port > 65535) return 0;
    *out = port;
    return 1;
}

static ValueState resolve_watch_value_rhs(const char *rhs,
    const IniStore *cli_store, const IniContext *ctx, int ini_count,
    int idx, int verbose, double *out_num)
{
    double dv = 0.0;
    ValueState st = VS_NULL;

    if (!rhs) return VS_NULL;
    if (is_literal_null(rhs)) return VS_NULL;
    if (rhs[0] == '\0') return VS_EMPTY;

    if (rhs[0] == '@') {
        const char *k = rhs + 1;
        const char *found = lookup_from_sources(cli_store, ctx, ini_count, k);
        if (!found) {
            if (verbose) fprintf(stderr, "[watch] values[%d] missing %s => null\n", idx, rhs);
            return VS_NULL;
        }
        if (found[0] == '\0') return VS_EMPTY;
        if (is_literal_null(found)) return VS_NULL;
        if (!parse_double(found, &dv)) {
            if (verbose) fprintf(stderr, "[watch] values[%d] non-numeric '%s' from %s => null\n", idx, found, rhs);
            return VS_NULL;
        }
        st = VS_NUM;
    } else if (parse_double(rhs, &dv)) {
        st = VS_NUM;
    }

    if (st == VS_NUM && out_num) *out_num = dv;
    return st;
}

static TextState resolve_watch_text_rhs(const char *rhs,
    const IniStore *cli_store, const IniContext *ctx, int ini_count,
    int idx, int verbose, char *out_text, size_t out_text_sz)
{
    if (out_text && out_text_sz > 0) out_text[0] = '\0';

    if (!rhs) return TS_NULL;
    if (is_literal_null(rhs)) return TS_NULL;
    if (rhs[0] == '\0') return TS_STR;

    if (rhs[0] == '@') {
        const char *k = rhs + 1;
        const char *found = lookup_from_sources(cli_store, ctx, ini_count, k);
        if (!found) {
            if (verbose) fprintf(stderr, "[watch] texts[%d] missing %s => null\n", idx, rhs);
            return TS_NULL;
        }
        if (is_literal_null(found)) return TS_NULL;
        if (out_text && out_text_sz > 0) clamp_textN(found, out_text, out_text_sz);
        return TS_STR;
    }

    if (out_text && out_text_sz > 0) clamp_textN(rhs, out_text, out_text_sz);
    return TS_STR;
}

static int resolve_watch_endpoint(const char *dest_raw, const char *port_raw,
    const IniStore *cli_store, const IniContext *ctx, int ini_count,
    char *dest_out, size_t dest_out_sz, int *port_out, int verbose, int log_defaults)
{
    if (!dest_raw || !dest_out || dest_out_sz == 0 || !port_out) return 0;

    if (dest_raw[0] == '@') {
        const char *k = dest_raw + 1;
        const char *found = lookup_from_sources(cli_store, ctx, ini_count, k);
        if (!found || !*found) {
            copy_cstr(dest_out, dest_out_sz, DEFAULT_DEST_IP);
            if (verbose && log_defaults) fprintf(stderr, "[watch] --dest %s missing => default %s\n", dest_raw, dest_out);
        } else {
            copy_cstr(dest_out, dest_out_sz, found);
        }
    } else {
        copy_cstr(dest_out, dest_out_sz, dest_raw);
    }

    *port_out = DEFAULT_PORT;
    if (port_raw) {
        const char *port_value = port_raw;
        if (port_raw[0] == '@') {
            const char *key = port_raw + 1;
            port_value = lookup_from_sources(cli_store, ctx, ini_count, key);
            if (!port_value) {
                *port_out = DEFAULT_PORT;
                if (verbose && log_defaults) fprintf(stderr, "[watch] --port %s missing => default %d\n", port_raw, *port_out);
                return 1;
            }
        }

        if (!parse_port_value(port_value, port_out)) {
            fprintf(stderr, "Error: invalid port value '%s'\n", port_value);
            return 0;
        }
    }

    return 1;
}

static int cfg_payload_any(const OsdSendConfig *cfg)
{
    for (int i = 0; i < 8; i++) {
        if (cfg->payload.value_used[i] || cfg->payload.text_used[i]) return 1;
    }
    return 0;
}

static int cfg_apply_values_send(Payload *p, const IniStore *ini, const OsdSendConfig *cfg, int verbose)
{
    for (int idx = 0; idx < 8; idx++) {
        if (!cfg->payload.value_used[idx]) continue;
        const char *rhs = cfg->payload.value_rhs[idx];

        if (is_literal_null(rhs)) {
            if (verbose) fprintf(stderr, "[send] values[%d]=null (literal)\n", idx);
            set_value_null(p, idx);
            continue;
        }

        if (rhs[0] == '\0') {
            if (verbose) fprintf(stderr, "[send] values[%d]=\"\" (clear)\n", idx);
            set_value_empty(p, idx);
            continue;
        }

        char resolved[INI_VAL_MAX];
        if (!resolve_ini_ref(ini, rhs, resolved, sizeof(resolved))) {
            if (verbose) fprintf(stderr, "[send] values[%d]=null (missing %s)\n", idx, rhs);
            set_value_null(p, idx);
            continue;
        }

        if (resolved[0] == '\0') {
            if (verbose) fprintf(stderr, "[send] values[%d]=\"\" (ini empty %s)\n", idx, rhs);
            set_value_empty(p, idx);
            continue;
        }

        if (is_literal_null(resolved)) {
            if (verbose) fprintf(stderr, "[send] values[%d]=null (ini 'null' %s)\n", idx, rhs);
            set_value_null(p, idx);
            continue;
        }

        double dv = 0.0;
        if (!parse_double(resolved, &dv)) {
            if (verbose) fprintf(stderr, "[send] values[%d]=null (non-numeric '%s' from %s)\n", idx, resolved, rhs);
            set_value_null(p, idx);
            continue;
        }

        if (verbose) fprintf(stderr, "[send] values[%d]=%.1f (from %s)\n", idx, dv, rhs);
        set_value_num(p, idx, dv);
    }
    return 1;
}

static int cfg_apply_texts_send(Payload *p, const IniStore *ini, const OsdSendConfig *cfg, int verbose)
{
    for (int idx = 0; idx < 8; idx++) {
        if (!cfg->payload.text_used[idx]) continue;
        const char *rhs = cfg->payload.text_rhs[idx];

        if (is_literal_null(rhs)) {
            if (verbose) fprintf(stderr, "[send] texts[%d]=null (literal)\n", idx);
            set_text_null(p, idx);
            continue;
        }

        if (rhs[0] == '\0') {
            if (verbose) fprintf(stderr, "[send] texts[%d]=\"\" (clear)\n", idx);
            set_text_str(p, idx, "");
            continue;
        }

        char resolved[INI_VAL_MAX];
        if (!resolve_ini_ref(ini, rhs, resolved, sizeof(resolved))) {
            if (verbose) fprintf(stderr, "[send] texts[%d]=null (missing %s)\n", idx, rhs);
            set_text_null(p, idx);
            continue;
        }

        if (is_literal_null(resolved)) {
            if (verbose) fprintf(stderr, "[send] texts[%d]=null (ini 'null' %s)\n", idx, rhs);
            set_text_null(p, idx);
            continue;
        }

        if (resolved[0] == '\0') {
            if (verbose) fprintf(stderr, "[send] texts[%d]=\"\" (ini empty %s)\n", idx, rhs);
            set_text_str(p, idx, "");
            continue;
        }

        if (verbose) fprintf(stderr, "[send] texts[%d]=\"%s\" (from %s)\n", idx, resolved, rhs);
        set_text_str(p, idx, resolved);
    }
    return 1;
}

/* ------------------------- SEND ------------------------- */

static int cmd_send(int argc, char **argv, const char *prog)
{
    char cfg_path[INI_PATH_MAX];
    int arg_ret = parse_config_arg(argc, argv, prog, cfg_path, sizeof(cfg_path));
    if (arg_ret == 0) return 0;
    if (arg_ret < 0) return 1;

    OsdSendConfig cfg;
    if (!cfg_parse_file(cfg_path, &cfg)) return 1;
    if (!cfg_payload_any(&cfg)) {
        fprintf(stderr, "Error: payload has no active values/texts entries\n");
        return 1;
    }

    int verbose = cfg.runtime.verbose;
    int print_json = cfg.runtime.print_json;

    IniStore ini;
    ini_init(&ini);
    if (cfg.ini.enabled) {
        for (int i = 0; i < cfg.ini.paths_count; i++) {
            if (!ini_add_file(&ini, cfg.ini.paths[i])) {
                if (verbose) fprintf(stderr, "[send] ini not readable: %s (%s)\n", cfg.ini.paths[i], strerror(errno));
            }
        }
    }

    IniStore cli_store;
    ini_init(&cli_store);
    CpuStatsState cpu_state;
    cpustats_init(&cpu_state);
    refresh_cli_store(&cli_store,
        cfg.hostapd.enabled ? cfg.hostapd.iface : NULL,
        cfg.hostapd.enabled ? cfg.hostapd.sta : NULL,
        cfg.wpa.enabled ? cfg.wpa.iface : NULL,
        cfg.rtl8812eu.enabled ? cfg.rtl8812eu.iface : NULL,
        cfg.cpu.enabled,
        &cpu_state,
        cfg.venc.enabled ? cfg.venc.url : NULL,
        verbose);
    ini_merge(&ini, &cli_store);

    const char *dest_raw = cfg.network.dest;
    const char *port_raw = cfg.network.port;

    char dest[64];
    if (dest_raw[0] == '@') {
        const char *v = ini_get(&ini, dest_raw + 1);
        if (!v || !*v) {
            copy_cstr(dest, sizeof(dest), DEFAULT_DEST_IP);
            if (verbose) fprintf(stderr, "[send] --dest %s missing => default %s\n", dest_raw, dest);
        } else {
            copy_cstr(dest, sizeof(dest), v);
        }
    } else {
        copy_cstr(dest, sizeof(dest), dest_raw);
    }

    int port = DEFAULT_PORT;
    if (port_raw && *port_raw) {
        char pbuf[64];
        if (!resolve_ini_ref(&ini, port_raw, pbuf, sizeof(pbuf))) {
            port = DEFAULT_PORT;
            if (verbose) fprintf(stderr, "[send] --port %s missing => default %d\n", port_raw, port);
        } else if (!parse_port_value(pbuf, &port)) {
            fprintf(stderr, "Error: invalid port value '%s'\n", pbuf);
            return 1;
        }
    }

    Payload payload;
    payload_init(&payload);
    if (!cfg_apply_values_send(&payload, &ini, &cfg, verbose)) return 1;
    if (!cfg_apply_texts_send(&payload, &ini, &cfg, verbose)) return 1;

    char out[BUILD_BUF];
    int out_len = serialize_payload(&payload, out, sizeof(out));
    if (out_len == -2) {
        fprintf(stderr, "Error: payload exceeds %d bytes\n", MAX_PAYLOAD);
        return 1;
    }
    if (out_len < 0) {
        fprintf(stderr, "Error: failed to serialize payload\n");
        return 1;
    }

    if (verbose) fprintf(stderr, "[send] dst=%s:%d len=%d json=%s\n", dest, port, out_len, out);

    if (print_json) {
        printf("%s\n", out);
        return 0;
    }

    int sock = open_udp_socket();
    if (sock < 0) { perror("socket"); return 1; }

    if (send_udp(sock, dest, port, out, (size_t)out_len) < 0) {
        perror("sendto");
        close(sock);
        return 1;
    }

    close(sock);
    return 0;
}

/* ------------------------- WATCH ------------------------- */

static int cmd_watch(int argc, char **argv, const char *prog)
{
    char cfg_path[INI_PATH_MAX];
    int arg_ret = parse_config_arg(argc, argv, prog, cfg_path, sizeof(cfg_path));
    if (arg_ret == 0) return 0;
    if (arg_ret < 0) return 1;

    OsdSendConfig cfg;
    if (!cfg_parse_file(cfg_path, &cfg)) return 1;

    int verbose = cfg.runtime.verbose;
    int interval_ms = cfg.watch.interval_ms;
    int retry_ms = cfg.watch.retry_ms;

    WatchSpec w;
    watchspec_init(&w);
    for (int i = 0; i < 8; i++) {
        if (cfg.payload.value_used[i]) {
            w.value_used[i] = 1;
            w.value_rhs[i] = xstrdup(cfg.payload.value_rhs[i]);
            if (!w.value_rhs[i]) {
                perror("malloc");
                watchspec_free(&w);
                return 1;
            }
        }
        if (cfg.payload.text_used[i]) {
            w.text_used[i] = 1;
            w.text_rhs[i] = xstrdup(cfg.payload.text_rhs[i]);
            if (!w.text_rhs[i]) {
                perror("malloc");
                watchspec_free(&w);
                return 1;
            }
        }
    }
    if (!watchspec_any(&w)) {
        fprintf(stderr, "Error: payload has no active values/texts entries\n");
        watchspec_free(&w);
        return 1;
    }

    const char *dest_raw = cfg.network.dest;
    const char *port_raw = cfg.network.port;
    const char *ini_paths[MAX_INI_PATHS];
    int ini_count = 0;
    if (cfg.ini.enabled) {
        for (int i = 0; i < cfg.ini.paths_count && i < MAX_INI_PATHS; i++) {
            ini_paths[ini_count++] = cfg.ini.paths[i];
        }
    }

    const char *hostapd_iface = cfg.hostapd.enabled ? cfg.hostapd.iface : NULL;
    const char *hostapd_sta = cfg.hostapd.enabled ? cfg.hostapd.sta : NULL;
    const char *wpa_iface = cfg.wpa.enabled ? cfg.wpa.iface : NULL;
    const char *rtl8812_iface = cfg.rtl8812eu.enabled ? cfg.rtl8812eu.iface : NULL;
    int cpu_enable = cfg.cpu.enabled;
    const char *venc_url = cfg.venc.enabled ? cfg.venc.url : NULL;

    int has_cli_source = (hostapd_sta && *hostapd_sta) || (wpa_iface && *wpa_iface) ||
        (rtl8812_iface && *rtl8812_iface) || cpu_enable ||
        (venc_url && *venc_url);
    if (ini_count == 0 && !has_cli_source) {
        fprintf(stderr, "Error: config needs at least one enabled source (ini/hostapd/wpa_cli/rtl8812eu/cpu/venc) for watch mode\n");
        watchspec_free(&w);
        return 1;
    }

    IniStore cli_store;
    ini_init(&cli_store);
    CpuStatsState cpu_state;
    cpustats_init(&cpu_state);

    IniContext *ctx = NULL;
    if (ini_count > 0) {
        ctx = (IniContext *)calloc((size_t)ini_count, sizeof(IniContext));
        if (!ctx) {
            perror("calloc");
            watchspec_free(&w);
            return 1;
        }
    }

    /* Initial load of all files */
    int have_ini0 = 0;
    for (int i = 0; i < ini_count; i++) {
        ctx[i].path = ini_paths[i];
        ini_init(&ctx[i].store);

        /* Open and parse */
        ctx[i].fp = fopen(ctx[i].path, "r");
        if (ctx[i].fp) {
            ini_parse_stream(&ctx[i].store, ctx[i].fp);
            have_ini0 = 1;

            struct stat st;
            if (fstat(fileno(ctx[i].fp), &st) == 0) {
                ctx[i].mtime = st.st_mtime;
                ctx[i].size = st.st_size;
                ctx[i].inode = st.st_ino;
            }
        } else {
            if (verbose) fprintf(stderr, "[watch] ini not readable initially: %s (%s)\n", ctx[i].path, strerror(errno));
        }
    }

    refresh_cli_store(&cli_store, hostapd_iface, hostapd_sta, wpa_iface, rtl8812_iface, cpu_enable, &cpu_state,
        venc_url, verbose);

    char dest[64];
    int port = DEFAULT_PORT;
    if (!resolve_watch_endpoint(dest_raw, port_raw, &cli_store, ctx, ini_count,
            dest, sizeof(dest), &port, verbose, 1)) {
        watchspec_free(&w);
        for (int i=0; i<ini_count; i++) if (ctx[i].fp) fclose(ctx[i].fp);
        free(ctx);
        return 1;
    }

    int sock = open_udp_socket();
    if (sock < 0) {
        perror("socket");
        watchspec_free(&w);
        /* Cleanup contexts including open files */
        for (int i=0; i<ini_count; i++) if (ctx[i].fp) fclose(ctx[i].fp);
        free(ctx);
        return 1;
    }

    if (verbose) {
        fprintf(stderr, "[watch] start dst=%s:%d ini=%d files interval=%dms\n", dest, port, ini_count, interval_ms);
    }

    uint64_t next_retry_at_ms = 0;
    PendingDelta retry_delta;
    pending_delta_init(&retry_delta);

    /* baseline send */
    {
        PendingDelta baseline;
        pending_delta_init(&baseline);
        payload_init(&baseline.payload);

        if (verbose && ini_count > 0 && !have_ini0) fprintf(stderr, "[watch] baseline: ini unreadable -> all watched @keys treated as null\n");
        if (verbose && ini_count == 0 && has_cli_source) fprintf(stderr, "[watch] baseline: using enabled non-ini sources only\n");

        for (int i = 0; i < 8; i++) {
            if (w.value_used[i] && w.value_rhs[i]) {
                double dv = 0.0;
                ValueState st = resolve_watch_value_rhs(w.value_rhs[i], &cli_store, ctx, ini_count, i, verbose, &dv);

                baseline.next_v_state[i] = st;
                baseline.next_v[i] = dv;
                baseline.pending_v[i] = 1;
                if (st == VS_NUM) { set_value_num(&baseline.payload, i, dv); }
                else if (st == VS_EMPTY) { set_value_empty(&baseline.payload, i); }
                else { set_value_null(&baseline.payload, i); }
            }
            if (w.text_used[i] && w.text_rhs[i]) {
                char t[MAX_TEXT_LEN + 1];
                TextState st = resolve_watch_text_rhs(w.text_rhs[i], &cli_store, ctx, ini_count, i, verbose, t, sizeof(t));

                baseline.next_t_state[i] = st;
                baseline.pending_t[i] = 1;
                if (st == TS_STR) {
                    copy_cstr(baseline.next_t[i], sizeof(baseline.next_t[i]), t);
                    set_text_str(&baseline.payload, i, t);
                } else {
                    baseline.next_t[i][0] = '\0';
                    set_text_null(&baseline.payload, i);
                }
            }
        }

        char out[BUILD_BUF];
        int out_len = serialize_payload(&baseline.payload, out, sizeof(out));
        if (out_len < 0) {
            fprintf(stderr, "[watch] baseline payload build failed; retrying in %dms\n", retry_ms);
            next_retry_at_ms = now_mono_ms() + (uint64_t)retry_ms;
            retry_delta = baseline;
            retry_delta.active = 1;
        }

        if (out_len >= 0) {
            if (verbose) fprintf(stderr, "[watch] baseline send len=%d json=%s\n", out_len, out);

            if (send_udp(sock, dest, port, out, (size_t)out_len) < 0) {
                perror("sendto(baseline)");
                if (verbose) fprintf(stderr, "[watch] baseline send failed; retrying in %dms\n", retry_ms);
                next_retry_at_ms = now_mono_ms() + (uint64_t)retry_ms;
                retry_delta = baseline;
                retry_delta.active = 1;
            } else {
                pending_delta_commit(&baseline, &w);
            }
        }
    }

    /* poll loop */
    for (;;) {
        for (int i = 0; i < ini_count; i++) {
            struct stat st;
            /* Check if file on disk changed (external modification/move) */
            if (stat(ctx[i].path, &st) == 0) {
                int changed = 0;
                /* If file exists but we have no handle, open it */
                if (!ctx[i].fp) {
                    ctx[i].fp = fopen(ctx[i].path, "r");
                    if (ctx[i].fp) {
                        changed = 1;
                        if (verbose) fprintf(stderr, "[watch] file %d (%s) appeared, loading...\n", i, ctx[i].path);
                    }
                } else {
                    /* Handle has inode? */
                    if (st.st_ino != ctx[i].inode) {
                        /* File replaced (mv) */
                        if (verbose) fprintf(stderr, "[watch] file %d (%s) replaced, reloading...\n", i, ctx[i].path);
                        fclose(ctx[i].fp);
                        ctx[i].fp = fopen(ctx[i].path, "r");
                        if (ctx[i].fp) {
                            changed = 1;
                        } else {
                            if (verbose) fprintf(stderr, "[watch] file %d (%s) replaced but unreadable, clearing...\n", i, ctx[i].path);
                            ini_init(&ctx[i].store);
                            ctx[i].mtime = 0;
                            ctx[i].size = 0;
                            ctx[i].inode = 0;
                        }
                    }
                }

                /* Force changed=1 if file present, to support fast updates (poor mtime resolution) */
                if (ctx[i].fp) changed = 1;

                if (changed && ctx[i].fp) {
                    /* Rewind and re-parse unconditionally */
                    rewind(ctx[i].fp);
                    ini_init(&ctx[i].store);
                    ini_parse_stream(&ctx[i].store, ctx[i].fp);

                    /* Refresh stats for inode check */
                    if (fstat(fileno(ctx[i].fp), &st) == 0) {
                         ctx[i].mtime = st.st_mtime;
                         ctx[i].size = st.st_size;
                         ctx[i].inode = st.st_ino;
                    }
                }
            } else {
                /* File missing */
                if (ctx[i].fp) {
                    if (verbose) fprintf(stderr, "[watch] file %d (%s) gone, clearing...\n", i, ctx[i].path);
                    fclose(ctx[i].fp);
                    ctx[i].fp = NULL;
                    ini_init(&ctx[i].store);
                    ctx[i].mtime = 0;
                    ctx[i].size = 0;
                    ctx[i].inode = 0;
                }
            }
        }

        refresh_cli_store(&cli_store, hostapd_iface, hostapd_sta, wpa_iface, rtl8812_iface, cpu_enable, &cpu_state,
            venc_url, verbose);

        uint64_t now_ms = now_mono_ms();
        int retry_due = (retry_delta.active && next_retry_at_ms != 0 && now_ms >= next_retry_at_ms);
        int retry_pending_not_due = (retry_delta.active && !retry_due);

        int any_changed = 0;
        PendingDelta delta;
        pending_delta_init(&delta);
        payload_init(&delta.payload);

        for (int i = 0; i < 8; i++) {
            if (w.value_used[i] && w.value_rhs[i]) {
                double dv = 0.0;
                ValueState st = resolve_watch_value_rhs(w.value_rhs[i], &cli_store, ctx, ini_count, i, verbose, &dv);

                int changed = 0;
                if (st != w.last_v_state[i]) changed = 1;
                else if (st == VS_NUM && dv != w.last_v[i]) changed = 1;
                if (changed && retry_pending_not_due && retry_delta.pending_v[i]) {
                    int same_pending = (st == retry_delta.next_v_state[i]);
                    if (same_pending && st == VS_NUM) same_pending = (dv == retry_delta.next_v[i]);
                    if (same_pending) changed = 0;
                }

                if (changed) {
                    if (verbose) {
                        if (st == VS_NUM) fprintf(stderr, "[watch] change values[%d]=%.1f\n", i, dv);
                        else if (st == VS_EMPTY) fprintf(stderr, "[watch] change values[%d]=\"\" (clear)\n", i);
                        else fprintf(stderr, "[watch] change values[%d]=null (ignore)\n", i);
                    }

                    delta.next_v_state[i] = st;
                    delta.next_v[i] = dv;
                    delta.pending_v[i] = 1;
                    if (st == VS_NUM) { set_value_num(&delta.payload, i, dv); }
                    else if (st == VS_EMPTY) { set_value_empty(&delta.payload, i); }
                    else { set_value_null(&delta.payload, i); }
                    any_changed = 1;
                }
            }

            if (w.text_used[i] && w.text_rhs[i]) {
                char t[MAX_TEXT_LEN + 1];
                TextState st = resolve_watch_text_rhs(w.text_rhs[i], &cli_store, ctx, ini_count, i, verbose, t, sizeof(t));

                int changed = 0;
                if (st != w.last_t_state[i]) changed = 1;
                else if (st == TS_STR && strcmp(t, w.last_t[i]) != 0) changed = 1;
                if (changed && retry_pending_not_due && retry_delta.pending_t[i]) {
                    int same_pending = (st == retry_delta.next_t_state[i]);
                    if (same_pending && st == TS_STR) same_pending = (strcmp(t, retry_delta.next_t[i]) == 0);
                    if (same_pending) changed = 0;
                }

                if (changed) {
                    if (verbose) {
                        if (st == TS_STR) fprintf(stderr, "[watch] change texts[%d]=\"%s\"\n", i, t);
                        else fprintf(stderr, "[watch] change texts[%d]=null (ignore)\n", i);
                    }

                    delta.next_t_state[i] = st;
                    delta.pending_t[i] = 1;
                    if (st == TS_STR) {
                        copy_cstr(delta.next_t[i], sizeof(delta.next_t[i]), t);
                        set_text_str(&delta.payload, i, t);
                    } else {
                        delta.next_t[i][0] = '\0';
                        set_text_null(&delta.payload, i);
                    }
                    any_changed = 1;
                }
            }
        }

        if (any_changed && retry_delta.active && !retry_due) {
            retry_delta = delta;
            retry_delta.active = 1;
        }

        PendingDelta *send_delta = NULL;
        if (any_changed) {
            if (!retry_delta.active || retry_due) send_delta = &delta;
        } else if (retry_due) {
            send_delta = &retry_delta;
        }

        if (send_delta) {
            if (verbose && send_delta == &retry_delta) {
                fprintf(stderr, "[watch] retrying pending update\n");
            }

            char resolved_dest[64];
            int resolved_port = DEFAULT_PORT;
            if (!resolve_watch_endpoint(dest_raw, port_raw, &cli_store, ctx, ini_count,
                    resolved_dest, sizeof(resolved_dest), &resolved_port, verbose, 0)) {
                next_retry_at_ms = now_ms + (uint64_t)retry_ms;
                retry_delta = *send_delta;
                retry_delta.active = 1;
                if (verbose) fprintf(stderr, "[watch] endpoint resolve failed; retrying in %dms\n", retry_ms);
            } else {
                if (strcmp(dest, resolved_dest) != 0 || port != resolved_port) {
                    if (verbose) fprintf(stderr, "[watch] endpoint changed => %s:%d\n", resolved_dest, resolved_port);
                    copy_cstr(dest, sizeof(dest), resolved_dest);
                    port = resolved_port;
                }

                char out[BUILD_BUF];
                int out_len = serialize_payload(&send_delta->payload, out, sizeof(out));
                if (out_len == -2) {
                    fprintf(stderr, "[watch] payload exceeds %d bytes, skipping\n", MAX_PAYLOAD);
                    next_retry_at_ms = now_ms + (uint64_t)retry_ms;
                    retry_delta = *send_delta;
                    retry_delta.active = 1;
                } else if (out_len < 0) {
                    fprintf(stderr, "[watch] failed to serialize payload\n");
                    next_retry_at_ms = now_ms + (uint64_t)retry_ms;
                    retry_delta = *send_delta;
                    retry_delta.active = 1;
                } else {
                    if (verbose) fprintf(stderr, "[watch] send len=%d json=%s\n", out_len, out);
                    if (send_udp(sock, dest, port, out, (size_t)out_len) < 0) {
                        perror("sendto(watch)");
                        next_retry_at_ms = now_ms + (uint64_t)retry_ms;
                        retry_delta = *send_delta;
                        retry_delta.active = 1;
                        if (verbose) fprintf(stderr, "[watch] send failed; retrying in %dms\n", retry_ms);
                    } else {
                        next_retry_at_ms = 0;
                        retry_delta.active = 0;
                        pending_delta_commit(send_delta, &w);
                    }
                }
            }
        }

        usleep((useconds_t)interval_ms * 1000);
    }

    close(sock);
    watchspec_free(&w);
    for (int i=0; i<ini_count; i++) if (ctx[i].fp) fclose(ctx[i].fp);
    free(ctx);
    return 0;
}

/* ------------------------- main ------------------------- */

int main(int argc, char **argv)
{
    const char *prog = argv[0];

    if (argc < 2) {
        usage_main(prog);
        return 1;
    }

    if (!strcmp(argv[1], "send")) {
        return cmd_send(argc - 1, argv + 1, prog);
    }
    if (!strcmp(argv[1], "watch")) {
        return cmd_watch(argc - 1, argv + 1, prog);
    }

    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage_main(prog);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    usage_main(prog);
    return 1;
}
