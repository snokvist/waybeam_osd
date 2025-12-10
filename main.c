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
#include <limits.h>
#include <stdbool.h>
#include <dlfcn.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_private.h"
#include "mi_sys.h"
#include "mi_rgn.h"
#include "mi_vpe.h"
#include "mi_venc.h"

#define DEFAULT_SCREEN_WIDTH 1280   // fallback resolution if config is absent
#define DEFAULT_SCREEN_HEIGHT 720
#define BUF_ROWS 60  // partial buffer height
#define CONFIG_PATH "/etc/waybeam_osd.json"
#define UDP_PORT 7777
#define UDP_MAX_PACKET 1280
#define UDP_VALUE_COUNT 8
#define SYSTEM_VALUE_COUNT 8
#define TOTAL_VALUE_COUNT (UDP_VALUE_COUNT + SYSTEM_VALUE_COUNT)
#define UDP_TEXT_COUNT 8
#define SYSTEM_TEXT_COUNT 8
#define TOTAL_TEXT_COUNT (UDP_TEXT_COUNT + SYSTEM_TEXT_COUNT)
#define TEXT_SLOT_MAX_CHARS 96
#define TEXT_SLOT_LEN (TEXT_SLOT_MAX_CHARS + 1)

enum {
    SYS_VALUE_TEMP = 0,
    SYS_VALUE_CPU_LOAD,
    SYS_VALUE_ENCODER_FPS,
    SYS_VALUE_ENCODER_BITRATE,
    SYS_VALUE_RESERVED4,
    SYS_VALUE_RESERVED5,
    SYS_VALUE_RESERVED6,
    SYS_VALUE_RESERVED7,
};

// LVGL buffers - allocated at runtime for ARGB8888 (32-bit per pixel)
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

typedef MI_S32 (*mi_venc_query_fn)(MI_VENC_CHN, MI_VENC_ChnStat_t *);

static mi_venc_query_fn pMI_VENC_Query = NULL;
static void *venc_dl_handle = NULL;
static int venc_dl_broken = 0;
static int venc_force_load = -1; /* lazy-env parsed */

typedef struct {
    int width;
    int height;
    int osd_x;
    int osd_y;
    int show_stats;
    int idle_ms;
    int udp_stats;
} app_config_t;

typedef enum {
    ASSET_BAR = 0,
    ASSET_TEXT,
} asset_type_t;

typedef enum {
    ORIENTATION_RIGHT = 0,
    ORIENTATION_LEFT,
} asset_orientation_t;

typedef struct {
    asset_type_t type;
    int id;
    int enabled;
    int value_index;
    int x;
    int y;
    int width;
    int height;
    float min;
    float max;
    uint32_t color;
    uint32_t text_color;
    int bg_style;
    int bg_opacity_pct;
    char label[64];
    int text_index;
    int text_indices[8];
    int text_indices_count;
    int text_inline;
    int rounded_outline;
    int segments;
    asset_orientation_t orientation;
} asset_cfg_t;

typedef struct {
    asset_cfg_t cfg;
    lv_obj_t *container_obj;
    lv_obj_t *obj;
    lv_obj_t *label_obj;
    int last_pct;
    char last_label_text[1024];
} asset_t;

typedef struct {
    uint32_t color;
    lv_opa_t opa;
} bg_style_t;

static const bg_style_t g_bg_styles[] = {
    {0x000000, LV_OPA_TRANSP}, // fully transparent baseline
    {0x000000, LV_OPA_50},
    {0xFFFFFF, LV_OPA_50},
    {0x111111, LV_OPA_70},
    {0x222222, LV_OPA_90},
    {0x2266CC, LV_OPA_60},
    {0x009688, LV_OPA_60},
    {0x4CAF50, LV_OPA_60},
    {0xFF9800, LV_OPA_70},
    {0xE91E63, LV_OPA_60},
    {0x9C27B0, LV_OPA_70},
};

static app_config_t g_cfg;
static int osd_width = DEFAULT_SCREEN_WIDTH;
static int osd_height = DEFAULT_SCREEN_HEIGHT;
static int osd_offset_x = 0;
static int osd_offset_y = 0;
static asset_t assets[8];
static int asset_count = 0;
static int rgn_pos_x = 0;
static int rgn_pos_y = 0;

#define MAX_ASSETS (int)(sizeof(assets) / sizeof(assets[0]))

// Sigmastar RGN
static MI_RGN_PaletteTable_t g_stPaletteTable = {};
static MI_RGN_HANDLE hRgnHandle;
static MI_RGN_ChnPort_t stVpeChnPort;
static MI_RGN_Attr_t stRgnAttr;
static MI_RGN_ChnPortParam_t stRgnChnAttr;
static MI_RGN_CanvasInfo_t g_cached_canvas_info;
static int g_canvas_info_valid = 0;
static int g_canvas_dirty = 0;

// UI
static lv_obj_t *stats_label = NULL;
static uint32_t last_frame_ms = 0;
static uint32_t last_loop_ms = 0;
static uint32_t fps_value = 0;
static uint64_t fps_start_ms = 0;
static uint32_t fps_frames = 0;
static int idle_ms_applied = 100;
static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t reload_requested = 0;
static lv_timer_t *stats_timer = NULL;
static const int max_ms = 32; // throttle channel pushes to ~30 fps
static int udp_sock = -1;
static double udp_values[UDP_VALUE_COUNT] = {0};
static double system_values[SYSTEM_VALUE_COUNT] = {0};
static char udp_texts[UDP_TEXT_COUNT][TEXT_SLOT_LEN] = {{0}};
static char system_texts[SYSTEM_TEXT_COUNT][TEXT_SLOT_LEN] = {{0}};
static int idle_cap_ms = 100;
static uint64_t last_system_refresh_ms = 0;
static uint64_t last_channel_push_ms = 0;
static bool pending_channel_flush = false;
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

static void init_system_channels(void)
{
    memset(system_values, 0, sizeof(system_values));
    memset(system_texts, 0, sizeof(system_texts));
    static const char *defaults[SYSTEM_TEXT_COUNT] = {
        "temp",
        "cpu",
        "enc fps",
        "bitrate",
        "sys4",
        "sys5",
        "sys6",
        "sys7",
    };
    for (int i = 0; i < SYSTEM_TEXT_COUNT; i++) {
        snprintf(system_texts[i], TEXT_SLOT_LEN, "%s", defaults[i]);
    }
}

static double get_value_channel(int idx)
{
    if (idx < 0) return 0.0;
    if (idx < UDP_VALUE_COUNT) return udp_values[idx];
    idx -= UDP_VALUE_COUNT;
    if (idx < SYSTEM_VALUE_COUNT) return system_values[idx];
    return 0.0;
}

static const char *get_text_channel(int idx)
{
    if (idx < 0) return "";
    if (idx < UDP_TEXT_COUNT) return udp_texts[idx];
    idx -= UDP_TEXT_COUNT;
    if (idx < SYSTEM_TEXT_COUNT) return system_texts[idx];
    return "";
}

static int to_canvas_x(int x)
{
    return x - osd_offset_x;
}

static int to_canvas_y(int y)
{
    return y - osd_offset_y;
}

static asset_orientation_t parse_orientation_string(const char *str, asset_orientation_t def)
{
    if (!str) return def;
    if (strcmp(str, "left") == 0) return ORIENTATION_LEFT;
    if (strcmp(str, "right") == 0) return ORIENTATION_RIGHT;
    return def;
}

