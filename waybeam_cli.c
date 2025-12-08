/*
 * waybeam.c - UDP OSD control CLI
 *
 * Implements:
 *   waybeam send   --dest IP [--port N] [values/texts/assets/timestamp] [--print-json]
 *   waybeam send   --dest IP [--port N] [baseline flags...] --stdin   (JSON Lines)
 *   waybeam watch  --dest IP [--port N] [--interval ms] --value-index i... --text-index j... <iface>
 *   waybeam asset  (alias of send)
 *
 * Contract alignment:
 *   - JSON UTF-8 datagrams
 *   - Top-level: values, texts, asset_updates, timestamp_ms
 *   - Omit fields if not provided (send mode)
 *   - New max payload limit: 1280 bytes (hard enforced)
 *   - texts clamped to 16 chars
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o waybeam waybeam.c
 */
#include <stdarg.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAYLOAD 1280
#define BUILD_BUF   1400

#define MAX_TEXT_LEN 16

/* ------------------------- helpers ------------------------- */

static void die_usage(const char *prog);

static long monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static int parse_int(const char *s, int *out)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (*end != '\0') return 0;
    if (v < -2147483648L || v > 2147483647L) return 0;
    *out = (int)v;
    return 1;
}

static int parse_long(const char *s, long *out)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

static int parse_double(const char *s, double *out)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

static int parse_bool(const char *s, int *out)
{
    if (!s) return 0;
    if (!strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "on")) {
        *out = 1; return 1;
    }
    if (!strcasecmp(s, "false") || !strcmp(s, "0") || !strcasecmp(s, "no") || !strcasecmp(s, "off")) {
        *out = 0; return 1;
    }
    return 0;
}

static void clamp_text16(const char *in, char out[MAX_TEXT_LEN + 1])
{
    if (!in) { out[0] = '\0'; return; }
    strncpy(out, in, MAX_TEXT_LEN);
    out[MAX_TEXT_LEN] = '\0';
}

/* Minimal JSON string escape (enough for SSID/labels). */
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

/* Trim leading/trailing whitespace in-place. */
static char *trim(char *s)
{
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

/* Very light JSON line sanity check (JSON-only mode). */
static int looks_like_json_object(const char *line)
{
    if (!line) return 0;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{') return 0;

    const char *e = line + strlen(line);
    while (e > p && isspace((unsigned char)e[-1])) e--;
    if (e <= p) return 0;
    if (e[-1] != '}') return 0;
    return 1;
}

/* ------------------------- UDP ------------------------- */

static int open_udp_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    return fd;
}

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
    if (sent < 0) return -1;
    return 0;
}

/* ------------------------- iw reader (watch) ------------------------- */

static int read_iw_link(const char *iface, char *ssid_out, size_t ssid_sz, double *signal_dbm_out)
{
    if (!iface || !ssid_out || ssid_sz == 0 || !signal_dbm_out)
        return 0;

    ssid_out[0] = '\0';
    *signal_dbm_out = 0.0;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "iw dev %s link 2>&1", iface);

    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char line[256];
    int have_ssid = 0;
    int have_signal = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "SSID:");
        if (p) {
            p += 5;
            while (*p == ' ' || *p == '\t') p++;
            size_t n = strcspn(p, "\r\n");
            if (n >= ssid_sz) n = ssid_sz - 1;
            memcpy(ssid_out, p, n);
            ssid_out[n] = '\0';
            if (ssid_out[0] != '\0')
                have_ssid = 1;
            continue;
        }

        p = strstr(line, "signal:");
        if (p) {
            double s = 0.0;
            if (sscanf(p, "signal: %lf", &s) == 1) {
                *signal_dbm_out = s;
                have_signal = 1;
            }
            continue;
        }

        if (strstr(line, "Not connected") || strstr(line, "not connected")) {
            have_ssid = 0;
            have_signal = 0;
        }
    }

    pclose(fp);
    return (have_ssid && have_signal);
}

static uint32_t color_for_signal_dbm(double s)
{
    if (s >= -50.0) return 0x00FF00; // green
    if (s >= -60.0) return 0xFFFF00; // yellow
    if (s >= -70.0) return 0xFFA500; // orange
    return 0xFF0000;                 // red
}

/* ------------------------- asset update model ------------------------- */

typedef struct {
    int id;
    int used;

    int enabled_set; int enabled;

    int type_set; char type[8];              // "bar" or "text"
    int value_index_set; int value_index;
    int text_index_set; int text_index;

    int text_indices_set; int text_indices[8]; int text_indices_count;
    int text_inline_set; int text_inline;

    int label_set; char label[128];
    int orientation_set; char orientation[16];

    int x_set; int x;
    int y_set; int y;
    int width_set; int width;
    int height_set; int height;

    int min_set; double minv;
    int max_set; double maxv;

    int bar_color_set; long bar_color;
    int text_color_set; long text_color;

    int background_set; int background;
    int background_opacity_set; int background_opacity;

    int segments_set; int segments;
    int rounded_outline_set; int rounded_outline;
} AssetUpdate;

