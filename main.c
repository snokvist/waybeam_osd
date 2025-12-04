#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "mi_sys.h"
#include "mi_rgn.h"
#include "mi_vpe.h"

#define DEFAULT_SCREEN_WIDTH 1280   // fallback resolution if config is absent
#define DEFAULT_SCREEN_HEIGHT 720
#define BUF_ROWS 60  // partial buffer height
#define CONFIG_PATH "config.json"
#define UDP_PORT 7777
#define UDP_MAX_PACKET 512

// LVGL buffers - allocated at runtime for ARGB8888 (32-bit per pixel)
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

typedef struct {
    int width;
    int height;
    int show_stats;
    int idle_ms;
    int udp_stats;
} app_config_t;

typedef enum {
    ASSET_BAR = 0,
    ASSET_BAR2,
    ASSET_LOTTIE,
} asset_type_t;

typedef struct {
    asset_type_t type;
    int value_index;
    int x;
    int y;
    int width;
    int height;
    float min;
    float max;
    uint32_t color;
    char label[64];
    int text_index;
    char file[256];
} asset_cfg_t;

typedef struct {
    asset_cfg_t cfg;
    lv_obj_t *obj;
    lv_obj_t *label_obj;
    int last_pct;
    char last_label_text[64];
} asset_t;

static app_config_t g_cfg;
static int osd_width = DEFAULT_SCREEN_WIDTH;
static int osd_height = DEFAULT_SCREEN_HEIGHT;
static asset_t assets[8];
static int asset_count = 0;

// Sigmastar RGN
static MI_RGN_PaletteTable_t g_stPaletteTable = {};
static MI_RGN_HANDLE hRgnHandle;
static MI_RGN_ChnPort_t stVpeChnPort;
static MI_RGN_Attr_t stRgnAttr;
static MI_RGN_ChnPortParam_t stRgnChnAttr;

// UI
static lv_obj_t *stats_label = NULL;
static uint32_t last_frame_ms = 0;
static uint32_t last_loop_ms = 0;
static uint32_t fps_value = 0;
static uint64_t fps_start_ms = 0;
static uint32_t fps_frames = 0;
static int idle_ms_applied = 100;
static volatile sig_atomic_t stop_requested = 0;
static lv_timer_t *stats_timer = NULL;
static int udp_sock = -1;
static double udp_values[8] = {0};
static char udp_texts[8][17] = {{0}};
static const char *g_embedded_lottie_json =
    "{\"v\":\"5.5.7\",\"fr\":30,\"ip\":0,\"op\":60,\"w\":200,\"h\":200,"
    "\"nm\":\"circle\",\"ddd\":0,\"assets\":[],\"layers\":[{\"ddd\":0,\"ind\":1,\"ty\":4,"
    "\"nm\":\"shape\",\"sr\":1,\"ks\":{\"o\":{\"a\":0,\"k\":100},\"r\":{\"a\":0,\"k\":0},"
    "\"p\":{\"a\":0,\"k\":[100,100,0]},\"a\":{\"a\":0,\"k\":[0,0,0]},\"s\":{\"a\":0,\"k\":[100,100,100]}},"
    "\"shapes\":[{\"ty\":\"el\",\"p\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[120,120]},\"nm\":\"ellipse\"},"
    "{\"ty\":\"fl\",\"c\":{\"a\":0,\"k\":[0.1,0.6,0.9,1]},\"o\":{\"a\":0,\"k\":100},\"nm\":\"fill\"}],"
    "\"ip\":0,\"op\":60,\"st\":0,\"bm\":0}]}";

// -------------------------
// Utility helpers
// -------------------------
static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float clamp_float(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int read_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t r = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[r] = '\0';
    *out_buf = buf;
    if (out_len) *out_len = r;
    return 0;
}