static int estimate_label_width_px(const asset_cfg_t *cfg)
{
    if (!cfg) return 0;
    if (cfg->label[0] != '\0') {
        int len = (int)strlen(cfg->label);
        int px = len * 10;
        if (px < 48) px = 48;
        if (px > 240) px = 240;
        return px;
    }
    if (cfg->text_index >= 0 || cfg->text_indices_count > 0) {
        return 160;
    }
    return 0;
}

static void compute_osd_geometry(void)
{
    osd_offset_x = 0;
    osd_offset_y = 0;
    osd_width = g_cfg.width;
    osd_height = g_cfg.height;

    rgn_pos_x = g_cfg.osd_x;
    rgn_pos_y = g_cfg.osd_y;
    if (rgn_pos_x < 0) {
        osd_offset_x -= rgn_pos_x;
        rgn_pos_x = 0;
    }
    if (rgn_pos_y < 0) {
        osd_offset_y -= rgn_pos_y;
        rgn_pos_y = 0;
    }

    if (osd_width < 1) osd_width = 1;
    if (osd_height < 1) osd_height = 1;
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

static int json_get_bool_range(const char *start, const char *end, const char *key, int *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p || p >= end) return -1;
    p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return -1;
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

static void json_get_int_array_range(const char *start, const char *end, const char *key, int *out, int max_count, int *out_count)
{
    if (!out || max_count <= 0) return;
    const char *p = find_key_range(start, end, key);
    if (!p) return;
    p = strchr(p, '[');
    if (!p || p >= end) return;
    p++;
    int count = 0;
    while (p < end && count < max_count) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p == ']') break;
        char *endptr = NULL;
        int v = (int)strtol(p, &endptr, 0);
        if (!endptr || endptr == p || endptr > end) break;
        out[count++] = v;
        p = endptr;
        while (p < end && *p != ',' && *p != ']') p++;
        if (p < end && *p == ',') p++;
    }
    if (out_count) *out_count = count;
}

static lv_opa_t pct_to_opa(int pct)
{
    int clamped = clamp_int(pct, 0, 100);
    return (lv_opa_t)((clamped * 255) / 100);
}

static void apply_background_style(lv_obj_t *obj, int bg_style, int bg_opacity_pct, lv_part_t part);
static void layout_bar_asset(asset_t *asset);
static void bar_draw_event_cb(lv_event_t *e);
static void destroy_asset_visual(asset_t *asset);
static void create_asset_visual(asset_t *asset);
static void maybe_attach_asset_label(asset_t *asset);

static asset_t *find_asset_by_id(int id)
{
    for (int i = 0; i < asset_count; i++) {
        if (assets[i].cfg.id == id) return &assets[i];
    }
    return NULL;
}

static void init_asset_defaults(asset_t *a, int id)
{
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->cfg.type = ASSET_BAR;
    a->cfg.id = id;
    a->cfg.enabled = 1;
    a->cfg.value_index = clamp_int(id, 0, TOTAL_VALUE_COUNT - 1);
    a->cfg.x = 40;
    a->cfg.y = 60 + id * 60;
    a->cfg.width = 320;
    a->cfg.height = 32;
    a->cfg.min = 0.0f;
    a->cfg.max = 1.0f;
    a->cfg.color = 0x2266CC;
    a->cfg.text_color = 0xFFFFFF;
    a->cfg.bg_style = -1;
    a->cfg.bg_opacity_pct = -1;
    a->cfg.text_indices_count = 0;
    a->cfg.text_inline = 0;
    a->cfg.text_index = -1;
    a->cfg.orientation = ORIENTATION_RIGHT;
    a->cfg.rounded_outline = 0;
    a->cfg.segments = 0;
    a->cfg.label[0] = '\0';
    a->last_pct = -1;
    a->last_label_text[0] = '\0';
}

static void style_bar_container(asset_t *asset, lv_color_t fallback_color, lv_opa_t fallback_opa)
{
    if (!asset || !asset->container_obj) return;

    if (asset->cfg.bg_style >= 0) {
        apply_background_style(asset->container_obj, asset->cfg.bg_style, asset->cfg.bg_opacity_pct, 0);
    } else {
        lv_obj_set_style_bg_color(asset->container_obj, fallback_color, 0);
        lv_obj_set_style_bg_opa(asset->container_obj, fallback_opa, 0);
    }

    int radius = lv_obj_get_height(asset->container_obj) / 2;
    if (radius < 6) radius = 6;
    lv_obj_set_style_radius(asset->container_obj, radius, 0);
}

static void apply_asset_styles(asset_t *asset)
{
    if (!asset) return;
    if (!asset->cfg.enabled) return;
    const asset_cfg_t *cfg = &asset->cfg;

    switch (cfg->type) {
        case ASSET_BAR: {
            style_bar_container(asset, lv_color_hex(0x222222), LV_OPA_40);
            if (asset->obj) {
                int thickness = cfg->height > 0 ? cfg->height : (cfg->rounded_outline ? 20 : 32);
                lv_obj_set_style_bg_opa(asset->obj, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_set_style_bg_color(asset->obj, lv_color_hex(cfg->color), LV_PART_INDICATOR);
                lv_obj_set_style_bg_opa(asset->obj, LV_OPA_COVER, LV_PART_INDICATOR);
                lv_obj_set_style_radius(asset->obj, thickness / 2, LV_PART_MAIN);
                lv_obj_set_style_radius(asset->obj, thickness / 2, LV_PART_INDICATOR);
                lv_obj_set_style_border_width(asset->obj, cfg->rounded_outline ? 2 : 0, 0);
                lv_obj_set_style_border_color(asset->obj, lv_color_hex(cfg->color), 0);
                lv_obj_set_style_pad_all(asset->obj, cfg->rounded_outline ? 6 : 0, 0);
            }
            break;
        }
        case ASSET_TEXT:
            if (asset->obj) {
                apply_background_style(asset->obj, cfg->bg_style, cfg->bg_opacity_pct, 0);
                lv_obj_set_style_text_color(asset->obj, lv_color_hex(cfg->text_color), 0);
                lv_obj_set_style_text_opa(asset->obj, LV_OPA_COVER, 0);
            }
            break;
        default:
            break;
    }

    if (asset->label_obj) {
        if (cfg->type == ASSET_TEXT) {
            apply_background_style(asset->label_obj, cfg->bg_style, cfg->bg_opacity_pct, 0);
        } else {
            lv_obj_set_style_bg_opa(asset->label_obj, LV_OPA_TRANSP, 0);
        }
        lv_obj_set_style_text_color(asset->label_obj, lv_color_hex(cfg->text_color), 0);
        lv_obj_set_style_text_opa(asset->label_obj, LV_OPA_COVER, 0);
    }
}

static void apply_background_style(lv_obj_t *obj, int bg_style, int bg_opacity_pct, lv_part_t part)
{
    if (!obj) return;
    if (bg_style < 0 || bg_style >= (int)(sizeof(g_bg_styles) / sizeof(g_bg_styles[0]))) {
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, part);
        return;
    }

    lv_color_t c = lv_color_hex(g_bg_styles[bg_style].color);
    lv_obj_set_style_bg_color(obj, c, part);
    lv_opa_t opa = g_bg_styles[bg_style].opa;
    if (bg_opacity_pct >= 0) {
        opa = pct_to_opa(bg_opacity_pct);
    }
    lv_obj_set_style_bg_opa(obj, opa, part);
}

/*
 * LVGL v9 draw task hook that replaces the indicator fill with segmented
 * rectangles when asset->cfg.segments > 1.
 */
