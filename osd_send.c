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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PAYLOAD      1280
#define BUILD_BUF        1900

#define MAX_TEXT_LEN     96

#define INI_MAX_KV       512
#define INI_KEY_MAX      64
#define INI_VAL_MAX      256

#define MAX_INI_PATHS    32
#define DEFAULT_DEST_IP  "127.0.0.1"
#define DEFAULT_PORT     7777
#define DEFAULT_INTERVAL 64

typedef struct {
    char key[INI_KEY_MAX];
    char val[INI_VAL_MAX];
} IniKV;

typedef struct {
    IniKV kv[INI_MAX_KV];
    int count;
    int loaded;
} IniStore;

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

static int parse_int(const char *s, int *out)
{
    if (!s) return 0;
    char tmp[64];
    strncpy(tmp, s, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
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
    strncpy(tmp, s, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
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

/* ------------------------- INI ------------------------- */

static void ini_init(IniStore *ini) { memset(ini, 0, sizeof(*ini)); }

static int ini_set(IniStore *ini, const char *k, const char *v)
{
    if (!k || !*k) return 0;

    for (int i = 0; i < ini->count; i++) {
        if (!strcmp(ini->kv[i].key, k)) {
            strncpy(ini->kv[i].val, v ? v : "", INI_VAL_MAX - 1);
            ini->kv[i].val[INI_VAL_MAX - 1] = '\0';
            return 1;
        }
    }

    if (ini->count >= INI_MAX_KV) return 0;
    strncpy(ini->kv[ini->count].key, k, INI_KEY_MAX - 1);
    ini->kv[ini->count].key[INI_KEY_MAX - 1] = '\0';
    strncpy(ini->kv[ini->count].val, v ? v : "", INI_VAL_MAX - 1);
    ini->kv[ini->count].val[INI_VAL_MAX - 1] = '\0';
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
        strncpy(out, v, out_sz - 1);
        out[out_sz - 1] = '\0';
        return 1;
    }

    strncpy(out, in, out_sz - 1);
    out[out_sz - 1] = '\0';
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
    strncpy(local.sun_path, local_path, sizeof(local.sun_path) - 1);
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
    strncpy(dst.sun_path, dst_path, sizeof(dst.sun_path) - 1);

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

static void parse_hostapd_opt(const char *arg, char *iface_out, size_t iface_sz, char *mac_out, size_t mac_sz)
{
    if (!arg) return;
    char tmp[128];
    strncpy(tmp, arg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *comma = strchr(tmp, ',');
    if (comma) {
        *comma = '\0';
        char *iface = trim(tmp);
        char *mac = trim(comma + 1);
        if (iface_out && iface_sz > 0) {
            strncpy(iface_out, iface, iface_sz - 1);
            iface_out[iface_sz - 1] = '\0';
        }
        if (mac_out && mac_sz > 0) {
            strncpy(mac_out, mac, mac_sz - 1);
            mac_out[mac_sz - 1] = '\0';
        }
    } else {
        if (mac_out && mac_sz > 0) {
            strncpy(mac_out, arg, mac_sz - 1);
            mac_out[mac_sz - 1] = '\0';
        }
    }
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
    if (!ctrl_request_with_dirs(dirs, iface, "SIGNAL_POLL", buf, sizeof(buf), 1000, verbose)) {
        if (verbose) fprintf(stderr, "[wpa] control request failed\n");
        return 0;
    }

    out->loaded = 1;
    (void)ini_parse_kv_buffer(out, buf);
    if (verbose) fprintf(stderr, "[wpa] parsed %d fields\n", out->count);
    return 1;
}

static void refresh_cli_store(IniStore *cli, const char *hostapd_iface, const char *hostapd_sta, const char *wpa_iface, int verbose)
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
    strncpy(p->texts[idx], clamped, MAX_TEXT_LEN);
    p->texts[idx][MAX_TEXT_LEN] = '\0';
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
                if (!appendf(out, out_cap, &len, "%s%.3f", sep, p->values[i])) return -1;
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

/* ------------------------- parsing specs ------------------------- */

static int parse_index_value_pair(const char *s, int *idx, char *val_buf, size_t val_sz)
{
    const char *eq = strchr(s, '=');
    if (!eq) return 0;

    char left[32];
    size_t ln = (size_t)(eq - s);
    if (ln == 0 || ln >= sizeof(left)) return 0;
    memcpy(left, s, ln);
    left[ln] = '\0';

    int i;
    if (!parse_int(trim(left), &i) || i < 0 || i > 7) return 0;

    const char *rv = eq + 1; /* IMPORTANT: allow empty RHS */
    /* do NOT trim away emptiness; trim spaces but keep empty */
    char tmp[INI_VAL_MAX];
    strncpy(tmp, rv, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *t = trim(tmp);

    if (val_buf && val_sz > 0) {
        strncpy(val_buf, t, val_sz - 1);
        val_buf[val_sz - 1] = '\0';
    }
    *idx = i;
    return 1;
}

/* Send-mode apply: missing @key => null (ignored). Empty => "" (clear). */
static int apply_values_list_send(Payload *p, const IniStore *ini, const char *spec, int verbose)
{
    if (!spec) return 1;

    char buf[700];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok) continue;

        int idx;
        char rhs[INI_VAL_MAX];
        if (!parse_index_value_pair(tok, &idx, rhs, sizeof(rhs))) {
            fprintf(stderr, "Bad --values entry: %s\n", tok);
            return 0;
        }

        /* literal null => null placeholder (ignored) */
        if (is_literal_null(rhs)) {
            if (verbose) fprintf(stderr, "[send] values[%d]=null (literal)\n", idx);
            set_value_null(p, idx);
            continue;
        }

        /* empty RHS => "" => clear numeric slot (backend clears to 0) */
        if (rhs[0] == '\0') {
            if (verbose) fprintf(stderr, "[send] values[%d]=\"\" (clear)\n", idx);
            set_value_empty(p, idx);
            continue;
        }

        /* resolve @key; missing => null */
        char resolved[INI_VAL_MAX];
        if (!resolve_ini_ref(ini, rhs, resolved, sizeof(resolved))) {
            if (verbose) fprintf(stderr, "[send] values[%d]=null (missing %s)\n", idx, rhs);
            set_value_null(p, idx);
            continue;
        }

        /* resolved empty => "" clear */
        if (resolved[0] == '\0') {
            if (verbose) fprintf(stderr, "[send] values[%d]=\"\" (ini empty %s)\n", idx, rhs);
            set_value_empty(p, idx);
            continue;
        }

        /* resolved "null" => null */
        if (is_literal_null(resolved)) {
            if (verbose) fprintf(stderr, "[send] values[%d]=null (ini 'null' %s)\n", idx, rhs);
            set_value_null(p, idx);
            continue;
        }

        double dv;
        if (!parse_double(resolved, &dv)) {
            /* non-numeric => null (ignore) */
            if (verbose) fprintf(stderr, "[send] values[%d]=null (non-numeric '%s' from %s)\n", idx, resolved, rhs);
            set_value_null(p, idx);
            continue;
        }

        if (verbose) fprintf(stderr, "[send] values[%d]=%.3f (from %s)\n", idx, dv, rhs);
        set_value_num(p, idx, dv);
    }

    return 1;
}

static int apply_texts_list_send(Payload *p, const IniStore *ini, const char *spec, int verbose)
{
    if (!spec) return 1;

    char buf[900];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok) continue;

        int idx;
        char rhs[INI_VAL_MAX];
        if (!parse_index_value_pair(tok, &idx, rhs, sizeof(rhs))) {
            fprintf(stderr, "Bad --texts entry: %s\n", tok);
            return 0;
        }

        if (is_literal_null(rhs)) {
            if (verbose) fprintf(stderr, "[send] texts[%d]=null (literal)\n", idx);
            set_text_null(p, idx);
            continue;
        }

        /* empty RHS => "" => clear text slot */
        if (rhs[0] == '\0') {
            if (verbose) fprintf(stderr, "[send] texts[%d]=\"\" (clear)\n", idx);
            set_text_str(p, idx, "");
            continue;
        }

        /* resolve @key; missing => null */
        char resolved[INI_VAL_MAX];
        if (!resolve_ini_ref(ini, rhs, resolved, sizeof(resolved))) {
            if (verbose) fprintf(stderr, "[send] texts[%d]=null (missing %s)\n", idx, rhs);
            set_text_null(p, idx);
            continue;
        }

        /* resolved "null" => null placeholder (ignored) */
        if (is_literal_null(resolved)) {
            if (verbose) fprintf(stderr, "[send] texts[%d]=null (ini 'null' %s)\n", idx, rhs);
            set_text_null(p, idx);
            continue;
        }

        /* resolved empty => clear */
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
        "  %s send  [options]\n"
        "  %s watch [options]\n"
        "\n"
        "Options (send/watch):\n"
        "  --ini <file>              ini key=value file\n"
        "  --dest <ip|@key>          destination IP (default: %s)\n"
        "  --port <n|@key>           UDP port (default: %d)\n"
        "  --values \"i=v,...\"        set values (v: number | @key | null | empty => \"\")\n"
        "  --texts  \"i=s,...\"        set texts  (s: text   | @key | null | empty => \"\")\n"
        "  --hostapd <[iface,]sta>   pull hostapd STA stats via control socket (overrides ini keys)\n"
        "  --wpa-cli <iface>         pull wpa_supplicant signal_poll via control socket (overrides ini keys)\n"
        "  --print-json              (send) print JSON instead of sending\n"
        "  --verbose, -v             extra debug output\n"
        "\n"
        "Watch-only:\n"
        "  --interval <ms>           poll interval (default: %d)\n"
        "\n"
        "Backend semantics reminder:\n"
        "  - null entries are ignored (slot keeps previous)\n"
        "  - omitted trailing indices keep previous\n"
        "  - empty string \"\" clears text slot, and clears numeric slot to 0\n"
        "\n"
        "Examples:\n"
        "  %s send --values \"0=-52\" --texts \"0=Trollvinter\"\n"
        "  %s send --ini /tmp/aalink_ext.msg --dest 10.6.0.1 --port 7777 \\\n"
        "    --values \"0=@used_rssi,1=@mcs,2=@width\" \\\n"
        "    --texts  \"0=@used_source,1=@gs_string\"\n"
        "  %s watch --ini /tmp/aalink_ext.msg --dest 10.6.0.1 --port 7777 --interval 64 \\\n"
        "    --values \"0=@used_rssi,1=@mcs\" --texts \"0=@used_source\"\n",
        prog, prog,
        DEFAULT_DEST_IP, DEFAULT_PORT, DEFAULT_INTERVAL,
        prog, prog, prog);
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

static int parse_and_store_list_rhs(char **rhs_arr, int used_arr[8], const char *spec)
{
    if (!spec) return 0;

    char buf[900];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok) continue;

        int idx;
        char rhs[INI_VAL_MAX];
        if (!parse_index_value_pair(tok, &idx, rhs, sizeof(rhs))) {
            fprintf(stderr, "Bad list entry: %s\n", tok);
            return 0;
        }

        free(rhs_arr[idx]);
        rhs_arr[idx] = xstrdup(rhs);
        if (!rhs_arr[idx]) { perror("malloc"); return 0; }
        used_arr[idx] = 1;
    }
    return 1;
}