static AssetUpdate *find_or_add_asset(AssetUpdate *arr, int *count, int id)
{
    for (int i = 0; i < *count; i++) {
        if (arr[i].used && arr[i].id == id) return &arr[i];
    }
    if (*count >= 8) return NULL; // contract max 8 assets
    AssetUpdate *a = &arr[*count];
    memset(a, 0, sizeof(*a));
    a->used = 1;
    a->id = id;
    (*count)++;
    return a;
}

static int parse_text_indices_value(const char *val, int *out, int *out_count)
{
    // Accept separators '|', ';', ':' within the value
    // Example: text_indices=1|2|3
    if (!val || !*val) return 0;

    char tmp[128];
    strncpy(tmp, val, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    int count = 0;
    char *p = tmp;
    while (*p) {
        // find next separator
        char *sep = strpbrk(p, "|;:");
        if (sep) *sep = '\0';

        char *token = trim(p);
        if (*token) {
            int idx;
            if (!parse_int(token, &idx) || idx < 0 || idx > 7) return 0;
            if (count >= 8) return 0;
            out[count++] = idx;
        }

        if (!sep) break;
        p = sep + 1;
    }

    *out_count = count;
    return 1;
}

static int apply_asset_kv(AssetUpdate *arr, int *count, const char *spec)
{
    // spec: "id=0,enabled=true,x=50,y=50,min=-80,max=-30,bar_color=0x00FF00"
    if (!spec) return 0;

    char buf[512];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int id_found = 0;
    int id_val = -1;

    // First pass: find id
    {
        char tmp[512];
        strncpy(tmp, buf, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *save = NULL;
        for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = trim(tok);
            char *v = trim(eq + 1);
            if (!strcmp(k, "id")) {
                int id;
                if (!parse_int(v, &id) || id < 0 || id > 1024) {
                    fprintf(stderr, "Invalid asset id: %s\n", v);
                    return 0;
                }
                id_found = 1;
                id_val = id;
                break;
            }
        }
    }

    if (!id_found) {
        fprintf(stderr, "--asset missing required id=...\n");
        return 0;
    }

    AssetUpdate *a = find_or_add_asset(arr, count, id_val);
    if (!a) {
        fprintf(stderr, "Too many assets (max 8)\n");
        return 0;
    }

    // Second pass: apply keys
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(tok);
        char *v = trim(eq + 1);

        if (!strcmp(k, "id")) continue;

        if (!strcmp(k, "enabled")) {
            int b;
            if (!parse_bool(v, &b)) { fprintf(stderr, "Bad enabled=%s\n", v); return 0; }
            a->enabled_set = 1; a->enabled = b;
        } else if (!strcmp(k, "type")) {
            if (strcmp(v, "bar") && strcmp(v, "text")) {
                fprintf(stderr, "Bad type=%s\n", v); return 0;
            }
            a->type_set = 1;
            strncpy(a->type, v, sizeof(a->type) - 1);
            a->type[sizeof(a->type) - 1] = '\0';
        } else if (!strcmp(k, "value_index")) {
            int idx;
            if (!parse_int(v, &idx) || idx < 0 || idx > 7) { fprintf(stderr, "Bad value_index=%s\n", v); return 0; }
            a->value_index_set = 1; a->value_index = idx;
        } else if (!strcmp(k, "text_index")) {
            int idx;
            if (!parse_int(v, &idx) || idx < -1 || idx > 7) { fprintf(stderr, "Bad text_index=%s\n", v); return 0; }
            a->text_index_set = 1; a->text_index = idx;
        } else if (!strcmp(k, "text_indices")) {
            int tmpi[8], n = 0;
            if (!parse_text_indices_value(v, tmpi, &n)) { fprintf(stderr, "Bad text_indices=%s\n", v); return 0; }
            a->text_indices_set = 1;
            a->text_indices_count = n;
            for (int i = 0; i < n; i++) a->text_indices[i] = tmpi[i];
        } else if (!strcmp(k, "text_inline")) {
            int b;
            if (!parse_bool(v, &b)) { fprintf(stderr, "Bad text_inline=%s\n", v); return 0; }
            a->text_inline_set = 1; a->text_inline = b;
        } else if (!strcmp(k, "label")) {
            a->label_set = 1;
            strncpy(a->label, v, sizeof(a->label) - 1);
            a->label[sizeof(a->label) - 1] = '\0';
        } else if (!strcmp(k, "orientation")) {
            a->orientation_set = 1;
            strncpy(a->orientation, v, sizeof(a->orientation) - 1);
            a->orientation[sizeof(a->orientation) - 1] = '\0';
        } else if (!strcmp(k, "x")) {
            int iv;
            if (!parse_int(v, &iv)) { fprintf(stderr, "Bad x=%s\n", v); return 0; }
            a->x_set = 1; a->x = iv;
        } else if (!strcmp(k, "y")) {
            int iv;
            if (!parse_int(v, &iv)) { fprintf(stderr, "Bad y=%s\n", v); return 0; }
            a->y_set = 1; a->y = iv;
        } else if (!strcmp(k, "width")) {
            int iv;
            if (!parse_int(v, &iv)) { fprintf(stderr, "Bad width=%s\n", v); return 0; }
            a->width_set = 1; a->width = iv;
        } else if (!strcmp(k, "height")) {
            int iv;
            if (!parse_int(v, &iv)) { fprintf(stderr, "Bad height=%s\n", v); return 0; }
            a->height_set = 1; a->height = iv;
        } else if (!strcmp(k, "min")) {
            double dv;
            if (!parse_double(v, &dv)) { fprintf(stderr, "Bad min=%s\n", v); return 0; }
            a->min_set = 1; a->minv = dv;
        } else if (!strcmp(k, "max")) {
            double dv;
            if (!parse_double(v, &dv)) { fprintf(stderr, "Bad max=%s\n", v); return 0; }
            a->max_set = 1; a->maxv = dv;
        } else if (!strcmp(k, "bar_color")) {
            long lv;
            if (!parse_long(v, &lv)) { fprintf(stderr, "Bad bar_color=%s\n", v); return 0; }
            a->bar_color_set = 1; a->bar_color = lv;
        } else if (!strcmp(k, "text_color")) {
            long lv;
            if (!parse_long(v, &lv)) { fprintf(stderr, "Bad text_color=%s\n", v); return 0; }
            a->text_color_set = 1; a->text_color = lv;
        } else if (!strcmp(k, "background")) {
            int iv;
            if (!parse_int(v, &iv)) { fprintf(stderr, "Bad background=%s\n", v); return 0; }
            a->background_set = 1; a->background = iv;
        } else if (!strcmp(k, "background_opacity")) {
            int iv;
            if (!parse_int(v, &iv) || iv < 0 || iv > 100) { fprintf(stderr, "Bad background_opacity=%s\n", v); return 0; }
            a->background_opacity_set = 1; a->background_opacity = iv;
        } else if (!strcmp(k, "segments")) {
            int iv;
            if (!parse_int(v, &iv)) { fprintf(stderr, "Bad segments=%s\n", v); return 0; }
            a->segments_set = 1; a->segments = iv;
        } else if (!strcmp(k, "rounded_outline")) {
            int b;
            if (!parse_bool(v, &b)) { fprintf(stderr, "Bad rounded_outline=%s\n", v); return 0; }
            a->rounded_outline_set = 1; a->rounded_outline = b;
        } else {
            fprintf(stderr, "Unknown asset key: %s\n", k);
            return 0;
        }
    }

    return 1;
}