static void bar_draw_event_cb(lv_event_t *e)
{
    asset_t *asset = (asset_t *)lv_event_get_user_data(e);
    if (!asset || asset->cfg.segments <= 1) return;

    lv_draw_task_t *task = (lv_draw_task_t *)lv_event_get_param(e);
    if (!task) return;

    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)task->draw_dsc;
    if (!base || base->part != LV_PART_INDICATOR) return;

    lv_draw_fill_dsc_t *fill_dsc = lv_draw_task_get_fill_dsc(task);
    if (fill_dsc) fill_dsc->opa = LV_OPA_TRANSP;
    lv_draw_border_dsc_t *border_dsc = lv_draw_task_get_border_dsc(task);
    if (border_dsc) border_dsc->opa = LV_OPA_TRANSP;

    int pct = asset->last_pct;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int segs = asset->cfg.segments;
    lv_area_t track_area;
    lv_obj_get_content_coords(base->obj, &track_area);

    int total_w = lv_area_get_width(&track_area);
    int total_h = lv_area_get_height(&task->area);
    if (total_w <= 0 || total_h <= 0) return;

    int seg_w = segs > 0 ? total_w / segs : total_w;
    if (seg_w <= 0) {
        segs = 1;
        seg_w = total_w;
    }
    int remainder = total_w - seg_w * segs;
    int gap = seg_w >= 14 ? 3 : (seg_w >= 8 ? 2 : (seg_w >= 5 ? 1 : 0));
    int filled = (pct * segs + 99) / 100;
    if (pct == 0) filled = 0;
    if (filled > segs) filled = segs;

    lv_draw_rect_dsc_t seg_dsc;
    lv_draw_rect_dsc_init(&seg_dsc);
    lv_obj_init_draw_rect_dsc(base->obj, LV_PART_INDICATOR, &seg_dsc);
    seg_dsc.bg_opa = LV_OPA_COVER;
    seg_dsc.border_opa = LV_OPA_TRANSP;
    if (lv_color_to_int(seg_dsc.bg_color) == 0) {
        seg_dsc.bg_color = lv_color_hex(asset->cfg.color);
    }
    seg_dsc.radius = total_h / 3;
    if (seg_dsc.radius > total_h / 2) seg_dsc.radius = total_h / 2;

    lv_base_dir_t dir = lv_obj_get_style_base_dir(base->obj, LV_PART_INDICATOR);
    if (dir == LV_BASE_DIR_RTL) {
        int x = track_area.x2 + 1;
        for (int i = 0; i < filled; i++) {
            int w = seg_w + (i < remainder ? 1 : 0);
            int draw_w = w;
            if (i < segs - 1 && gap < draw_w) draw_w -= gap;
            if (draw_w <= 0) {
                x -= w;
                continue;
            }
            lv_area_t seg_area = task->area;
            seg_area.x2 = x - 1;
            seg_area.x1 = seg_area.x2 - draw_w + 1;
            lv_draw_rect(base->layer, &seg_dsc, &seg_area);
            x -= w;
        }
    } else {
        int x = track_area.x1;
        for (int i = 0; i < filled; i++) {
            int w = seg_w + (i < remainder ? 1 : 0);
            int draw_w = w;
            if (i < segs - 1 && gap < draw_w) draw_w -= gap;
            if (draw_w <= 0) {
                x += w;
                continue;
            }
            lv_area_t seg_area = task->area;
            seg_area.x1 = x;
            seg_area.x2 = x + draw_w - 1;
            lv_draw_rect(base->layer, &seg_dsc, &seg_area);
            x += w;
        }
    }
}