/* Watch resolve:
 *  - missing @key => null (ignored)
 *  - empty => "" (clear)
 *  - "null" => null
 *  - numeric parse -> number
 */

/* ------------------------- SEND ------------------------- */

static int cmd_send(int argc, char **argv, const char *prog)
{
    const char *dest_raw = NULL;
    const char *port_raw = NULL;
    const char *ini_paths[MAX_INI_PATHS];
    int ini_count = 0;
    const char *values_spec = NULL;
    const char *texts_spec = NULL;
    int print_json = 0;
    int verbose = 0;
    const char *hostapd_opt = NULL;
    const char *wpa_iface = NULL;
    char hostapd_iface[64] = {0};
    char hostapd_sta[64] = {0};

    IniStore ini;
    ini_init(&ini);
    IniStore cli_store;
    ini_init(&cli_store);

    Payload payload;
    payload_init(&payload);

    static struct option opts[] = {
        {"dest", required_argument, 0, 1},
        {"port", required_argument, 0, 2},
        {"ini",  required_argument, 0, 3},
        {"values", required_argument, 0, 5},
        {"texts", required_argument, 0, 7},
        {"hostapd", required_argument, 0, 11},
        {"wpa-cli", required_argument, 0, 12},
        {"print-json", no_argument, 0, 9},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    /* Grab ini first (if provided) */
    optind = 1;
    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "hv", opts, &idx)) != -1) {
        if (opt == 3) {
            if (ini_count < MAX_INI_PATHS) {
                ini_paths[ini_count++] = optarg;
            } else {
                fprintf(stderr, "Warning: too many ini files, ignoring %s\n", optarg);
            }
        }
        if (opt == 'h') { usage_main(prog); return 0; }
        if (opt == 'v') verbose = 1;
    }

    if (ini_count > 0) {
        for (int i = 0; i < ini_count; i++) {
            if (!ini_add_file(&ini, ini_paths[i])) {
                if (verbose) fprintf(stderr, "[send] ini not readable: %s (%s)\n", ini_paths[i], strerror(errno));
            }
        }
    }

    /* Parse again for actual options */
    optind = 1;
    idx = 0;
    while ((opt = getopt_long(argc, argv, "hv", opts, &idx)) != -1) {
        switch (opt) {
        case 1: dest_raw = optarg; break;
        case 2: port_raw = optarg; break;
        case 3: /* already handled */ break;
        case 5: values_spec = optarg; break;
        case 7: texts_spec = optarg; break;
        case 11: hostapd_opt = optarg; break;
        case 12: wpa_iface = optarg; break;
        case 9: print_json = 1; break;
        case 'v': verbose = 1; break;
        case 'h':
        default:
            usage_main(prog);
            return (opt == 'h') ? 0 : 1;
        }
    }

    parse_hostapd_opt(hostapd_opt, hostapd_iface, sizeof(hostapd_iface), hostapd_sta, sizeof(hostapd_sta));
    refresh_cli_store(&cli_store, hostapd_iface, hostapd_sta, wpa_iface, verbose);
    ini_merge(&ini, &cli_store);

    if (!dest_raw) dest_raw = DEFAULT_DEST_IP;

    char dest[64];
    if (dest_raw[0] == '@') {
        const char *v = ini_get(&ini, dest_raw + 1);
        if (!v || !*v) {
            /* default if missing */
            strncpy(dest, DEFAULT_DEST_IP, sizeof(dest) - 1);
            dest[sizeof(dest) - 1] = '\0';
            if (verbose) fprintf(stderr, "[send] --dest %s missing => default %s\n", dest_raw, dest);
        } else {
            strncpy(dest, v, sizeof(dest) - 1);
            dest[sizeof(dest) - 1] = '\0';
        }
    } else {
        strncpy(dest, dest_raw, sizeof(dest) - 1);
        dest[sizeof(dest) - 1] = '\0';
    }

    int port = DEFAULT_PORT;
    if (port_raw) {
        char pbuf[64];
        if (!resolve_ini_ref(&ini, port_raw, pbuf, sizeof(pbuf))) {
            /* default if missing */
            port = DEFAULT_PORT;
            if (verbose) fprintf(stderr, "[send] --port %s missing => default %d\n", port_raw, port);
        } else {
            port = atoi(pbuf);
        }
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid port\n");
        return 1;
    }

    if (values_spec) {
        if (!apply_values_list_send(&payload, &ini, values_spec, verbose)) return 1;
    }
    if (texts_spec) {
        if (!apply_texts_list_send(&payload, &ini, texts_spec, verbose)) return 1;
    }

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
    const char *dest_raw = NULL;
    const char *port_raw = NULL;
    const char *ini_paths[MAX_INI_PATHS];
    int ini_count = 0;
    int interval_ms = DEFAULT_INTERVAL;
    int verbose = 0;
    const char *hostapd_opt = NULL;
    const char *wpa_iface = NULL;
    char hostapd_iface[64] = {0};
    char hostapd_sta[64] = {0};
    IniStore cli_store;
    ini_init(&cli_store);

    WatchSpec w;
    watchspec_init(&w);

    static struct option opts[] = {
        {"dest", required_argument, 0, 1},
        {"port", required_argument, 0, 2},
        {"ini",  required_argument, 0, 3},
        {"values", required_argument, 0, 5},
        {"texts", required_argument, 0, 7},
        {"interval", required_argument, 0, 10},
        {"hostapd", required_argument, 0, 11},
        {"wpa-cli", required_argument, 0, 12},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    optind = 1;
    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "hv", opts, &idx)) != -1) {
        switch (opt) {
        case 1: dest_raw = optarg; break;
        case 2: port_raw = optarg; break;
        case 3:
            if (ini_count < MAX_INI_PATHS) {
                ini_paths[ini_count++] = optarg;
            } else {
                fprintf(stderr, "Warning: too many ini files, ignoring %s\n", optarg);
            }
            break;
        case 5:
            if (!parse_and_store_list_rhs(w.value_rhs, w.value_used, optarg)) { watchspec_free(&w); return 1; }
            break;
        case 7:
            if (!parse_and_store_list_rhs(w.text_rhs, w.text_used, optarg)) { watchspec_free(&w); return 1; }
            break;
        case 10:
            interval_ms = atoi(optarg);
            break;
        case 11:
            hostapd_opt = optarg;
            break;
        case 12:
            wpa_iface = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
        default:
            usage_main(prog);
            watchspec_free(&w);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (interval_ms < 5) interval_ms = 5;

    if (!watchspec_any(&w)) {
        fprintf(stderr, "Error: watch needs at least one --values or --texts\n");
        usage_main(prog);
        watchspec_free(&w);
        return 1;
    }

    int has_cli_source = (hostapd_opt && *hostapd_opt) || (wpa_iface && *wpa_iface);
    if (ini_count == 0 && !has_cli_source) {
        fprintf(stderr, "Error: at least one --ini, --hostapd, or --wpa-cli must be specified\n");
        usage_main(prog);
        watchspec_free(&w);
        return 1;
    }

    if (!dest_raw) dest_raw = DEFAULT_DEST_IP;

    IniContext *ctx = NULL;
    if (ini_count > 0) {
        ctx = (IniContext *)calloc(ini_count, sizeof(IniContext));
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

    parse_hostapd_opt(hostapd_opt, hostapd_iface, sizeof(hostapd_iface), hostapd_sta, sizeof(hostapd_sta));
    refresh_cli_store(&cli_store, hostapd_iface, hostapd_sta, wpa_iface, verbose);

    char dest[64];
    if (dest_raw[0] == '@') {
        const char *k = dest_raw + 1;
        const char *found = lookup_from_sources(&cli_store, ctx, ini_count, k);

        if (!found || !*found) {
            strncpy(dest, DEFAULT_DEST_IP, sizeof(dest) - 1);
            dest[sizeof(dest) - 1] = '\0';
            if (verbose) fprintf(stderr, "[watch] --dest %s missing => default %s\n", dest_raw, dest);
        } else {
            strncpy(dest, found, sizeof(dest) - 1);
            dest[sizeof(dest) - 1] = '\0';
        }
    } else {
        strncpy(dest, dest_raw, sizeof(dest) - 1);
        dest[sizeof(dest) - 1] = '\0';
    }

    int port = DEFAULT_PORT;
    if (port_raw) {
        /* Re-implement logic properly */
        char pbuf[64];
        int resolved = 0;

        if (port_raw[0] != '@') {
            strncpy(pbuf, port_raw, sizeof(pbuf)-1);
            pbuf[sizeof(pbuf)-1] = '\0';
            resolved = 1;
        } else {
            const char *key = port_raw + 1;
            const char *v = lookup_from_sources(&cli_store, ctx, ini_count, key);
            if (v) {
                strncpy(pbuf, v, sizeof(pbuf)-1);
                pbuf[sizeof(pbuf)-1] = '\0';
                resolved = 1;
            }
        }

        if (!resolved) {
            port = DEFAULT_PORT;
            if (verbose) fprintf(stderr, "[watch] --port %s missing => default %d\n", port_raw, port);
        } else {
            port = atoi(pbuf);
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid port\n");
        watchspec_free(&w);
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

    /* baseline send */
    {
        Payload pb;
        payload_init(&pb);

        if (verbose && ini_count > 0 && !have_ini0) fprintf(stderr, "[watch] baseline: ini unreadable -> all watched @keys treated as null\n");
        if (verbose && ini_count == 0 && has_cli_source) fprintf(stderr, "[watch] baseline: using control-socket data only (no ini files)\n");

        /* We must duplicate the logic here or make a helper. Inline for now. */
        for (int i = 0; i < 8; i++) {
            if (w.value_used[i] && w.value_rhs[i]) {
                double dv = 0.0;
                /* Manual resolve loop for baseline */
                ValueState st = VS_NULL;
                const char *rhs = w.value_rhs[i];
                /* Copied resolve logic */
                if (!rhs) st = VS_NULL;
                else if (is_literal_null(rhs)) st = VS_NULL;
                else if (rhs[0] == '\0') st = VS_EMPTY;
                else if (rhs[0] == '@') {
                    const char *k = rhs + 1;
                    const char *found = lookup_from_sources(&cli_store, ctx, ini_count, k);
                    if (!found) {
                        if (verbose) fprintf(stderr, "[watch] values[%d] missing %s => null\n", i, rhs);
                        st = VS_NULL;
                    } else if (found[0] == '\0') {
                        st = VS_EMPTY;
                    } else if (is_literal_null(found)) {
                        st = VS_NULL;
                    } else {
                        if (parse_double(found, &dv)) st = VS_NUM;
                        else {
                            if (verbose) fprintf(stderr, "[watch] values[%d] non-numeric '%s' from %s => null\n", i, found, rhs);
                            st = VS_NULL;
                        }
                    }
                } else {
                    if (parse_double(rhs, &dv)) st = VS_NUM;
                }

                w.last_v_state[i] = st;
                if (st == VS_NUM) { w.last_v[i] = dv; set_value_num(&pb, i, dv); }
                else if (st == VS_EMPTY) { set_value_empty(&pb, i); }
                else { set_value_null(&pb, i); }
            }
            if (w.text_used[i] && w.text_rhs[i]) {
                char t[MAX_TEXT_LEN + 1];
                TextState st = TS_NULL;
                const char *rhs = w.text_rhs[i];
                if (!rhs) st = TS_NULL;
                else if (is_literal_null(rhs)) st = TS_NULL;
                else if (rhs[0] == '\0') { t[0]='\0'; st = TS_STR; }
                else if (rhs[0] == '@') {
                    const char *k = rhs + 1;
                    const char *found = lookup_from_sources(&cli_store, ctx, ini_count, k);
                    if (!found) {
                        if (verbose) fprintf(stderr, "[watch] texts[%d] missing %s => null\n", i, rhs);
                        st = TS_NULL;
                    } else if (is_literal_null(found)) {
                        st = TS_NULL;
                    } else {
                        clamp_textN(found, t, MAX_TEXT_LEN + 1);
                        st = TS_STR;
                    }
                } else {
                    clamp_textN(rhs, t, MAX_TEXT_LEN + 1);
                    st = TS_STR;
                }

                w.last_t_state[i] = st;
                if (st == TS_STR) {
                    strncpy(w.last_t[i], t, MAX_TEXT_LEN);
                    w.last_t[i][MAX_TEXT_LEN] = '\0';
                    set_text_str(&pb, i, t);
                } else {
                    w.last_t[i][0] = '\0';
                    set_text_null(&pb, i);
                }
            }
        }

        char out[BUILD_BUF];
        int out_len = serialize_payload(&pb, out, sizeof(out));
        if (out_len < 0) {
            fprintf(stderr, "Error: baseline payload build failed\n");
            close(sock);
            watchspec_free(&w);
            for (int i=0; i<ini_count; i++) if (ctx[i].fp) fclose(ctx[i].fp);
            free(ctx);
            return 1;
        }

        if (verbose) fprintf(stderr, "[watch] baseline send len=%d json=%s\n", out_len, out);

        if (send_udp(sock, dest, port, out, (size_t)out_len) < 0) {
            perror("sendto(baseline)");
            close(sock);
            watchspec_free(&w);
            for (int i=0; i<ini_count; i++) if (ctx[i].fp) fclose(ctx[i].fp);
            free(ctx);
            return 1;
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
                        changed = 1;
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

        refresh_cli_store(&cli_store, hostapd_iface, hostapd_sta, wpa_iface, verbose);

        int any_changed = 0;
        Payload pb;
        payload_init(&pb);

        for (int i = 0; i < 8; i++) {
            if (w.value_used[i] && w.value_rhs[i]) {
                double dv = 0.0;
                /* Inline resolve logic again since we can't use helper easily with ctx array */
                ValueState st = VS_NULL;
                const char *rhs = w.value_rhs[i];
                if (!rhs) st = VS_NULL;
                else if (is_literal_null(rhs)) st = VS_NULL;
                else if (rhs[0] == '\0') st = VS_EMPTY;
                else if (rhs[0] == '@') {
                    const char *k = rhs + 1;
                    const char *found = lookup_from_sources(&cli_store, ctx, ini_count, k);
                    if (!found) {
                        /* Keep silent if missing, or verbose? Previous log had verbose */
                         if (verbose) fprintf(stderr, "[watch] values[%d] missing %s => null\n", i, rhs);
                        st = VS_NULL;
                    } else if (found[0] == '\0') {
                        st = VS_EMPTY;
                    } else if (is_literal_null(found)) {
                        st = VS_NULL;
                    } else {
                        if (parse_double(found, &dv)) st = VS_NUM;
                        else st = VS_NULL;
                    }
                } else {
                    if (parse_double(rhs, &dv)) st = VS_NUM;
                }

                int changed = 0;
                if (st != w.last_v_state[i]) changed = 1;
                else if (st == VS_NUM && dv != w.last_v[i]) changed = 1;

                if (changed) {
                    if (verbose) {
                        if (st == VS_NUM) fprintf(stderr, "[watch] change values[%d]=%.3f\n", i, dv);
                        else if (st == VS_EMPTY) fprintf(stderr, "[watch] change values[%d]=\"\" (clear)\n", i);
                        else fprintf(stderr, "[watch] change values[%d]=null (ignore)\n", i);
                    }

                    w.last_v_state[i] = st;
                    if (st == VS_NUM) { w.last_v[i] = dv; set_value_num(&pb, i, dv); }
                    else if (st == VS_EMPTY) { set_value_empty(&pb, i); }
                    else { set_value_null(&pb, i); }
                    any_changed = 1;
                }
            }

            if (w.text_used[i] && w.text_rhs[i]) {
                char t[MAX_TEXT_LEN + 1];
                TextState st = TS_NULL;
                const char *rhs = w.text_rhs[i];
                if (!rhs) st = TS_NULL;
                else if (is_literal_null(rhs)) st = TS_NULL;
                else if (rhs[0] == '\0') { t[0]='\0'; st = TS_STR; }
                else if (rhs[0] == '@') {
                    const char *k = rhs + 1;
                    const char *found = lookup_from_sources(&cli_store, ctx, ini_count, k);
                    if (!found) {
                        if (verbose) fprintf(stderr, "[watch] texts[%d] missing %s => null\n", i, rhs);
                        st = TS_NULL;
                    } else if (is_literal_null(found)) {
                        st = TS_NULL;
                    } else {
                        clamp_textN(found, t, MAX_TEXT_LEN + 1);
                        st = TS_STR;
                    }
                } else {
                    clamp_textN(rhs, t, MAX_TEXT_LEN + 1);
                    st = TS_STR;
                }

                int changed = 0;
                if (st != w.last_t_state[i]) changed = 1;
                else if (st == TS_STR && strcmp(t, w.last_t[i]) != 0) changed = 1;

                if (changed) {
                    if (verbose) {
                        if (st == TS_STR) fprintf(stderr, "[watch] change texts[%d]=\"%s\"\n", i, t);
                        else fprintf(stderr, "[watch] change texts[%d]=null (ignore)\n", i);
                    }

                    w.last_t_state[i] = st;
                    if (st == TS_STR) {
                        strncpy(w.last_t[i], t, MAX_TEXT_LEN);
                        w.last_t[i][MAX_TEXT_LEN] = '\0';
                        set_text_str(&pb, i, t);
                    } else {
                        w.last_t[i][0] = '\0';
                        set_text_null(&pb, i);
                    }
                    any_changed = 1;
                }
            }
        }

        if (any_changed) {
            char out[BUILD_BUF];
            int out_len = serialize_payload(&pb, out, sizeof(out));
            if (out_len == -2) {
                fprintf(stderr, "[watch] payload exceeds %d bytes, skipping\n", MAX_PAYLOAD);
            } else if (out_len < 0) {
                fprintf(stderr, "[watch] failed to serialize payload\n");
            } else {
                if (verbose) fprintf(stderr, "[watch] send len=%d json=%s\n", out_len, out);
                if (send_udp(sock, dest, port, out, (size_t)out_len) < 0) {
                    perror("sendto(watch)");
                    break;
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