static const char *find_key(const char *json, const char *key)
{
    static char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static const char *find_key_range(const char *start, const char *end, const char *key)
{
    static char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
#if defined(_GNU_SOURCE)
    return memmem(start, (size_t)(end - start), pattern, strlen(pattern));
#else
    const char *p = start;
    size_t plen = strlen(pattern);
    while (p && p + plen <= end) {
        const char *hit = strstr(p, pattern);
        if (!hit || hit >= end) return NULL;
        return hit;
    }
    return NULL;
#endif
}

static int json_get_int(const char *json, const char *key, int *out)
{
    const char *p = find_key(json, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return -1;
    *out = (int)strtol(p, NULL, 0);
    return 0;
}

static int json_get_float(const char *json, const char *key, float *out)
{
    const char *p = find_key(json, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return -1;
    *out = (float)strtod(p, NULL);
    return 0;
}

static int json_get_bool(const char *json, const char *key, int *out)
{
    const char *p = find_key(json, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!strncmp(p, "true", 4)) {
        *out = 1;
        return 0;
    }
    if (!strncmp(p, "false", 5)) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int json_get_int_range(const char *start, const char *end, const char *key, int *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p || p >= end) return -1;
    p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return -1;
    *out = (int)strtol(p, NULL, 0);
    return 0;
}

static int json_get_float_range(const char *start, const char *end, const char *key, float *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p || p >= end) return -1;
    p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return -1;
    *out = (float)strtod(p, NULL);
    return 0;
}

static int json_get_string_range(const char *start, const char *end, const char *key, char *buf, size_t buf_sz)
{
    const char *p = find_key_range(start, end, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p || p >= end) return -1;
    p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '\"') return -1;
    p++; // skip opening quote
    const char *q = p;
    while (q < end && *q && *q != '\"') q++;
    size_t len = (size_t)(q - p);
    if (q >= end || len + 1 > buf_sz) return -1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return 0;
}

static void set_defaults(void)
{
    g_cfg.width = DEFAULT_SCREEN_WIDTH;
    g_cfg.height = DEFAULT_SCREEN_HEIGHT;
    g_cfg.show_stats = 1;
    g_cfg.idle_ms = 100;
    g_cfg.udp_stats = 0;

    asset_count = 1;
    memset(assets, 0, sizeof(assets));
    assets[0].cfg.type = ASSET_BAR;
    assets[0].cfg.value_index = 0;
    assets[0].cfg.text_index = -1;
    assets[0].cfg.x = 40;
    assets[0].cfg.y = 200;
    assets[0].cfg.width = 320;
    assets[0].cfg.height = 32;
    assets[0].cfg.min = 0.0f;
    assets[0].cfg.max = 1.0f;
    assets[0].cfg.color = 0x2266CC;
    assets[0].last_pct = -1;
    assets[0].last_label_text[0] = '\0';
}

static void parse_assets_array(const char *json)
{
    const char *p = strstr(json, "\"assets\"");
    if (!p) return;
    const char *arr = strchr(p, '[');
    if (!arr) return;
    p = arr + 1;
    asset_count = 0;

    while (*p && asset_count < 8) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        const char *obj_start = p;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        const char *obj_end = p;
        if (depth != 0) break;

        asset_t a = {0};
        a.cfg.type = ASSET_BAR;
        a.cfg.value_index = asset_count;
        a.cfg.x = 40;
        a.cfg.y = 60 + asset_count * 60;
        a.cfg.width = 320;
        a.cfg.height = 32;
        a.cfg.min = 0.0f;
        a.cfg.max = 1.0f;
        a.cfg.color = 0x2266CC;
        a.cfg.text_index = -1;
        a.cfg.label[0] = '\0';

        char type_buf[32];
        if (json_get_string_range(obj_start, obj_end, "type", type_buf, sizeof(type_buf)) == 0) {
            if (strcmp(type_buf, "example_bar_2") == 0 || strcmp(type_buf, "example_bar2") == 0) {
                a.cfg.type = ASSET_BAR2;
            } else if (strcmp(type_buf, "lottie") == 0) {
                a.cfg.type = ASSET_LOTTIE;
            } else {
                a.cfg.type = ASSET_BAR;
            }
        }

        int v = 0;
        float fv = 0.0f;
        if (json_get_int_range(obj_start, obj_end, "value_index", &v) == 0) a.cfg.value_index = clamp_int(v, 0, 7);
        if (json_get_int_range(obj_start, obj_end, "x", &v) == 0) a.cfg.x = v;
        if (json_get_int_range(obj_start, obj_end, "y", &v) == 0) a.cfg.y = v;
        if (json_get_int_range(obj_start, obj_end, "width", &v) == 0) a.cfg.width = v;
        if (json_get_int_range(obj_start, obj_end, "height", &v) == 0) a.cfg.height = v;
        if (json_get_float_range(obj_start, obj_end, "min", &fv) == 0) a.cfg.min = fv;
        if (json_get_float_range(obj_start, obj_end, "max", &fv) == 0) a.cfg.max = fv;
        if (json_get_int_range(obj_start, obj_end, "color", &v) == 0) a.cfg.color = (uint32_t)v;
        if (json_get_int_range(obj_start, obj_end, "text_index", &v) == 0) a.cfg.text_index = clamp_int(v, -1, 7);
        json_get_string_range(obj_start, obj_end, "label", a.cfg.label, sizeof(a.cfg.label));
        json_get_string_range(obj_start, obj_end, "file", a.cfg.file, sizeof(a.cfg.file));

        a.last_pct = -1;
        a.last_label_text[0] = '\0';

        assets[asset_count++] = a;
    }

    if (asset_count == 0) {
        memset(assets, 0, sizeof(assets));
        asset_count = 1;
        assets[0].cfg.type = ASSET_BAR;
        assets[0].cfg.value_index = 0;
        assets[0].cfg.text_index = -1;
        assets[0].cfg.x = 40;
        assets[0].cfg.y = 200;
        assets[0].cfg.width = 320;
        assets[0].cfg.height = 32;
        assets[0].cfg.min = 0.0f;
        assets[0].cfg.max = 1.0f;
        assets[0].cfg.color = 0x2266CC;
        assets[0].last_pct = -1;
        assets[0].last_label_text[0] = '\0';
    }
}