static void layout_bar_asset(asset_t *asset)
{
    if (!asset || !asset->container_obj || !asset->obj) return;
    const asset_cfg_t *cfg = &asset->cfg;
    int pad_x = 8;
    int pad_y = 6;
    int bar_width = cfg->width > 0 ? cfg->width : (cfg->rounded_outline ? 200 : 320);
    int bar_height = cfg->height > 0 ? cfg->height : (cfg->rounded_outline ? 20 : 32);
    int label_width = 0;
    int label_height = 0;

    if (asset->label_obj) {
        // Avoid a full layout pass when we already have cached label dimensions, but
        // fall back to calculating them if they haven't been resolved yet. This keeps
        // orientation changes from degenerating into repeated layouts while still
        // giving left/right placement accurate sizing data.
        label_width = lv_obj_get_width(asset->label_obj);
        label_height = lv_obj_get_height(asset->label_obj);
        if (label_width == 0 || label_height == 0) {
            lv_obj_update_layout(asset->label_obj);
            label_width = lv_obj_get_width(asset->label_obj);
            label_height = lv_obj_get_height(asset->label_obj);
        }
    }

    int extra_height = cfg->rounded_outline ? 4 : 0;
    int container_height = bar_height;
    if (label_height > container_height) container_height = label_height;
    container_height += pad_y * 2 + extra_height;
    int gap = (label_width > 0) ? pad_x : 0;
    int tail_pad = pad_x + (label_width > 0 ? 4 : 0);
    int container_width = pad_x + bar_width + gap + label_width + tail_pad;

    lv_obj_set_size(asset->container_obj, container_width, container_height);
    int container_x = to_canvas_x(cfg->x);
    if (cfg->orientation == ORIENTATION_LEFT) {
        container_x -= container_width;
    }
    lv_obj_set_pos(asset->container_obj, container_x, to_canvas_y(cfg->y));
    int base_radius_height = bar_height + pad_y * 2 + extra_height;
    int container_radius = base_radius_height / 2;
    if (container_radius < 6) container_radius = 6;
    if (container_radius > container_height / 2) container_radius = container_height / 2;
    lv_obj_set_style_radius(asset->container_obj, container_radius, 0);

    lv_obj_set_size(asset->obj, bar_width, bar_height);
    lv_obj_set_style_radius(asset->obj, bar_height / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(asset->obj, bar_height / 2, LV_PART_INDICATOR);
    lv_obj_set_style_base_dir(asset->obj, LV_BASE_DIR_LTR, LV_PART_MAIN);
    lv_obj_set_style_base_dir(asset->obj, LV_BASE_DIR_LTR, LV_PART_INDICATOR);

    if (cfg->orientation == ORIENTATION_LEFT) {
        lv_obj_set_style_base_dir(asset->obj, LV_BASE_DIR_RTL, LV_PART_MAIN);
        lv_obj_set_style_base_dir(asset->obj, LV_BASE_DIR_RTL, LV_PART_INDICATOR);
        int bar_x = pad_x + label_width + gap;
        if (asset->label_obj) {
            lv_obj_align(asset->label_obj, LV_ALIGN_LEFT_MID, pad_x, 0);
        }
        lv_obj_align(asset->obj, LV_ALIGN_LEFT_MID, bar_x, 0);
    } else {
        int label_x = pad_x + bar_width + gap;
        lv_obj_align(asset->obj, LV_ALIGN_LEFT_MID, pad_x, 0);
        if (asset->label_obj) {
            lv_obj_align(asset->label_obj, LV_ALIGN_LEFT_MID, label_x, 0);
        }
    }
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
    g_cfg.osd_x = 0;
    g_cfg.osd_y = 0;
    g_cfg.show_stats = 1;
    g_cfg.idle_ms = 100;
    g_cfg.udp_stats = 1;

    memset(udp_values, 0, sizeof(udp_values));
    memset(udp_texts, 0, sizeof(udp_texts));
    init_system_channels();
    last_system_refresh_ms = 0;
    last_channel_push_ms = 0;
    pending_channel_flush = false;

    memset(assets, 0, sizeof(assets));
    asset_count = 1;
    init_asset_defaults(&assets[0], 0);
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

        asset_t a;
        init_asset_defaults(&a, asset_count);

        char type_buf[32];
        if (json_get_string_range(obj_start, obj_end, "type", type_buf, sizeof(type_buf)) == 0) {
            if (strcmp(type_buf, "text") == 0) {
                a.cfg.type = ASSET_TEXT;
            } else {
                a.cfg.type = ASSET_BAR;
            }
        }

        int v = 0;
        float fv = 0.0f;
        if (json_get_bool_range(obj_start, obj_end, "enabled", &v) == 0 || json_get_bool_range(obj_start, obj_end, "enable", &v) == 0) a.cfg.enabled = v;
        if (json_get_int_range(obj_start, obj_end, "value_index", &v) == 0) a.cfg.value_index = clamp_int(v, 0, TOTAL_VALUE_COUNT - 1);
        if (json_get_int_range(obj_start, obj_end, "id", &v) == 0) a.cfg.id = clamp_int(v, 0, 63);
        if (json_get_int_range(obj_start, obj_end, "x", &v) == 0) a.cfg.x = v;
        if (json_get_int_range(obj_start, obj_end, "y", &v) == 0) a.cfg.y = v;
        if (json_get_int_range(obj_start, obj_end, "width", &v) == 0) a.cfg.width = v;
        if (json_get_int_range(obj_start, obj_end, "height", &v) == 0) a.cfg.height = v;
        if (json_get_float_range(obj_start, obj_end, "min", &fv) == 0) a.cfg.min = fv;
        if (json_get_float_range(obj_start, obj_end, "max", &fv) == 0) a.cfg.max = fv;
        if (json_get_int_range(obj_start, obj_end, "bar_color", &v) == 0) a.cfg.color = (uint32_t)v;
        if (json_get_int_range(obj_start, obj_end, "text_color", &v) == 0) a.cfg.text_color = (uint32_t)v;
        if (json_get_int_range(obj_start, obj_end, "background", &v) == 0) a.cfg.bg_style = clamp_int(v, -1, (int)(sizeof(g_bg_styles) / sizeof(g_bg_styles[0])) - 1);
        if (json_get_int_range(obj_start, obj_end, "background_opacity", &v) == 0) a.cfg.bg_opacity_pct = clamp_int(v, 0, 100);
        if (json_get_int_range(obj_start, obj_end, "segments", &v) == 0) a.cfg.segments = clamp_int(v, 0, 64);
        if (json_get_int_range(obj_start, obj_end, "text_index", &v) == 0) a.cfg.text_index = clamp_int(v, -1, TOTAL_TEXT_COUNT - 1);
        json_get_int_array_range(obj_start, obj_end, "text_indices", a.cfg.text_indices, 8, &a.cfg.text_indices_count);
        for (int i = 0; i < a.cfg.text_indices_count; i++) {
            a.cfg.text_indices[i] = clamp_int(a.cfg.text_indices[i], 0, TOTAL_TEXT_COUNT - 1);
        }
        if (json_get_bool_range(obj_start, obj_end, "text_inline", &v) == 0) a.cfg.text_inline = v;
        if (json_get_bool_range(obj_start, obj_end, "rounded_outline", &v) == 0) a.cfg.rounded_outline = v;
        json_get_string_range(obj_start, obj_end, "label", a.cfg.label, sizeof(a.cfg.label));
        char orient_buf[16];
        if (json_get_string_range(obj_start, obj_end, "orientation", orient_buf, sizeof(orient_buf)) == 0) {
            a.cfg.orientation = parse_orientation_string(orient_buf, ORIENTATION_RIGHT);
        }

        a.last_pct = -1;
        a.last_label_text[0] = '\0';

        assets[asset_count++] = a;
        if (asset_count >= MAX_ASSETS) break;
    }

    if (asset_count == 0) {
        memset(assets, 0, sizeof(assets));
        asset_count = 1;
        init_asset_defaults(&assets[0], 0);
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
    if (json_get_int(json, "osd_x", &v) == 0) g_cfg.osd_x = v;
    if (json_get_int(json, "osd_y", &v) == 0) g_cfg.osd_y = v;
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
    for (int i = 0; i < UDP_VALUE_COUNT; i++) {
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
    for (int i = 0; i < UDP_TEXT_COUNT; i++) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != '\"') break;
        p++; // skip quote
        const char *start = p;
        while (*p && *p != '\"') p++;
        size_t len = (size_t)(p - start);
        if (len > TEXT_SLOT_LEN - 1) len = TEXT_SLOT_LEN - 1;
        memcpy(udp_texts[i], start, len);
        udp_texts[i][len] = '\0';
        if (*p != '\"') break;
        p++; // skip closing quote
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
}

static void parse_udp_asset_updates(const char *buf)
{
    const char *p = strstr(buf, "\"asset_updates\"");
    if (!p) return;
    const char *arr = strchr(p, '[');
    if (!arr) return;
    p = arr + 1;

    while (*p) {
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

        int id = -1;
        if (json_get_int_range(obj_start, obj_end, "id", &id) != 0 || id < 0) {
            continue;
        }

        asset_t *asset = find_asset_by_id(id);
        if (!asset) {
            if (asset_count >= MAX_ASSETS) continue;
            asset = &assets[asset_count++];
            init_asset_defaults(asset, id);
            asset->cfg.enabled = 0;
        }

        int v = 0;
        float fv = 0.0f;
        int restyle = 0;
        int relayout = 0;
        int rerange = 0;
        int recreate = 0;
        int text_change = 0;

        int enabled_flag = asset->cfg.enabled;
        if (json_get_bool_range(obj_start, obj_end, "enabled", &v) == 0 || json_get_bool_range(obj_start, obj_end, "enable", &v) == 0) {
            enabled_flag = v ? 1 : 0;
        }

        char type_buf[32];
        if (json_get_string_range(obj_start, obj_end, "type", type_buf, sizeof(type_buf)) == 0) {
            asset_type_t new_type = asset->cfg.type;
            if (strcmp(type_buf, "text") == 0) {
                new_type = ASSET_TEXT;
            } else {
                new_type = ASSET_BAR;
            }
            if (new_type != asset->cfg.type) {
                asset->cfg.type = new_type;
                recreate = 1;
            }
        }

        if (json_get_int_range(obj_start, obj_end, "value_index", &v) == 0) {
            int idx = clamp_int(v, 0, TOTAL_VALUE_COUNT - 1);
            if (idx != asset->cfg.value_index) {
                asset->cfg.value_index = idx;
            }
        }

        if (json_get_int_range(obj_start, obj_end, "text_index", &v) == 0) {
            int idx = clamp_int(v, -1, TOTAL_TEXT_COUNT - 1);
            if (idx != asset->cfg.text_index) {
                asset->cfg.text_index = idx;
                text_change = 1;
            }
        }

        int indices_tmp[8] = {0};
        int idx_count = 0;
        if (find_key_range(obj_start, obj_end, "text_indices")) {
            json_get_int_array_range(obj_start, obj_end, "text_indices", indices_tmp, 8, &idx_count);
            for (int i = 0; i < idx_count; i++) {
                indices_tmp[i] = clamp_int(indices_tmp[i], 0, TOTAL_TEXT_COUNT - 1);
            }
            if (idx_count != asset->cfg.text_indices_count || memcmp(indices_tmp, asset->cfg.text_indices, sizeof(int) * (size_t)idx_count) != 0) {
                memcpy(asset->cfg.text_indices, indices_tmp, sizeof(int) * (size_t)idx_count);
                asset->cfg.text_indices_count = idx_count;
                text_change = 1;
            }
        }

        if (json_get_bool_range(obj_start, obj_end, "text_inline", &v) == 0) {
            int inline_flag = v ? 1 : 0;
            if (inline_flag != asset->cfg.text_inline) {
                asset->cfg.text_inline = inline_flag;
                text_change = 1;
            }
        }

        if (json_get_bool_range(obj_start, obj_end, "rounded_outline", &v) == 0) {
            int outline_flag = v ? 1 : 0;
            if (outline_flag != asset->cfg.rounded_outline) {
                asset->cfg.rounded_outline = outline_flag;
                recreate = 1;
            }
        }

        if (json_get_string_range(obj_start, obj_end, "label", asset->cfg.label, sizeof(asset->cfg.label)) == 0) {
            text_change = 1;
        }

        char orient_buf[16];
        if (json_get_string_range(obj_start, obj_end, "orientation", orient_buf, sizeof(orient_buf)) == 0) {
            asset_orientation_t orientation = parse_orientation_string(orient_buf, asset->cfg.orientation);
            if (orientation != asset->cfg.orientation) {
                asset->cfg.orientation = orientation;
                relayout = 1;
            }
        }

        if (json_get_int_range(obj_start, obj_end, "bar_color", &v) == 0) {
            uint32_t color = (uint32_t)v;
            if (asset->cfg.type != ASSET_TEXT && asset->cfg.color != color) {
                asset->cfg.color = color;
                restyle = 1;
            }
        }
        if (json_get_int_range(obj_start, obj_end, "text_color", &v) == 0) {
            uint32_t color = (uint32_t)v;
            if (asset->cfg.text_color != color) {
                asset->cfg.text_color = color;
                restyle = 1;
                text_change = 1;
            }
        }
        if (json_get_int_range(obj_start, obj_end, "background", &v) == 0) {
            int bg = clamp_int(v, -1, (int)(sizeof(g_bg_styles) / sizeof(g_bg_styles[0])) - 1);
            if (asset->cfg.bg_style != bg) {
                asset->cfg.bg_style = bg;
                restyle = 1;
            }
        }
        if (json_get_int_range(obj_start, obj_end, "background_opacity", &v) == 0) {
            int opa = clamp_int(v, 0, 100);
            if (asset->cfg.bg_opacity_pct != opa) {
                asset->cfg.bg_opacity_pct = opa;
                restyle = 1;
            }
        }

        if (json_get_int_range(obj_start, obj_end, "segments", &v) == 0) {
            int segs = clamp_int(v, 0, 64);
            if (asset->cfg.segments != segs) {
                asset->cfg.segments = segs;
                if (asset->obj) lv_obj_invalidate(asset->obj);
            }
        }

        if (json_get_int_range(obj_start, obj_end, "x", &v) == 0) {
            if (asset->cfg.x != v) {
                asset->cfg.x = v;
                relayout = 1;
            }
        }
        if (json_get_int_range(obj_start, obj_end, "y", &v) == 0) {
            if (asset->cfg.y != v) {
                asset->cfg.y = v;
                relayout = 1;
            }
        }
        if (json_get_int_range(obj_start, obj_end, "width", &v) == 0) {
            if (asset->cfg.width != v) {
                asset->cfg.width = v;
                relayout = 1;
                recreate = asset->cfg.type == ASSET_TEXT ? 1 : recreate;
            }
        }
        if (json_get_int_range(obj_start, obj_end, "height", &v) == 0) {
            if (asset->cfg.height != v) {
                asset->cfg.height = v;
                relayout = 1;
                recreate = asset->cfg.type == ASSET_TEXT ? 1 : recreate;
            }
        }
        if (json_get_float_range(obj_start, obj_end, "min", &fv) == 0) {
            if (asset->cfg.min != fv) {
                asset->cfg.min = fv;
                rerange = 1;
            }
        }
        if (json_get_float_range(obj_start, obj_end, "max", &fv) == 0) {
            if (asset->cfg.max != fv) {
                asset->cfg.max = fv;
                rerange = 1;
            }
        }

        int enabled_change = (enabled_flag != asset->cfg.enabled);
        asset->cfg.enabled = enabled_flag;

        if (!asset->cfg.enabled) {
            destroy_asset_visual(asset);
            continue;
        }

        if (!asset->obj || recreate || enabled_change) {
            create_asset_visual(asset);
            restyle = 1;
            relayout = 0;
            rerange = 1;
            text_change = 1;
        } else {
            if (relayout) {
                if (asset->cfg.type == ASSET_TEXT) {
                    int width = asset->cfg.width > 0 ? asset->cfg.width : LV_SIZE_CONTENT;
                    int height = asset->cfg.height > 0 ? asset->cfg.height : LV_SIZE_CONTENT;
                    lv_obj_set_size(asset->obj, width, height);
                    lv_obj_set_pos(asset->obj, asset->cfg.x, asset->cfg.y);
                } else {
                    layout_bar_asset(asset);
                }
            }

            if (rerange && asset->cfg.type == ASSET_BAR) {
                lv_bar_set_range(asset->obj, 0, 100);
                asset->last_pct = -1;
            }
        }

        if (restyle) {
            apply_asset_styles(asset);
        }

        if (text_change) {
            int wants_label = (asset->cfg.label[0] != '\0') || (asset->cfg.text_index >= 0);
            int label_created = 0;
            if (asset->cfg.type != ASSET_TEXT) {
                if (wants_label && !asset->label_obj) {
                    maybe_attach_asset_label(asset);
                    label_created = asset->label_obj != NULL;
                } else if (!wants_label && asset->label_obj) {
                    lv_obj_del(asset->label_obj);
                    asset->label_obj = NULL;
                    layout_bar_asset(asset);
                }
            }
            asset->last_label_text[0] = '\0';
            if (label_created) {
                apply_asset_styles(asset);
            }
        }
    }
}

static const char *get_asset_text(const asset_t *asset)
{
    if (asset->cfg.text_index >= 0 && asset->cfg.text_index < TOTAL_TEXT_COUNT) {
        const char *t = get_text_channel(asset->cfg.text_index);
        if (t[0] != '\0') return t;
    }
    if (asset->cfg.label[0] != '\0') return asset->cfg.label;
    return "";
}

static void compose_asset_text(const asset_t *asset, char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return;
    buf[0] = '\0';
    if (!asset) return;

        if (asset->cfg.type == ASSET_TEXT) {
            size_t written = 0;
            if (asset->cfg.text_indices_count > 0) {
                for (int i = 0; i < asset->cfg.text_indices_count; i++) {
                    int idx = clamp_int(asset->cfg.text_indices[i], 0, TOTAL_TEXT_COUNT - 1);
                    const char *t = get_text_channel(idx);
                    if (t[0] == '\0') continue;
                if (written > 0 && written < buf_sz - 1) {
                    buf[written++] = asset->cfg.text_inline ? ' ' : '\n';
                }
                size_t len = strnlen(t, TEXT_SLOT_LEN - 1);
                size_t to_copy = len < buf_sz - 1 - written ? len : buf_sz - 1 - written;
                memcpy(buf + written, t, to_copy);
                written += to_copy;
                if (written >= buf_sz - 1) break;
            }
        }

        if (written == 0 && asset->cfg.text_index >= 0 && asset->cfg.text_index < TOTAL_TEXT_COUNT) {
            const char *t = get_text_channel(asset->cfg.text_index);
            size_t len = strnlen(t, TEXT_SLOT_LEN - 1);
            size_t to_copy = len < buf_sz - 1 ? len : buf_sz - 1;
            memcpy(buf, t, to_copy);
            written = to_copy;
        }

        if (written == 0 && asset->cfg.label[0] != '\0') {
            strncpy(buf, asset->cfg.label, buf_sz - 1);
            buf[buf_sz - 1] = '\0';
        } else {
            buf[written] = '\0';
        }
        return;
    }

    const char *t = get_asset_text(asset);
    strncpy(buf, t, buf_sz - 1);
    buf[buf_sz - 1] = '\0';
}

static bool poll_udp(void)
{
    if (udp_sock < 0) return false;
    char buf[UDP_MAX_PACKET];
    ssize_t r = 0;
    bool updated = false;

    while ((r = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, NULL, NULL)) > 0) {
        buf[r] = '\0';
        parse_udp_values(buf);
        parse_udp_texts(buf);
        parse_udp_asset_updates(buf);
        updated = true;
    }

    return updated;
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

static bool set_system_value(int idx, double v)
{
    if (idx < 0 || idx >= SYSTEM_VALUE_COUNT) return false;
    double prev = system_values[idx];
    double diff = prev - v;
    if (diff < 0) diff = -diff;
    if (diff < 0.001) return false;
    system_values[idx] = v;
    return true;
}

static double read_soc_temperature(void)
{
    FILE *fp = popen("ipctool --temp 2>/dev/null", "r");
    if (!fp) return -1.0;
    char line[64];
    double temp = -1.0;
    if (fgets(line, sizeof(line), fp)) {
        double v = 0.0;
        if (sscanf(line, "%lf", &v) == 1) temp = v;
    }
    pclose(fp);
    return temp;
}

static double read_cpu_load_pct(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1.0;
    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1.0;
    }
    fclose(f);

    unsigned long long user = 0, nice = 0, system_time = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system_time, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        return -1.0;
    }

    unsigned long long idle_all = idle + iowait;
    unsigned long long non_idle = user + nice + system_time + irq + softirq + steal;
    unsigned long long total = idle_all + non_idle;
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;
    if (prev_total == 0 && prev_idle == 0) {
        prev_total = total;
        prev_idle = idle_all;
        return -1.0;
    }

    unsigned long long totald = total - prev_total;
    unsigned long long idled = idle_all - prev_idle;
    prev_total = total;
    prev_idle = idle_all;
    if (totald == 0) return -1.0;

    double pct = (double)(totald - idled) * 100.0 / (double)totald;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}