/* ------------------------- values/texts model ------------------------- */

typedef struct {
    int values_present[8];
    double values[8];

    int texts_present[8];
    char texts[8][MAX_TEXT_LEN + 1];

    AssetUpdate assets[8];
    int asset_count;

    int timestamp_set;
    long timestamp_ms;
} PayloadBuilder;

static void pb_init(PayloadBuilder *pb)
{
    memset(pb, 0, sizeof(*pb));
}

static int pb_set_value(PayloadBuilder *pb, int idx, double v)
{
    if (idx < 0 || idx > 7) return 0;
    pb->values_present[idx] = 1;
    pb->values[idx] = v;
    return 1;
}

static int pb_set_text(PayloadBuilder *pb, int idx, const char *s)
{
    if (idx < 0 || idx > 7) return 0;
    char clamped[MAX_TEXT_LEN + 1];
    clamp_text16(s, clamped);
    strncpy(pb->texts[idx], clamped, MAX_TEXT_LEN);
    pb->texts[idx][MAX_TEXT_LEN] = '\0';
    pb->texts_present[idx] = 1;
    return 1;
}

static int pb_add_asset_spec(PayloadBuilder *pb, const char *spec)
{
    return apply_asset_kv(pb->assets, &pb->asset_count, spec);
}

/* ------------------------- serialization ------------------------- */