static void load_config(void)
{
    set_defaults();

    char *json = NULL;
    if (read_file(CONFIG_PATH, &json, NULL) != 0) {
        return;
    }

    int v = 0;
    float fv = 0.0f;
    if (json_get_int(json, "width", &v) == 0) g_cfg.width = v;
    if (json_get_int(json, "height", &v) == 0) g_cfg.height = v;
    if (json_get_bool(json, "show_stats", &v) == 0) g_cfg.show_stats = v;
    if (json_get_bool(json, "udp_stats", &v) == 0) g_cfg.udp_stats = v;
    if (json_get_int(json, "idle_ms", &v) == 0) {
        g_cfg.idle_ms = clamp_int(v, 10, 1000);
    } else if (json_get_int(json, "refresh_ms", &v) == 0) {
        // Backward compatibility with older configs
        g_cfg.idle_ms = clamp_int(v, 10, 1000);
    }

    // Backwards-compatible single bar fields (used only if no assets array)
    if (json_get_int(json, "bar_x", &v) == 0) assets[0].cfg.x = v;
    if (json_get_int(json, "bar_y", &v) == 0) assets[0].cfg.y = v;
    if (json_get_int(json, "bar_width", &v) == 0) assets[0].cfg.width = v;
    if (json_get_int(json, "bar_height", &v) == 0) assets[0].cfg.height = v;
    if (json_get_float(json, "bar_min", &fv) == 0) assets[0].cfg.min = fv;
    if (json_get_float(json, "bar_max", &fv) == 0) assets[0].cfg.max = fv;
    if (json_get_int(json, "bar_color", &v) == 0) assets[0].cfg.color = (uint32_t)v;

    // Preferred structured assets list
    parse_assets_array(json);

    free(json);
}

static int setup_udp_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static void parse_udp_values(const char *buf)
{
    const char *p = strstr(buf, "\"values\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    for (int i = 0; i < 8; i++) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *endptr = NULL;
        double val = strtod(p, &endptr);
        if (p == endptr) break;
        udp_values[i] = val;
        p = endptr;
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
}

static void parse_udp_texts(const char *buf)
{
    const char *p = strstr(buf, "\"texts\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    for (int i = 0; i < 8; i++) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != '\"') break;
        p++; // skip quote
        const char *start = p;
        while (*p && *p != '\"') p++;
        size_t len = (size_t)(p - start);
        if (len > 16) len = 16;
        memcpy(udp_texts[i], start, len);
        udp_texts[i][len] = '\0';
        if (*p != '\"') break;
        p++; // skip closing quote
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
}

static const char *get_asset_text(const asset_t *asset)
{
    if (asset->cfg.text_index >= 0 && asset->cfg.text_index < 8) {
        const char *t = udp_texts[asset->cfg.text_index];
        if (t[0] != '\0') return t;
        if (asset->cfg.label[0] != '\0') return asset->cfg.label;
        return "";
    }
    if (asset->cfg.label[0] != '\0') return asset->cfg.label;
    return "";
}

static bool poll_udp(void)
{
    if (udp_sock < 0) return false;
    char buf[UDP_MAX_PACKET];
    ssize_t r = 0;
    ssize_t last_r = -1;
    // Drain the socket to keep only the freshest payload
    while ((r = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, NULL, NULL)) > 0) {
        last_r = r;
    }
    if (last_r > 0) {
        buf[last_r] = '\0';
        parse_udp_values(buf);
        parse_udp_texts(buf);
        return true;
    }
    return false;
}

// -------------------------
// LVGL tick function
// -------------------------
uint32_t my_get_milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(((uint64_t)ts.tv_sec * 1000ULL) + (uint64_t)(ts.tv_nsec / 1000000ULL));
}