static bool ensure_venc_query_loaded(void)
{
    if (pMI_VENC_Query) return true;
    if (venc_dl_broken) return false;

    if (venc_force_load < 0) {
        const char *env = getenv("WAYBEAM_VENC_FORCE_LOAD");
        venc_force_load = (env && env[0] == '1') ? 1 : 0;
    }

    int flags = RTLD_LAZY | RTLD_LOCAL;
#ifdef RTLD_NODELETE
    flags |= RTLD_NODELETE;
#endif

#ifdef RTLD_NOLOAD
    venc_dl_handle = dlopen("libmi_venc.so", flags | RTLD_NOLOAD);
#else
    venc_dl_handle = NULL;
#endif

    if (!venc_dl_handle && venc_force_load) {
        venc_dl_handle = dlopen("libmi_venc.so", flags);
        if (!venc_dl_handle) {
            fprintf(stderr, "[enc] dlopen libmi_venc.so failed (force load): %s\n", dlerror());
            venc_dl_broken = 1;
            return false;
        }
    }

    if (!venc_dl_handle) {
#ifdef RTLD_NOLOAD
        fprintf(stderr, "[enc] libmi_venc.so not preloaded; skipping encoder stats (set WAYBEAM_VENC_FORCE_LOAD=1 to force)\n");
#else
        fprintf(stderr, "[enc] libmi_venc.so not available; skipping encoder stats\n");
#endif
        venc_dl_broken = 1;
        return false;
    }

    pMI_VENC_Query = (mi_venc_query_fn)dlsym(venc_dl_handle, "MI_VENC_Query");
    if (!pMI_VENC_Query) {
        fprintf(stderr, "[enc] dlsym MI_VENC_Query failed: %s\n", dlerror());
        venc_dl_broken = 1;
        return false;
    }

    return true;
}