static int any_values(const PayloadBuilder *pb)
{
    for (int i = 0; i < 8; i++) if (pb->values_present[i]) return 1;
    return 0;
}

static int any_texts(const PayloadBuilder *pb)
{
    for (int i = 0; i < 8; i++) if (pb->texts_present[i]) return 1;
    return 0;
}

static int any_assets(const PayloadBuilder *pb)
{
    return pb->asset_count > 0;
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

static int serialize_payload(const PayloadBuilder *pb, char *out, size_t out_cap)
{
    int len = 0;
    if (!appendf(out, out_cap, &len, "{")) return -1;

    int first_field = 1;

    /* values */
    if (any_values(pb)) {
        if (!appendf(out, out_cap, &len, "%s\"values\":[", first_field ? "" : ",")) return -1;
        first_field = 0;

        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!pb->values_present[i]) continue;
            if (!appendf(out, out_cap, &len, "%s%.3f", first ? "" : ",", pb->values[i])) return -1;
            first = 0;
        }
        if (first) {
            // Shouldn't happen because any_values() true, but be safe.
            if (!appendf(out, out_cap, &len, "0")) return -1;
        }
        if (!appendf(out, out_cap, &len, "]")) return -1;
    }

    /* texts */
    if (any_texts(pb)) {
        if (!appendf(out, out_cap, &len, "%s\"texts\":[", first_field ? "" : ",")) return -1;
        first_field = 0;

        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!pb->texts_present[i]) continue;
            char esc[128];
            json_escape(pb->texts[i], esc, sizeof(esc));
            if (!appendf(out, out_cap, &len, "%s\"%s\"", first ? "" : ",", esc)) return -1;
            first = 0;
        }
        if (first) {
            if (!appendf(out, out_cap, &len, "\"\"")) return -1;
        }
        if (!appendf(out, out_cap, &len, "]")) return -1;
    }

    /* asset_updates */
    if (any_assets(pb)) {
        if (!appendf(out, out_cap, &len, "%s\"asset_updates\":[", first_field ? "" : ",")) return -1;
        first_field = 0;

        for (int i = 0; i < pb->asset_count; i++) {
            const AssetUpdate *a = &pb->assets[i];
            if (!a->used) continue;

            if (!appendf(out, out_cap, &len, "%s{", (i == 0) ? "" : ",")) return -1;
            if (!appendf(out, out_cap, &len, "\"id\":%d", a->id)) return -1;

            if (a->enabled_set) {
                if (!appendf(out, out_cap, &len, ",\"enabled\":%s", a->enabled ? "true" : "false")) return -1;
            }
            if (a->type_set) {
                if (!appendf(out, out_cap, &len, ",\"type\":\"%s\"", a->type)) return -1;
            }
            if (a->value_index_set) {
                if (!appendf(out, out_cap, &len, ",\"value_index\":%d", a->value_index)) return -1;
            }
            if (a->text_index_set) {
                if (!appendf(out, out_cap, &len, ",\"text_index\":%d", a->text_index)) return -1;
            }
            if (a->text_indices_set) {
                if (!appendf(out, out_cap, &len, ",\"text_indices\":["))
                    return -1;
                for (int j = 0; j < a->text_indices_count; j++) {
                    if (!appendf(out, out_cap, &len, "%s%d", (j == 0) ? "" : ",", a->text_indices[j])) return -1;
                }
                if (!appendf(out, out_cap, &len, "]")) return -1;
            }
            if (a->text_inline_set) {
                if (!appendf(out, out_cap, &len, ",\"text_inline\":%s", a->text_inline ? "true" : "false")) return -1;
            }
            if (a->label_set) {
                char esc[256];
                json_escape(a->label, esc, sizeof(esc));
                if (!appendf(out, out_cap, &len, ",\"label\":\"%s\"", esc)) return -1;
            }
            if (a->orientation_set) {
                char esc[64];
                json_escape(a->orientation, esc, sizeof(esc));
                if (!appendf(out, out_cap, &len, ",\"orientation\":\"%s\"", esc)) return -1;
            }
            if (a->x_set) {
                if (!appendf(out, out_cap, &len, ",\"x\":%d", a->x)) return -1;
            }
            if (a->y_set) {
                if (!appendf(out, out_cap, &len, ",\"y\":%d", a->y)) return -1;
            }
            if (a->width_set) {
                if (!appendf(out, out_cap, &len, ",\"width\":%d", a->width)) return -1;
            }
            if (a->height_set) {
                if (!appendf(out, out_cap, &len, ",\"height\":%d", a->height)) return -1;
            }
            if (a->min_set) {
                if (!appendf(out, out_cap, &len, ",\"min\":%.3f", a->minv)) return -1;
            }
            if (a->max_set) {
                if (!appendf(out, out_cap, &len, ",\"max\":%.3f", a->maxv)) return -1;
            }
            if (a->bar_color_set) {
                if (!appendf(out, out_cap, &len, ",\"bar_color\":%ld", a->bar_color)) return -1;
            }
            if (a->text_color_set) {
                if (!appendf(out, out_cap, &len, ",\"text_color\":%ld", a->text_color)) return -1;
            }
            if (a->background_set) {
                if (!appendf(out, out_cap, &len, ",\"background\":%d", a->background)) return -1;
            }
            if (a->background_opacity_set) {
                if (!appendf(out, out_cap, &len, ",\"background_opacity\":%d", a->background_opacity)) return -1;
            }
            if (a->segments_set) {
                if (!appendf(out, out_cap, &len, ",\"segments\":%d", a->segments)) return -1;
            }
            if (a->rounded_outline_set) {
                if (!appendf(out, out_cap, &len, ",\"rounded_outline\":%s", a->rounded_outline ? "true" : "false")) return -1;
            }

            if (!appendf(out, out_cap, &len, "}")) return -1;
        }

        if (!appendf(out, out_cap, &len, "]")) return -1;
    }

    /* timestamp */
    if (pb->timestamp_set) {
        if (!appendf(out, out_cap, &len, "%s\"timestamp_ms\":%ld", first_field ? "" : ",", pb->timestamp_ms))
            return -1;
        first_field = 0;
    }

    if (!appendf(out, out_cap, &len, "}")) return -1;

    if (len > MAX_PAYLOAD) {
        return -2; // size exceeded
    }

    return len;
}