static uint64_t monotonic_ms64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

// -------------------------
// LVGL flush callback
// -------------------------
void my_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    MI_RGN_CanvasInfo_t info;
    MI_RGN_GetCanvasInfo(hRgnHandle, &info);

    if (!info.virtAddr) {
        lv_display_flush_ready(disp);
        return;
    }

    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    uint32_t *src = (uint32_t *)px_map;  // Source is ARGB8888 (32-bit)

    for (int y = 0; y < h; y++) {
        uint16_t *dest = (uint16_t *)(info.virtAddr +
                                      (area->y1 + y) * info.u32Stride +
                                      area->x1 * 2);  // Dest is ARGB4444 (16-bit)

        for (int x = 0; x < w; x++) {
            uint32_t argb8888 = src[y * w + x];
            
            // Extract ARGB8888 components (8 bits each)
            uint8_t a8 = (argb8888 >> 24) & 0xFF;  // Alpha
            uint8_t r8 = (argb8888 >> 16) & 0xFF;  // Red
            uint8_t g8 = (argb8888 >> 8) & 0xFF;   // Green
            uint8_t b8 = argb8888 & 0xFF;          // Blue
            
            // Convert 8-bit to 4-bit by taking the upper 4 bits
            // This is equivalent to: value >> 4
            uint8_t a4 = (a8 >> 4) & 0x0F;  // 8-bit -> 4-bit alpha
            uint8_t r4 = (r8 >> 4) & 0x0F;  // 8-bit -> 4-bit red
            uint8_t g4 = (g8 >> 4) & 0x0F;  // 8-bit -> 4-bit green
            uint8_t b4 = (b8 >> 4) & 0x0F;  // 8-bit -> 4-bit blue
            
            // Pack into ARGB4444 format: AAAA RRRR GGGG BBBB
            uint16_t argb4444 = (a4 << 12) | (r4 << 8) | (g4 << 4) | b4;
            
            dest[x] = argb4444;
        }
    }

    MI_RGN_UpdateCanvas(hRgnHandle);
    lv_display_flush_ready(disp);
}

// -------------------------
// Initialize RGN
// -------------------------
void mi_region_init(void)
{
    MI_RGN_Init(&g_stPaletteTable);
    hRgnHandle = 0;

    memset(&stRgnAttr, 0, sizeof(MI_RGN_Attr_t));
    stRgnAttr.eType = E_MI_RGN_TYPE_OSD;
    stRgnAttr.stOsdInitParam.ePixelFmt = E_MI_RGN_PIXEL_FORMAT_ARGB4444;  // Changed to ARGB4444
    stRgnAttr.stOsdInitParam.stSize.u32Width = osd_width;
    stRgnAttr.stOsdInitParam.stSize.u32Height = osd_height;

    MI_RGN_Create(hRgnHandle, &stRgnAttr);

    stVpeChnPort.eModId = E_MI_RGN_MODID_VPE;
    stVpeChnPort.s32DevId = 0;
    stVpeChnPort.s32ChnId = 0;
    stVpeChnPort.s32OutputPortId = 0;

    memset(&stRgnChnAttr, 0, sizeof(MI_RGN_ChnPortParam_t));
    stRgnChnAttr.bShow = 1;
    stRgnChnAttr.stPoint.u32X = 0;
    stRgnChnAttr.stPoint.u32Y = 0;
    stRgnChnAttr.unPara.stOsdChnPort.u32Layer = 0;
    stRgnChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode = E_MI_RGN_PIXEL_ALPHA;

    MI_RGN_AttachToChn(hRgnHandle, &stVpeChnPort, &stRgnChnAttr);
}