static bool query_encoder_stats(double *fps_out, double *bitrate_out)
{
    if (!fps_out || !bitrate_out) return false;
    if (!ensure_venc_query_loaded()) return false;
    MI_VENC_ChnStat_t stat;
    memset(&stat, 0, sizeof(stat));
    MI_S32 ret = pMI_VENC_Query(0, &stat);
    if (ret != MI_SUCCESS) {
        fprintf(stderr, "[enc] MI_VENC_Query failed: %d\n", ret);
        return false;
    }
    if (stat.u32FrmRateDen == 0) {
        fprintf(stderr, "[enc] MI_VENC_Query returned zero denominator (num=%u, br=%u)\n", stat.u32FrmRateNum, stat.u32BitRate);
        return false;
    }
    *fps_out = (double)stat.u32FrmRateNum / (double)stat.u32FrmRateDen;
    *bitrate_out = (double)stat.u32BitRate;
    if (*fps_out <= 0.0 || *bitrate_out <= 0.0) {
        fprintf(stderr, "[enc] fps=%.2f bitrate=%.2f (num=%u den=%u br=%u)\n",
                *fps_out, *bitrate_out, stat.u32FrmRateNum, stat.u32FrmRateDen, stat.u32BitRate);
    }
    return true;
}

static bool refresh_system_values(void)
{
    uint64_t now = monotonic_ms64();
    if (last_system_refresh_ms != 0 && now - last_system_refresh_ms < 1000) return false;
    last_system_refresh_ms = now;

    bool changed = false;

    double temp = read_soc_temperature();
    if (temp >= 0.0) changed |= set_system_value(SYS_VALUE_TEMP, temp);

    double cpu = read_cpu_load_pct();
    if (cpu >= 0.0) changed |= set_system_value(SYS_VALUE_CPU_LOAD, cpu);

    double fps = 0.0;
    double bitrate = 0.0;
    if (query_encoder_stats(&fps, &bitrate)) {
        changed |= set_system_value(SYS_VALUE_ENCODER_FPS, fps);
        changed |= set_system_value(SYS_VALUE_ENCODER_BITRATE, bitrate);
    }

    return changed;
}

static const MI_RGN_CanvasInfo_t *get_cached_canvas(void)
{
    if (!g_canvas_info_valid || !g_cached_canvas_info.virtAddr) {
        if (MI_RGN_GetCanvasInfo(hRgnHandle, &g_cached_canvas_info) != MI_RGN_OK) {
            g_canvas_info_valid = 0;
            return NULL;
        }
        g_canvas_info_valid = 1;
    }
    return &g_cached_canvas_info;
}