/* ------------------------- parsing value/text flags ------------------------- */

static int parse_index_value_pair(const char *s, int *idx, char *val_buf, size_t val_sz)
{
    // expects: "<idx>=<value_str>"
    const char *eq = strchr(s, '=');
    if (!eq) return 0;
    char left[32];
    size_t ln = (size_t)(eq - s);
    if (ln == 0 || ln >= sizeof(left)) return 0;
    memcpy(left, s, ln);
    left[ln] = '\0';
    int i;
    if (!parse_int(trim(left), &i) || i < 0 || i > 7) return 0;

    const char *rv = eq + 1;
    rv = trim((char *)rv);

    if (val_buf && val_sz > 0) {
        strncpy(val_buf, rv, val_sz - 1);
        val_buf[val_sz - 1] = '\0';
    }

    *idx = i;
    return 1;
}

static int apply_values_list(PayloadBuilder *pb, const char *spec)
{
    // spec: "0=-52,1=-51,3=-60"
    if (!spec) return 0;
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok) continue;

        int idx;
        char vbuf[64];
        if (!parse_index_value_pair(tok, &idx, vbuf, sizeof(vbuf))) {
            fprintf(stderr, "Bad --values entry: %s\n", tok);
            return 0;
        }
        double dv;
        if (!parse_double(vbuf, &dv)) {
            fprintf(stderr, "Bad number in --values: %s\n", vbuf);
            return 0;
        }
        pb_set_value(pb, idx, dv);
    }
    return 1;
}

static int apply_texts_list(PayloadBuilder *pb, const char *spec)
{
    // spec: "0=SSID,1=Foo"
    if (!spec) return 0;
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok) continue;

        int idx;
        char vbuf[128];
        if (!parse_index_value_pair(tok, &idx, vbuf, sizeof(vbuf))) {
            fprintf(stderr, "Bad --texts entry: %s\n", tok);
            return 0;
        }
        pb_set_text(pb, idx, vbuf);
    }
    return 1;
}

/* ------------------------- send command ------------------------- */

static void usage_send(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s send --dest <ip> [--port <n>] [--value i=v ...] [--values list]\n"
        "         [--text i=s ...] [--texts list]\n"
        "         [--asset k=v,...] [--timestamp-ms <n>] [--print-json]\n"
        "\n"
        "  %s send --dest <ip> [--port <n>] [baseline flags...] --stdin\n"
        "\n"
        "Notes:\n"
        "  - Max payload: %d bytes (hard limit).\n"
        "  - texts are clamped to %d chars.\n"
        "  - --asset entries with same id are merged.\n"
        "  - text_indices value uses '|' ';' or ':' separators inside the value.\n"
        "\n"
        "Examples:\n"
        "  %s send --dest 192.168.2.20 --value 0=-52 --text 0=Trollvinter\n"
        "  %s send --dest 192.168.2.20 --asset id=0,enabled=false\n"
        "  %s send --dest 192.168.2.20 --asset id=0,x=50,y=50 --asset id=0,min=-80,max=-30\n"
        "  %s send --dest 192.168.2.20 --asset id=0,min=-80,max=-30 --stdin < updates.jsonl\n",
        prog, prog, MAX_PAYLOAD, MAX_TEXT_LEN,
        prog, prog, prog, prog);
}