// -------------------------
// Initialize LVGL display
// -------------------------
void init_lvgl(void)
{
    lv_init();

    // Set LVGL tick callback
    lv_tick_set_cb(my_get_milliseconds);

    size_t buf_size = (size_t)osd_width * BUF_ROWS * sizeof(lv_color_t);
    buf1 = (lv_color_t *)malloc(buf_size);
    buf2 = (lv_color_t *)malloc(buf_size);

    lv_display_t * disp = lv_display_create(osd_width, osd_height);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_flush_cb);
}


static lv_obj_t *create_simple_bar(const asset_cfg_t *cfg)
{
    lv_obj_t *bar = lv_bar_create(lv_scr_act());
    lv_obj_set_size(bar, cfg->width, cfg->height);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, cfg->x, cfg->y);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(cfg->color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    return bar;
}

static lv_obj_t *create_example_bar2(const asset_cfg_t *cfg)
{
    static lv_style_t style_bg;
    static lv_style_t style_indic;
    static int styles_init = 0;

    if (!styles_init) {
        lv_style_init(&style_bg);
        lv_style_set_border_color(&style_bg, lv_palette_main(LV_PALETTE_BLUE));
        lv_style_set_border_width(&style_bg, 2);
        lv_style_set_pad_all(&style_bg, 6);
        lv_style_set_radius(&style_bg, 6);
        lv_style_set_anim_duration(&style_bg, 1000);

        lv_style_init(&style_indic);
        lv_style_set_bg_opa(&style_indic, LV_OPA_COVER);
        lv_style_set_bg_color(&style_indic, lv_palette_main(LV_PALETTE_BLUE));
        lv_style_set_radius(&style_indic, 3);
        styles_init = 1;
    }

    lv_obj_t *bar = lv_bar_create(lv_scr_act());
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &style_bg, 0);
    lv_obj_add_style(bar, &style_indic, LV_PART_INDICATOR);

    lv_obj_set_size(bar, cfg->width > 0 ? cfg->width : 200, cfg->height > 0 ? cfg->height : 20);
    lv_obj_set_pos(bar, cfg->x, cfg->y);
    lv_bar_set_range(bar, 0, 100);
    return bar;
}

static lv_color_t lottie_color_from_json(const char *json)
{
    uint32_t hash = 5381u;
    for (const unsigned char *p = (const unsigned char *)json; *p; p++) {
        hash = ((hash << 5) + hash) + *p; // djb2
    }
    uint8_t r = (hash >> 0) & 0xFF;
    uint8_t g = (hash >> 8) & 0xFF;
    uint8_t b = (hash >> 16) & 0xFF;
    return lv_color_make(r, g, b);
}

typedef struct {
    lv_obj_t *arc;
    int16_t span;
} spinner_anim_t;

static void lottie_spinner_anim_cb(void *var, int32_t v)
{
    spinner_anim_t *ctx = (spinner_anim_t *)var;
    if (!ctx || !ctx->arc) return;

    int16_t start = (int16_t)(v % 360);
    int16_t end = start + ctx->span;
    if (end > 360) end -= 360;

    lv_arc_set_angles(ctx->arc, start, end);
}

static void lottie_spinner_delete_cb(lv_event_t *e)
{
    spinner_anim_t *ctx = (spinner_anim_t *)lv_event_get_user_data(e);
    if (ctx) {
        lv_anim_del(ctx, lottie_spinner_anim_cb);
        free(ctx);
    }
}