static void clear_rgn_canvas(void)
{
    const MI_RGN_CanvasInfo_t *info = get_cached_canvas();
    if (!info) return;
    MI_U32 stride = info->u32Stride;
    MI_U32 height = info->stSize.u32Height;
    if (stride == 0 || height == 0) return;
    MI_U32 size = stride * height;

    if (info->phyAddr) {
        MI_SYS_MemsetPa(info->phyAddr, 0, size);
    } else if (info->virtAddr) {
        memset((void *)info->virtAddr, 0, size);
    }
    MI_RGN_UpdateCanvas(hRgnHandle);
    g_canvas_dirty = 0;
    g_canvas_info_valid = 0;
    memset(&g_cached_canvas_info, 0, sizeof(g_cached_canvas_info));
}

// -------------------------
// LVGL flush callback
// -------------------------
void my_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    const MI_RGN_CanvasInfo_t *info = get_cached_canvas();

    if (!info || !info->virtAddr) {
        lv_display_flush_ready(disp);
        return;
    }

    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    uint32_t *src = (uint32_t *)px_map;  // Source is ARGB8888 (32-bit)

    for (int y = 0; y < h; y++) {
        uint16_t *dest = (uint16_t *)(info->virtAddr +
                                      (area->y1 + y) * info->u32Stride +
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

    g_canvas_dirty = 1;
    lv_display_flush_ready(disp);
}

// -------------------------
// Initialize RGN
// -------------------------
void mi_region_init(void)
{
    MI_RGN_Init(&g_stPaletteTable);
    hRgnHandle = 0;

    g_canvas_info_valid = 0;
    memset(&g_cached_canvas_info, 0, sizeof(g_cached_canvas_info));

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
    stRgnChnAttr.stPoint.u32X = (MI_U32)rgn_pos_x;
    stRgnChnAttr.stPoint.u32Y = (MI_U32)rgn_pos_y;
    stRgnChnAttr.unPara.stOsdChnPort.u32Layer = 0;
    stRgnChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode = E_MI_RGN_PIXEL_ALPHA;

    MI_RGN_AttachToChn(hRgnHandle, &stVpeChnPort, &stRgnChnAttr);
    clear_rgn_canvas();
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


static lv_obj_t *create_bar(asset_t *asset)
{
    if (!asset) return NULL;
    const asset_cfg_t *cfg = &asset->cfg;
    asset->container_obj = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(asset->container_obj);
    lv_obj_clear_flag(asset->container_obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_bar_create(asset->container_obj);
    lv_obj_remove_style_all(bar);
    if (cfg->rounded_outline) {
        lv_obj_set_style_border_width(bar, 2, 0);
        lv_obj_set_style_border_color(bar, lv_color_hex(cfg->color), 0);
        lv_obj_set_style_pad_all(bar, 6, 0);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_set_style_anim_duration(bar, 1000, 0);
    } else {
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, cfg->height > 0 ? cfg->height / 2 : 16, LV_PART_MAIN);
    }
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(cfg->color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(bar, bar_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, asset);
    lv_bar_set_range(bar, 0, 100);
    return bar;
}

static void destroy_asset_visual(asset_t *asset)
{
    if (!asset) return;
    if (asset->container_obj) {
        lv_obj_del(asset->container_obj);
    } else {
        if (asset->label_obj) lv_obj_del(asset->label_obj);
        if (asset->obj) lv_obj_del(asset->obj);
    }
    asset->container_obj = NULL;
    asset->label_obj = NULL;
    asset->obj = NULL;
    asset->last_pct = -1;
    asset->last_label_text[0] = '\0';
}

static lv_obj_t *create_text_asset(asset_t *asset)
{
    lv_obj_t *label = lv_label_create(lv_scr_act());
    int width = asset->cfg.width > 0 ? asset->cfg.width : LV_SIZE_CONTENT;
    int height = asset->cfg.height > 0 ? asset->cfg.height : LV_SIZE_CONTENT;
    lv_obj_set_size(label, width, height);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, to_canvas_x(asset->cfg.x), to_canvas_y(asset->cfg.y));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    apply_background_style(label, asset->cfg.bg_style, asset->cfg.bg_opacity_pct, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(asset->cfg.text_color), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);

    char text_buf[128];
    compose_asset_text(asset, text_buf, sizeof(text_buf));
    lv_label_set_text(label, text_buf);
    strncpy(asset->last_label_text, text_buf, sizeof(asset->last_label_text) - 1);
    asset->last_label_text[sizeof(asset->last_label_text) - 1] = '\0';
    return label;
}

static void create_asset_visual(asset_t *asset)
{
    if (!asset || !asset->cfg.enabled) return;
    destroy_asset_visual(asset);
    switch (asset->cfg.type) {
        case ASSET_BAR:
            asset->obj = create_bar(asset);
            maybe_attach_asset_label(asset);
            break;
        case ASSET_TEXT:
            asset->obj = create_text_asset(asset);
            break;
        default:
            asset->obj = create_bar(asset);
            maybe_attach_asset_label(asset);
            break;
    }

    if (asset->container_obj && asset->cfg.type != ASSET_TEXT) {
        layout_bar_asset(asset);
    }

    apply_asset_styles(asset);
}

static void maybe_attach_asset_label(asset_t *asset)
{
    if (!asset->obj) return;
    if (asset->cfg.type == ASSET_TEXT) return;
    if (asset->cfg.label[0] == '\0' && asset->cfg.text_index < 0) return;
    lv_obj_t *parent = asset->container_obj ? asset->container_obj : lv_scr_act();
    asset->label_obj = lv_label_create(parent);
    lv_obj_set_style_text_color(asset->label_obj, lv_color_hex(asset->cfg.text_color), 0);
    lv_obj_set_style_text_opa(asset->label_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(asset->label_obj, LV_OPA_TRANSP, 0);

    char text_buf[128];
    compose_asset_text(asset, text_buf, sizeof(text_buf));
    lv_label_set_text(asset->label_obj, text_buf);
    strncpy(asset->last_label_text, text_buf, sizeof(asset->last_label_text) - 1);
    asset->last_label_text[sizeof(asset->last_label_text) - 1] = '\0';
    if (asset->container_obj) {
        layout_bar_asset(asset);
    } else {
        lv_obj_align_to(asset->label_obj, asset->obj, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    }
}

static void create_assets(void)
{
    for (int i = 0; i < asset_count; i++) {
        create_asset_visual(&assets[i]);
    }
}

static void destroy_assets(void)
{
    for (int i = 0; i < asset_count; i++) {
        destroy_asset_visual(&assets[i]);
    }

    asset_count = 0;
    memset(assets, 0, sizeof(assets));
}

static void update_assets_from_channels(void)
{
    for (int i = 0; i < asset_count; i++) {
        if (!assets[i].cfg.enabled) continue;
        const asset_cfg_t *cfg = &assets[i].cfg;
        float min = cfg->min;
        float max = cfg->max;
        if (max <= min + 0.0001f) {
            max = min + 1.0f;
        }
        float v = (float)get_value_channel(clamp_int(cfg->value_index, 0, TOTAL_VALUE_COUNT - 1));
        v = clamp_float(v, min, max);
        float pct_f = (v - min) / (max - min);
        int pct = clamp_int((int)(pct_f * 100.0f), 0, 100);

        switch (cfg->type) {
            case ASSET_BAR:
                if (assets[i].obj && assets[i].last_pct != pct) {
                    lv_bar_set_value(assets[i].obj, pct, LV_ANIM_OFF);
                    assets[i].last_pct = pct;
                }
                break;
            case ASSET_TEXT: {
                if (assets[i].obj) {
                    char text_buf[1024];
                    compose_asset_text(&assets[i], text_buf, sizeof(text_buf));
                    if (strncmp(text_buf, assets[i].last_label_text, sizeof(assets[i].last_label_text) - 1) != 0) {
                        lv_label_set_text(assets[i].obj, text_buf);
                        strncpy(assets[i].last_label_text, text_buf, sizeof(assets[i].last_label_text) - 1);
                        assets[i].last_label_text[sizeof(assets[i].last_label_text) - 1] = '\0';
                    }
                }
                continue;
            }
            default:
                break;
        }

        if (assets[i].label_obj) {
            char text_buf[1024];
            compose_asset_text(&assets[i], text_buf, sizeof(text_buf));
            if (strncmp(text_buf, assets[i].last_label_text, sizeof(assets[i].last_label_text) - 1) != 0) {
                lv_label_set_text(assets[i].label_obj, text_buf);
                lv_obj_update_layout(assets[i].label_obj);
                strncpy(assets[i].last_label_text, text_buf, sizeof(assets[i].last_label_text) - 1);
                assets[i].last_label_text[sizeof(assets[i].last_label_text) - 1] = '\0';
                if (assets[i].container_obj) {
                    layout_bar_asset(&assets[i]);
                }
            }
        }
    }
}

static void handle_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void handle_sighup(int sig)
{
    (void)sig;
    reload_requested = 1;
}

static void reload_config_runtime(void)
{
    printf("Reloading config...\n");

    destroy_assets();
    load_config();

    idle_cap_ms = clamp_int(g_cfg.idle_ms, 10, 1000);
    idle_ms_applied = idle_cap_ms;

    create_assets();
    refresh_system_values();
    update_assets_from_channels();
    pending_channel_flush = false;
    last_channel_push_ms = monotonic_ms64();

    if (stats_label) {
        if (g_cfg.show_stats) {
            lv_obj_clear_flag(stats_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(stats_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    fps_start_ms = monotonic_ms64();
    fps_frames = 0;
}

static void cleanup_resources(void)
{
    destroy_assets();

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
    int active_assets = 0;
    for (int i = 0; i < asset_count; i++) {
        if (!assets[i].cfg.enabled) continue;
        active_assets++;
        if (primary_w == 0 && assets[i].obj) {
            primary_w = lv_obj_get_width(assets[i].obj);
            primary_h = lv_obj_get_height(assets[i].obj);
        }
    }
    char buf[1024];
    int disp_w = lv_disp_get_hor_res(NULL);
    int disp_h = lv_disp_get_ver_res(NULL);
    int off = 0;
    off += lv_snprintf(buf + off, sizeof(buf) - off,
                       "OSD %dx%d (disp %dx%d)\n"
                       "Assets %d/%d | primary %d,%d\n"
                       "FPS %u | work %ums | loop %ums | idle %dms",
                       osd_width, osd_height,
                       disp_w, disp_h,
                       active_assets, asset_count, primary_w, primary_h,
                       fps_value, last_frame_ms, last_loop_ms, idle_ms_applied);

    if (off < (int)sizeof(buf) - 32) {
        int rows = UDP_VALUE_COUNT > SYSTEM_VALUE_COUNT ? UDP_VALUE_COUNT : SYSTEM_VALUE_COUNT;
        off += lv_snprintf(buf + off, sizeof(buf) - off, "\nValues (v=UDP s=SYS):");
        for (int i = 0; i < rows && off < (int)sizeof(buf) - 24; i++) {
            char udp_val[24];
            char sys_val[24];
            if (g_cfg.udp_stats && i < UDP_VALUE_COUNT) {
                int whole = (int)udp_values[i];
                int frac = (int)((udp_values[i] - whole) * 100.0);
                if (frac < 0) frac = -frac;
                lv_snprintf(udp_val, sizeof(udp_val), "%d.%02d", whole, frac);
            } else {
                lv_snprintf(udp_val, sizeof(udp_val), "-");
            }

            if (i < SYSTEM_VALUE_COUNT) {
                int whole = (int)system_values[i];
                int frac = (int)((system_values[i] - whole) * 100.0);
                if (frac < 0) frac = -frac;
                lv_snprintf(sys_val, sizeof(sys_val), "%d.%02d", whole, frac);
            } else {
                lv_snprintf(sys_val, sizeof(sys_val), "-");
            }

            off += lv_snprintf(buf + off, sizeof(buf) - off, "\n %d v=%s | s=%s", i, udp_val, sys_val);
        }

        rows = UDP_TEXT_COUNT > SYSTEM_TEXT_COUNT ? UDP_TEXT_COUNT : SYSTEM_TEXT_COUNT;
        off += lv_snprintf(buf + off, sizeof(buf) - off, "\nTexts (t=UDP s=SYS):");
        for (int i = 0; i < rows && off < (int)sizeof(buf) - 20; i++) {
            const char *udp_t = (g_cfg.udp_stats && i < UDP_TEXT_COUNT && udp_texts[i][0]) ? udp_texts[i] : "-";
            const char *sys_t = (i < SYSTEM_TEXT_COUNT && system_texts[i][0]) ? system_texts[i] : "-";
            off += lv_snprintf(buf + off, sizeof(buf) - off, "\n %d t=%s | s=%s", i, udp_t, sys_t);
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
    compute_osd_geometry();
    signal(SIGINT, handle_sigint);
    signal(SIGHUP, handle_sighup);

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
    lv_obj_align(stats_label, LV_ALIGN_TOP_LEFT, to_canvas_x(4), to_canvas_y(4));
    lv_label_set_text(stats_label, "OSD stats");

    // Timers (throttled to ~10 Hz)
    stats_timer = lv_timer_create(stats_timer_cb, 250, NULL);

    refresh_system_values();
    update_assets_from_channels();
    pending_channel_flush = false;
    last_channel_push_ms = monotonic_ms64();

    idle_cap_ms = clamp_int(g_cfg.idle_ms, 10, 1000);
    idle_ms_applied = idle_cap_ms;

    // Main loop paced by a simple UDP poll cap
    while (!stop_requested) {
        if (reload_requested) {
            reload_requested = 0;
            reload_config_runtime();
        }

        uint64_t loop_start = monotonic_ms64();

        if (refresh_system_values()) pending_channel_flush = true;

        uint64_t now_for_wait = monotonic_ms64();
        int wait_ms = idle_cap_ms;
        if (pending_channel_flush && last_channel_push_ms != 0) {
            uint64_t earliest_push = last_channel_push_ms + (uint64_t)max_ms;
            if (earliest_push > now_for_wait) {
                uint64_t remaining = earliest_push - now_for_wait;
                int until_push = clamp_int((int)remaining, 0, wait_ms);
                wait_ms = until_push;
            }
        }

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
                pending_channel_flush = true;
            }
        }

        uint64_t now = monotonic_ms64();
        if (pending_channel_flush) {
            if (last_channel_push_ms == 0 || now - last_channel_push_ms >= (uint64_t)max_ms) {
                update_assets_from_channels();
                pending_channel_flush = false;
                last_channel_push_ms = now;
            }
        }

        uint64_t frame_start = monotonic_ms64();
        lv_timer_handler();
        if (g_canvas_dirty) {
            MI_RGN_UpdateCanvas(hRgnHandle);
            g_canvas_dirty = 0;
            g_canvas_info_valid = 0;
            memset(&g_cached_canvas_info, 0, sizeof(g_cached_canvas_info));
        }
        fps_frames++;
        last_frame_ms = (uint32_t)(monotonic_ms64() - frame_start);

        last_loop_ms = (uint32_t)(monotonic_ms64() - loop_start);
    }

    cleanup_resources();

    return 0;
}