static int cmd_send(int argc, char **argv, const char *prog, int alias_asset)
{
    const char *dest = NULL;
    int port = 7777;
    int use_stdin = 0;
    int print_json = 0;

    PayloadBuilder pb;
    pb_init(&pb);

    static struct option opts[] = {
        {"dest", required_argument, 0, 1},
        {"port", required_argument, 0, 2},
        {"value", required_argument, 0, 3},
        {"values", required_argument, 0, 4},
        {"text", required_argument, 0, 5},
        {"texts", required_argument, 0, 6},
        {"asset", required_argument, 0, 7},
        {"timestamp-ms", required_argument, 0, 8},
        {"stdin", no_argument, 0, 9},
        {"print-json", no_argument, 0, 10},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    optind = 1;
    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "h", opts, &idx)) != -1) {
        switch (opt) {
        case 1: dest = optarg; break;
        case 2: port = atoi(optarg); break;
        case 3: {
            int i;
            char vbuf[64];
            if (!parse_index_value_pair(optarg, &i, vbuf, sizeof(vbuf))) {
                fprintf(stderr, "Bad --value %s\n", optarg);
                return 1;
            }
            double dv;
            if (!parse_double(vbuf, &dv)) {
                fprintf(stderr, "Bad number in --value: %s\n", vbuf);
                return 1;
            }
            pb_set_value(&pb, i, dv);
            break;
        }
        case 4:
            if (!apply_values_list(&pb, optarg)) return 1;
            break;
        case 5: {
            int i;
            char sbuf[128];
            if (!parse_index_value_pair(optarg, &i, sbuf, sizeof(sbuf))) {
                fprintf(stderr, "Bad --text %s\n", optarg);
                return 1;
            }
            pb_set_text(&pb, i, sbuf);
            break;
        }
        case 6:
            if (!apply_texts_list(&pb, optarg)) return 1;
            break;
        case 7:
            if (!pb_add_asset_spec(&pb, optarg)) return 1;
            break;
        case 8: {
            long ts;
            if (!parse_long(optarg, &ts)) {
                fprintf(stderr, "Bad --timestamp-ms %s\n", optarg);
                return 1;
            }
            pb.timestamp_set = 1;
            pb.timestamp_ms = ts;
            break;
        }
        case 9:
            use_stdin = 1;
            break;
        case 10:
            print_json = 1;
            break;
        case 'h':
        default:
            usage_send(prog);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!dest) {
        fprintf(stderr, "Error: --dest is required.\n");
        usage_send(prog);
        return 1;
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid --port.\n");
        return 1;
    }

    if (print_json && use_stdin) {
        fprintf(stderr, "Error: --print-json cannot be used with --stdin.\n");
        return 1;
    }

    /* In send/asset we expect no positional args. */
    if (!alias_asset) {
        if (optind < argc) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
            usage_send(prog);
            return 1;
        }
    } else {
        /* asset alias also should not take extra positionals */
        if (optind < argc) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
            usage_send(prog);
            return 1;
        }
    }

    char out[BUILD_BUF];
    int len = serialize_payload(&pb, out, sizeof(out));

    int baseline_has_any = any_values(&pb) || any_texts(&pb) || any_assets(&pb) || pb.timestamp_set;

    if (!use_stdin) {
        if (len == -2) {
            fprintf(stderr, "Error: payload exceeds %d bytes.\n", MAX_PAYLOAD);
            return 1;
        }
        if (len < 0) {
            fprintf(stderr, "Error: failed to build payload.\n");
            return 1;
        }

        if (print_json) {
            printf("%s\n", out);
            return 0;
        }

        int sock = open_udp_socket();
        if (sock < 0) { perror("socket"); return 1; }
        int rc = send_udp(sock, dest, port, out, (size_t)len);
        if (rc < 0) { perror("sendto"); close(sock); return 1; }
        close(sock);
        return 0;
    }

    /* --stdin mode (JSON Lines only) */
    int sock = open_udp_socket();
    if (sock < 0) { perror("socket"); return 1; }

    /* Send baseline once if flags produced anything. */
    if (baseline_has_any) {
        if (len == -2) {
            fprintf(stderr, "Error: baseline payload exceeds %d bytes.\n", MAX_PAYLOAD);
            close(sock);
            return 1;
        }
        if (len < 0) {
            fprintf(stderr, "Error: failed to build baseline payload.\n");
            close(sock);
            return 1;
        }
        if (send_udp(sock, dest, port, out, (size_t)len) < 0) {
            perror("sendto(baseline)");
            close(sock);
            return 1;
        }
    }

    char line[4096];
    long line_no = 0;

    while (fgets(line, sizeof(line), stdin)) {
        line_no++;

        char *t = trim(line);
        if (!*t) continue;

        if (!looks_like_json_object(t)) {
            fprintf(stderr, "stdin line %ld: not a JSON object\n", line_no);
            continue;
        }

        size_t l = strlen(t);
        if (l > MAX_PAYLOAD) {
            fprintf(stderr, "stdin line %ld: payload len=%zu exceeds %d bytes, skipped\n",
                    line_no, l, MAX_PAYLOAD);
            continue;
        }

        if (send_udp(sock, dest, port, t, l) < 0) {
            fprintf(stderr, "stdin line %ld: sendto failed: %s\n", line_no, strerror(errno));
            continue;
        }
    }

    close(sock);
    return 0;
}