static lv_obj_t *create_lottie_asset(const asset_cfg_t *cfg)
{
    const char *json_source = g_embedded_lottie_json;
    char *json_from_file = NULL;
    if (cfg->file[0]) {
        if (read_file(cfg->file, &json_from_file, NULL) == 0) {
            json_source = json_from_file;
        } else {
            printf("Lottie file not accessible: %s, using embedded sample.\n", cfg->file);
        }
    }

    lv_color_t accent = lottie_color_from_json(json_source);
    free(json_from_file);

    lv_obj_t *spinner = lv_arc_create(lv_scr_act());
    if (!spinner) return NULL;

    lv_obj_remove_style_all(spinner);
    lv_arc_set_bg_angles(spinner, 0, 360);
    lv_arc_set_angles(spinner, 0, 90);

    int w = cfg->width > 0 ? cfg->width : 140;
    int h = cfg->height > 0 ? cfg->height : 140;
    lv_obj_set_size(spinner, w, h);
    lv_obj_set_pos(spinner, cfg->x, cfg->y);

    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(spinner, lv_color_darken(accent, LV_OPA_60), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(spinner, accent, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    spinner_anim_t *ctx = malloc(sizeof(spinner_anim_t));
    if (!ctx) return spinner;
    ctx->arc = spinner;
    ctx->span = 120;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_values(&a, 0, 359);
    lv_anim_set_time(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, lottie_spinner_anim_cb);
    lv_anim_start(&a);

    lv_obj_add_event_cb(spinner, lottie_spinner_delete_cb, LV_EVENT_DELETE, ctx);

    return spinner;
}

static void maybe_attach_asset_label(asset_t *asset)
{
    if (!asset->obj) return;
    if (asset->cfg.label[0] == '\0' && asset->cfg.text_index < 0) return;
    asset->label_obj = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(asset->label_obj, lv_color_white(), 0);
    lv_obj_set_style_text_opa(asset->label_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(asset->label_obj, LV_OPA_TRANSP, 0);
    lv_label_set_text(asset->label_obj, get_asset_text(asset));
    lv_obj_align_to(asset->label_obj, asset->obj, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

static void create_assets(void)
{
    for (int i = 0; i < asset_count; i++) {
        switch (assets[i].cfg.type) {
            case ASSET_BAR:
                assets[i].obj = create_simple_bar(&assets[i].cfg);
                maybe_attach_asset_label(&assets[i]);
                break;
            case ASSET_BAR2:
                assets[i].obj = create_example_bar2(&assets[i].cfg);
                maybe_attach_asset_label(&assets[i]);
                break;
            case ASSET_LOTTIE:
                assets[i].obj = create_lottie_asset(&assets[i].cfg);
                maybe_attach_asset_label(&assets[i]);
                break;
            default:
                assets[i].obj = create_simple_bar(&assets[i].cfg);
                maybe_attach_asset_label(&assets[i]);
                break;
        }
    }
}

static void update_assets_from_udp(void)
{
    for (int i = 0; i < asset_count; i++) {
        const asset_cfg_t *cfg = &assets[i].cfg;
        float min = cfg->min;
        float max = cfg->max;
        if (max <= min + 0.0001f) {
            max = min + 1.0f;
        }
        float v = (float)udp_values[clamp_int(cfg->value_index, 0, 7)];
        v = clamp_float(v, min, max);
        float pct_f = (v - min) / (max - min);
        int pct = clamp_int((int)(pct_f * 100.0f), 0, 100);

        switch (cfg->type) {
            case ASSET_BAR:
            case ASSET_BAR2:
                if (assets[i].obj && assets[i].last_pct != pct) {
                    lv_bar_set_value(assets[i].obj, pct, LV_ANIM_OFF);
                    assets[i].last_pct = pct;
                }
                break;
            case ASSET_LOTTIE:
                // Animated by LVGL internally; descriptor refresh handled below.
                break;
            default:
                break;
        }

        if (assets[i].label_obj) {
            const char *txt = get_asset_text(&assets[i]);
            if (strncmp(txt, assets[i].last_label_text, sizeof(assets[i].last_label_text) - 1) != 0) {
                lv_label_set_text(assets[i].label_obj, txt);
                strncpy(assets[i].last_label_text, txt, sizeof(assets[i].last_label_text) - 1);
                assets[i].last_label_text[sizeof(assets[i].last_label_text) - 1] = '\0';
            }
        }
    }
}

static void handle_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void cleanup_resources(void)
{
    if (stats_timer) {
        lv_timer_del(stats_timer);
        stats_timer = NULL;
    }

    // Tear down OSD region cleanly
    MI_RGN_DetachFromChn(hRgnHandle, &stVpeChnPort);
    MI_RGN_Destroy(hRgnHandle);

    if (udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
    }

    free(buf1);
    free(buf2);
}

static void stats_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint64_t now = monotonic_ms64();
    if (fps_start_ms == 0) fps_start_ms = now;
    uint64_t elapsed = now - fps_start_ms;
    if (elapsed > 0) {
        fps_value = (uint32_t)((fps_frames * 1000ULL) / elapsed);
        fps_frames = 0;
        fps_start_ms = now;
    }

    int primary_w = 0;
    int primary_h = 0;
    if (asset_count > 0) {
        lv_obj_t *p = assets[0].obj;
        if (p) {
            primary_w = lv_obj_get_width(p);
            primary_h = lv_obj_get_height(p);
        }
    }
    char buf[512];
    int disp_w = lv_disp_get_hor_res(NULL);
    int disp_h = lv_disp_get_ver_res(NULL);
    int off = 0;
    off += lv_snprintf(buf + off, sizeof(buf) - off,
                       "OSD %dx%d (disp %dx%d)\n"
                       "Assets %d | primary %d,%d\n"
                       "FPS %u | work %ums | loop %ums | idle %dms",
                       osd_width, osd_height,
                       disp_w, disp_h,
                       asset_count, primary_w, primary_h,
                       fps_value, last_frame_ms, last_loop_ms, idle_ms_applied);

    if (g_cfg.udp_stats && off < (int)sizeof(buf) - 32) {
        off += lv_snprintf(buf + off, sizeof(buf) - off, "\nUDP values:");
        for (int i = 0; i < 8 && off < (int)sizeof(buf) - 16; i++) {
            int whole = (int)udp_values[i];
            int frac = (int)((udp_values[i] - whole) * 100.0);
            if (frac < 0) frac = -frac;
            off += lv_snprintf(buf + off, sizeof(buf) - off, "\n v%d=%d.%02d", i, whole, frac);
        }
        off += lv_snprintf(buf + off, sizeof(buf) - off, "\nUDP texts:");
        for (int i = 0; i < 8 && off < (int)sizeof(buf) - 20; i++) {
            const char *t = udp_texts[i][0] ? udp_texts[i] : "-";
            off += lv_snprintf(buf + off, sizeof(buf) - off, "\n t%d=%s", i, t);
        }
    }

    if (stats_label) {
        lv_label_set_text(stats_label, buf);
        if (g_cfg.show_stats) {
            lv_obj_clear_flag(stats_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(stats_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}



// -------------------------
// Main
// -------------------------
int main(void)
{
    load_config();
    osd_width = g_cfg.width;
    osd_height = g_cfg.height;
    signal(SIGINT, handle_sigint);

    udp_sock = setup_udp_socket();

    printf("Initializing OSD region...\n");
    mi_region_init();

    printf("Initializing LVGL...\n");
    init_lvgl();

    // Transparent screen
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_TRANSP, LV_PART_MAIN);

    create_assets();

    // Lightweight stats in top-left
    stats_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(stats_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(stats_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stats_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stats_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stats_label, 4, LV_PART_MAIN);
    lv_obj_align(stats_label, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_text(stats_label, "OSD stats");

    // Timers (throttled to ~10 Hz)
    stats_timer = lv_timer_create(stats_timer_cb, 250, NULL);

    update_assets_from_udp();

    int idle_cap_ms = clamp_int(g_cfg.idle_ms, 10, 1000);
    idle_ms_applied = idle_cap_ms;

    // Main loop paced by a simple UDP poll cap
    while (!stop_requested) {
        uint64_t loop_start = monotonic_ms64();

        int wait_ms = idle_cap_ms;

        struct pollfd pfd = {0};
        nfds_t nfds = 0;
        if (udp_sock >= 0) {
            pfd.fd = udp_sock;
            pfd.events = POLLIN;
            nfds = 1;
        }

        uint64_t poll_start = monotonic_ms64();
        int ret = poll(nfds ? &pfd : NULL, nfds, wait_ms);
        uint64_t poll_spent = monotonic_ms64() - poll_start;
        idle_ms_applied = clamp_int((int)poll_spent, 0, idle_cap_ms);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (poll_udp()) {
                update_assets_from_udp();
            }
        }

        uint64_t frame_start = monotonic_ms64();
        lv_timer_handler();
        fps_frames++;
        last_frame_ms = (uint32_t)(monotonic_ms64() - frame_start);

        last_loop_ms = (uint32_t)(monotonic_ms64() - loop_start);
    }

    cleanup_resources();

    return 0;
}