/* ------------------------- watch command ------------------------- */

static void usage_watch(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s watch --dest <ip> [--port <n>] [--interval <ms>]\n"
        "          --value-index <i>... [--text-index <i>...] <iface>\n"
        "\n"
        "Notes:\n"
        "  - Only the specified indices are populated.\n"
        "  - bar color updates are sent for asset id == value-index.\n"
        "  - Max payload: %d bytes.\n"
        "\n"
        "Example:\n"
        "  %s watch --dest 192.168.2.20 --interval 16 \\\n"
        "      --value-index 0 --value-index 1 --value-index 2 --value-index 3 \\\n"
        "      --text-index 0 wlx40a5ef2f2308\n",
        prog, MAX_PAYLOAD, prog);
}

static int cmd_watch(int argc, char **argv, const char *prog)
{
    const char *dest = NULL;
    int port = 7777;
    int interval_ms = 100;

    int value_idx_set[8] = {0};
    int text_idx_set[8] = {0};

    static struct option opts[] = {
        {"dest", required_argument, 0, 1},
        {"port", required_argument, 0, 2},
        {"interval", required_argument, 0, 3},
        {"value-index", required_argument, 0, 4},
        {"text-index", required_argument, 0, 5},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    optind = 1;
    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "h", opts, &idx)) != -1) {
        switch (opt) {
        case 1: dest = optarg; break;
        case 2: port = atoi(optarg); break;
        case 3: interval_ms = atoi(optarg); break;
        case 4: {
            int i;
            if (!parse_int(optarg, &i) || i < 0 || i > 7) {
                fprintf(stderr, "Bad --value-index %s\n", optarg);
                return 1;
            }
            value_idx_set[i] = 1;
            break;
        }
        case 5: {
            int i;
            if (!parse_int(optarg, &i) || i < 0 || i > 7) {
                fprintf(stderr, "Bad --text-index %s\n", optarg);
                return 1;
            }
            text_idx_set[i] = 1;
            break;
        }
        case 'h':
        default:
            usage_watch(prog);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!dest) {
        fprintf(stderr, "Error: --dest is required.\n");
        usage_watch(prog);
        return 1;
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid --port.\n");
        return 1;
    }
    if (interval_ms < 5) interval_ms = 5;

    if (optind >= argc) {
        fprintf(stderr, "Error: missing <iface>.\n");
        usage_watch(prog);
        return 1;
    }
    const char *iface = argv[optind];

    int any_v = 0, any_t = 0;
    for (int i = 0; i < 8; i++) {
        if (value_idx_set[i]) any_v = 1;
        if (text_idx_set[i]) any_t = 1;
    }
    if (!any_v && !any_t) {
        fprintf(stderr, "Error: specify at least one --value-index or --text-index.\n");
        return 1;
    }

    int sock = open_udp_socket();
    if (sock < 0) { perror("socket"); return 1; }

    uint32_t last_color[8] = {0};
    int last_color_set[8] = {0};

    long last_debug = -1;

        /* One-time asset init push for bar id 0 (per default template)
     * We omit label because contract defines label as string, while the
     * provided template showed label:-1.
     */
    {
        PayloadBuilder initpb;
        pb_init(&initpb);

        const char *init_spec =
            "id=0,"
            "type=bar,"
            "enabled=true,"
            "value_index=0,"
            "text_index=0,"
            "x=690,"
            "y=10,"
            "width=400,"
            "height=12,"
            "min=-100,"
            "max=-40,"
            "orientation=left,"
            "background=2,"
            "background_opacity=30,"
            "bar_color=2254540,"
            "text_color=0";

        if (!pb_add_asset_spec(&initpb, init_spec)) {
            fprintf(stderr, "[watch] failed to build initial asset spec\n");
        } else {
            char init_json[BUILD_BUF];
            int init_len = serialize_payload(&initpb, init_json, sizeof(init_json));
            if (init_len == -2) {
                fprintf(stderr, "[watch] initial asset payload exceeds %d bytes\n", MAX_PAYLOAD);
            } else if (init_len < 0) {
                fprintf(stderr, "[watch] failed to serialize initial asset payload\n");
            } else {
                if (send_udp(sock, dest, port, init_json, (size_t)init_len) < 0) {
                    perror("sendto(init asset)");
                } else {
                    fprintf(stderr, "[watch] sent initial asset config for id=0\n");
                }
            }
        }
    }




    while (1) {
        char ssid_raw[128];
        double signal_dbm = 0.0;

        int ok = read_iw_link(iface, ssid_raw, sizeof(ssid_raw), &signal_dbm);

        char ssid16[MAX_TEXT_LEN + 1];
        if (ok) clamp_text16(ssid_raw, ssid16);
        else {
            strncpy(ssid16, "DISCONNECTED", sizeof(ssid16));
            ssid16[MAX_TEXT_LEN] = '\0';
            signal_dbm = 0.0;
        }

        PayloadBuilder pb;
        pb_init(&pb);

        /* Populate only requested indices */
        for (int i = 0; i < 8; i++) {
            if (value_idx_set[i] && ok) pb_set_value(&pb, i, signal_dbm);
            else if (value_idx_set[i] && !ok) {
                // leave absent rather than forcing zeros
                // (keeps payload smaller and semantics clear)
            }

            if (text_idx_set[i]) pb_set_text(&pb, i, ssid16);
        }

        /* Color updates for assets matching value indices */
        uint32_t c = ok ? color_for_signal_dbm(signal_dbm) : 0xFF0000;

        for (int i = 0; i < 8; i++) {
            if (!value_idx_set[i]) continue;

            int changed = !last_color_set[i] || last_color[i] != c;
            if (!changed) continue;

            char spec[64];
            snprintf(spec, sizeof(spec), "id=%d,bar_color=%u", i, (unsigned)c);
            if (!pb_add_asset_spec(&pb, spec)) {
                // if asset list full or parse error, ignore for now
            }

            last_color[i] = c;
            last_color_set[i] = 1;
        }

        char out[BUILD_BUF];
        int len = serialize_payload(&pb, out, sizeof(out));
        if (len == -2) {
            fprintf(stderr, "[watch] payload exceeds %d bytes, skipping frame\n", MAX_PAYLOAD);
        } else if (len < 0) {
            fprintf(stderr, "[watch] failed to build payload\n");
        } else {
            if (send_udp(sock, dest, port, out, (size_t)len) < 0) {
                perror("sendto");
                break;
            }
        }

        long now = monotonic_sec();
        if (now != last_debug) {
            last_debug = now;
            fprintf(stderr,
                    "[watch] dst=%s:%d iface=%s ok=%d ssid=\"%s\" signal=%.1f dBm color=0x%06X len=%d\n",
                    dest, port, iface, ok, ssid16, signal_dbm, (unsigned)c,
                    (len > 0) ? len : 0);
        }

        usleep((useconds_t)interval_ms * 1000);
    }

    close(sock);
    return 0;
}

/* ------------------------- top-level ------------------------- */

static void usage_main(const char *prog)
{
    fprintf(stderr,
        "waybeam - UDP OSD control tool\n"
        "\n"
        "Usage:\n"
        "  %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  send     Build and send a single payload, or send JSON Lines with --stdin\n"
        "  watch    Stream RSSI/SSID from iw to selected value/text indices\n"
        "  asset    Alias of send (asset-focused usage)\n"
        "\n"
        "Global notes:\n"
        "  - --dest is required for send/watch/asset.\n"
        "  - Default --port is 7777.\n"
        "  - Max payload size is %d bytes (hard limit).\n"
        "\n"
        "Run:\n"
        "  %s send --help\n"
        "  %s watch --help\n",
        prog, MAX_PAYLOAD, prog, prog);
}

static void die_usage(const char *prog)
{
    usage_main(prog);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];

    if (argc < 2) {
        usage_main(prog);
        return 1;
    }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "send")) {
        return cmd_send(argc - 1, argv + 1, prog, 0);
    } else if (!strcmp(cmd, "asset")) {
        return cmd_send(argc - 1, argv + 1, prog, 1);
    } else if (!strcmp(cmd, "watch")) {
        return cmd_watch(argc - 1, argv + 1, prog);
    } else if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage_main(prog);
        return 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage_main(prog);
        return 1;
    }
}
