#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
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
#include <limits.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_private.h"
#include "lvgl/src/libs/svg/lv_svg.h"
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
    ASSET_TEXT,
    ASSET_IMAGE,
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
    int image_opacity_pct;
    char label[64];
    char image_path[256];
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
    char last_label_text[128];
    char image_src[288];
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
static asset_t assets[8];
static int asset_count = 0;
typedef struct {
    int enabled;
    int duration_ms;
    asset_cfg_t cfg;
} splash_cfg_t;

static splash_cfg_t g_splash_cfg;
static asset_t splash_asset;
static lv_timer_t *splash_timer = NULL;

#define MAX_ASSETS (int)(sizeof(assets) / sizeof(assets[0]))

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
static volatile sig_atomic_t reload_requested = 0;
static lv_timer_t *stats_timer = NULL;
static int udp_sock = -1;
static double udp_values[8] = {0};
static char udp_texts[8][17] = {{0}};
static int idle_cap_ms = 100;
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

static asset_orientation_t parse_orientation_string(const char *str, asset_orientation_t def)
{
    if (!str) return def;
    if (strcmp(str, "left") == 0) return ORIENTATION_LEFT;
    if (strcmp(str, "right") == 0) return ORIENTATION_RIGHT;
    return def;
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

// Minimal FS bridge so LVGL's SVG decoder can read POSIX paths
typedef struct {
    FILE *f;
} fs_file_t;

static void *fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    const char *flags = (mode & LV_FS_MODE_WR) ? "wb" : "rb";
    FILE *f = fopen(path, flags);
    if (!f) return NULL;
    fs_file_t *file = (fs_file_t *)malloc(sizeof(fs_file_t));
    if (!file) {
        fclose(f);
        return NULL;
    }
    file->f = f;
    return file;
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    fs_file_t *file = (fs_file_t *)file_p;
    if (!file) return LV_FS_RES_INV_PARAM;
    fclose(file->f);
    free(file);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;
    fs_file_t *file = (fs_file_t *)file_p;
    if (!file || !file->f) return LV_FS_RES_INV_PARAM;
    size_t r = fread(buf, 1, btr, file->f);
    if (br) *br = (uint32_t)r;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    (void)drv;
    fs_file_t *file = (fs_file_t *)file_p;
    if (!file || !file->f) return LV_FS_RES_INV_PARAM;
    int origin = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) origin = SEEK_CUR;
    else if (whence == LV_FS_SEEK_END) origin = SEEK_END;
    if (fseek(file->f, (long)pos, origin) != 0) return LV_FS_RES_FS_ERR;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos)
{
    (void)drv;
    fs_file_t *file = (fs_file_t *)file_p;
    if (!file || !file->f || !pos) return LV_FS_RES_INV_PARAM;
    long p = ftell(file->f);
    if (p < 0) return LV_FS_RES_FS_ERR;
    *pos = (uint32_t)p;
    return LV_FS_RES_OK;
}

static void register_simple_fs_driver(void)
{
#if LV_USE_SVG
    static int registered = 0;
    if (registered) return;
    lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = 'S';
    drv.open_cb = fs_open_cb;
    drv.close_cb = fs_close_cb;
    drv.read_cb = fs_read_cb;
    drv.seek_cb = fs_seek_cb;
    drv.tell_cb = fs_tell_cb;
    lv_fs_drv_register(&drv);
    lv_svg_init();
    registered = 1;
#endif
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
static void splash_timer_cb(lv_timer_t *timer);
static void clear_splashscreen(void);
static void maybe_show_splashscreen(void);

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
    a->cfg.value_index = clamp_int(id, 0, 7);
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
    a->cfg.image_opacity_pct = 100;
    a->cfg.text_indices_count = 0;
    a->cfg.text_inline = 0;
    a->cfg.text_index = -1;
    a->cfg.orientation = ORIENTATION_RIGHT;
    a->cfg.rounded_outline = 0;
    a->cfg.segments = 0;
    a->cfg.label[0] = '\0';
    a->cfg.image_path[0] = '\0';
    a->last_pct = -1;
    a->last_label_text[0] = '\0';
    a->image_src[0] = '\0';
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
        case ASSET_IMAGE:
            if (asset->obj) {
                apply_background_style(asset->obj, cfg->bg_style, cfg->bg_opacity_pct, 0);
                lv_opa_t opa = cfg->image_opacity_pct >= 0 ? pct_to_opa(cfg->image_opacity_pct) : LV_OPA_COVER;
                lv_obj_set_style_opa(asset->obj, opa, 0);
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
    int container_x = cfg->x;
    if (cfg->orientation == ORIENTATION_LEFT) {
        container_x -= container_width;
    }
    lv_obj_set_pos(asset->container_obj, container_x, cfg->y);
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
    g_cfg.show_stats = 1;
    g_cfg.idle_ms = 100;
    g_cfg.udp_stats = 0;

    memset(assets, 0, sizeof(assets));
    asset_count = 1;
    init_asset_defaults(&assets[0], 0);

    memset(&splash_asset, 0, sizeof(splash_asset));
    init_asset_defaults(&splash_asset, -1);
    memset(&g_splash_cfg, 0, sizeof(g_splash_cfg));
    g_splash_cfg.enabled = 0;
    g_splash_cfg.duration_ms = 0;
    g_splash_cfg.cfg = splash_asset.cfg;
    g_splash_cfg.cfg.type = ASSET_IMAGE;
    g_splash_cfg.cfg.bg_style = -1;
    g_splash_cfg.cfg.bg_opacity_pct = -1;
    g_splash_cfg.cfg.image_opacity_pct = 100;
    g_splash_cfg.cfg.enabled = 1;
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
            } else if (strcmp(type_buf, "image") == 0) {
                a.cfg.type = ASSET_IMAGE;
            } else {
                a.cfg.type = ASSET_BAR;
            }
        }

        int v = 0;
        float fv = 0.0f;
        if (json_get_bool_range(obj_start, obj_end, "enabled", &v) == 0 || json_get_bool_range(obj_start, obj_end, "enable", &v) == 0) a.cfg.enabled = v;
        if (json_get_int_range(obj_start, obj_end, "value_index", &v) == 0) a.cfg.value_index = clamp_int(v, 0, 7);
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
        if (json_get_int_range(obj_start, obj_end, "text_index", &v) == 0) a.cfg.text_index = clamp_int(v, -1, 7);
        json_get_int_array_range(obj_start, obj_end, "text_indices", a.cfg.text_indices, 8, &a.cfg.text_indices_count);
        if (json_get_bool_range(obj_start, obj_end, "text_inline", &v) == 0) a.cfg.text_inline = v;
        if (json_get_bool_range(obj_start, obj_end, "rounded_outline", &v) == 0) a.cfg.rounded_outline = v;
        if (json_get_int_range(obj_start, obj_end, "image_opacity", &v) == 0) a.cfg.image_opacity_pct = clamp_int(v, 0, 100);
        json_get_string_range(obj_start, obj_end, "label", a.cfg.label, sizeof(a.cfg.label));
        json_get_string_range(obj_start, obj_end, "image_path", a.cfg.image_path, sizeof(a.cfg.image_path));
        if (a.cfg.image_path[0] == '\0') {
            json_get_string_range(obj_start, obj_end, "source", a.cfg.image_path, sizeof(a.cfg.image_path));
        }
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

static void parse_splashscreen(const char *json)
{
    const char *p = strstr(json, "\"splashscreen\"");
    if (!p) return;
    const char *obj = strchr(p, '{');
    if (!obj) return;
    const char *cursor = obj + 1;
    int depth = 1;
    while (*cursor && depth > 0) {
        if (*cursor == '{') depth++;
        else if (*cursor == '}') depth--;
        cursor++;
    }
    if (depth != 0) return;
    const char *obj_end = cursor;

    int v = 0;
    float fv = 0.0f;
    asset_cfg_t *cfg = &g_splash_cfg.cfg;
    if (json_get_bool_range(obj, obj_end, "enabled", &v) == 0) g_splash_cfg.enabled = v;
    if (json_get_int_range(obj, obj_end, "duration_ms", &v) == 0) g_splash_cfg.duration_ms = clamp_int(v, 0, 60000);
    if (json_get_int_range(obj, obj_end, "x", &v) == 0) cfg->x = v;
    if (json_get_int_range(obj, obj_end, "y", &v) == 0) cfg->y = v;
    if (json_get_int_range(obj, obj_end, "width", &v) == 0) cfg->width = v;
    if (json_get_int_range(obj, obj_end, "height", &v) == 0) cfg->height = v;
    if (json_get_int_range(obj, obj_end, "background", &v) == 0) cfg->bg_style = clamp_int(v, -1, (int)(sizeof(g_bg_styles) / sizeof(g_bg_styles[0])) - 1);
    if (json_get_int_range(obj, obj_end, "background_opacity", &v) == 0) cfg->bg_opacity_pct = clamp_int(v, 0, 100);
    if (json_get_int_range(obj, obj_end, "image_opacity", &v) == 0) cfg->image_opacity_pct = clamp_int(v, 0, 100);
    if (json_get_float_range(obj, obj_end, "min", &fv) == 0) cfg->min = fv;
    if (json_get_float_range(obj, obj_end, "max", &fv) == 0) cfg->max = fv;
    json_get_string_range(obj, obj_end, "image_path", cfg->image_path, sizeof(cfg->image_path));
    if (cfg->image_path[0] == '\0') {
        json_get_string_range(obj, obj_end, "source", cfg->image_path, sizeof(cfg->image_path));
    }
    cfg->type = ASSET_IMAGE;
    cfg->enabled = 1;
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

    // Optional splashscreen block (ARGB or SVG image)
    parse_splashscreen(json);

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
            } else if (strcmp(type_buf, "image") == 0) {
                new_type = ASSET_IMAGE;
            } else {
                new_type = ASSET_BAR;
            }
            if (new_type != asset->cfg.type) {
                asset->cfg.type = new_type;
                recreate = 1;
            }
        }

        if (json_get_int_range(obj_start, obj_end, "value_index", &v) == 0) {
            int idx = clamp_int(v, 0, 7);
            if (idx != asset->cfg.value_index) {
                asset->cfg.value_index = idx;
            }
        }

        if (json_get_int_range(obj_start, obj_end, "text_index", &v) == 0) {
            int idx = clamp_int(v, -1, 7);
            if (idx != asset->cfg.text_index) {
                asset->cfg.text_index = idx;
                text_change = 1;
            }
        }

        int indices_tmp[8] = {0};
        int idx_count = 0;
        if (find_key_range(obj_start, obj_end, "text_indices")) {
            json_get_int_array_range(obj_start, obj_end, "text_indices", indices_tmp, 8, &idx_count);
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

        if (json_get_int_range(obj_start, obj_end, "image_opacity", &v) == 0) {
            int opacity = clamp_int(v, 0, 100);
            if (opacity != asset->cfg.image_opacity_pct) {
                asset->cfg.image_opacity_pct = opacity;
                restyle = 1;
            }
        }

        if (json_get_string_range(obj_start, obj_end, "image_path", asset->cfg.image_path, sizeof(asset->cfg.image_path)) == 0) {
            recreate = 1;
        } else if (json_get_string_range(obj_start, obj_end, "source", asset->cfg.image_path, sizeof(asset->cfg.image_path)) == 0) {
            recreate = 1;
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
    if (asset->cfg.text_index >= 0 && asset->cfg.text_index < 8) {
        const char *t = udp_texts[asset->cfg.text_index];
        if (t[0] != '\0') return t;
        if (asset->cfg.label[0] != '\0') return asset->cfg.label;
        return "";
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
                int idx = clamp_int(asset->cfg.text_indices[i], 0, 7);
                const char *t = udp_texts[idx];
                if (t[0] == '\0') continue;
                if (written > 0 && written < buf_sz - 1) {
                    buf[written++] = asset->cfg.text_inline ? ' ' : '\n';
                }
                size_t len = strnlen(t, sizeof(udp_texts[0]));
                size_t to_copy = len < buf_sz - 1 - written ? len : buf_sz - 1 - written;
                memcpy(buf + written, t, to_copy);
                written += to_copy;
                if (written >= buf_sz - 1) break;
            }
        }

        if (written == 0 && asset->cfg.text_index >= 0 && asset->cfg.text_index < 8) {
            const char *t = udp_texts[asset->cfg.text_index];
            size_t len = strnlen(t, sizeof(udp_texts[0]));
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
    ssize_t last_r = -1;
    // Drain the socket to keep only the freshest payload
    while ((r = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, NULL, NULL)) > 0) {
        last_r = r;
    }
    if (last_r > 0) {
        buf[last_r] = '\0';
        parse_udp_values(buf);
        parse_udp_texts(buf);
        parse_udp_asset_updates(buf);
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

    register_simple_fs_driver();

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

static lv_obj_t *create_image_asset(asset_t *asset)
{
    if (!asset) return NULL;
    if (asset->cfg.image_path[0] == '\0') return NULL;
    register_simple_fs_driver();
    const char *src = asset->cfg.image_path;
    if (asset->cfg.image_path[1] != ':' && asset->cfg.image_path[0] != '/') {
        snprintf(asset->image_src, sizeof(asset->image_src), "S:%s", asset->cfg.image_path);
        src = asset->image_src;
    }
    lv_obj_t *img = lv_image_create(lv_scr_act());
    lv_image_set_src(img, src);
    if (asset->cfg.width > 0 || asset->cfg.height > 0) {
        int width = asset->cfg.width > 0 ? asset->cfg.width : LV_SIZE_CONTENT;
        int height = asset->cfg.height > 0 ? asset->cfg.height : LV_SIZE_CONTENT;
        lv_obj_set_size(img, width, height);
    }
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, asset->cfg.x, asset->cfg.y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    return img;
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
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, asset->cfg.x, asset->cfg.y);
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
        case ASSET_IMAGE:
            asset->obj = create_image_asset(asset);
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

static void update_assets_from_udp(void)
{
    for (int i = 0; i < asset_count; i++) {
        if (!assets[i].cfg.enabled) continue;
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
                if (assets[i].obj && assets[i].last_pct != pct) {
                    lv_bar_set_value(assets[i].obj, pct, LV_ANIM_OFF);
                    assets[i].last_pct = pct;
                }
                break;
            case ASSET_TEXT: {
                if (assets[i].obj) {
                    char text_buf[128];
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
            char text_buf[128];
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
    clear_splashscreen();

    destroy_assets();
    load_config();

    idle_cap_ms = clamp_int(g_cfg.idle_ms, 10, 1000);
    idle_ms_applied = idle_cap_ms;

    create_assets();
    update_assets_from_udp();

    if (stats_label) {
        if (g_cfg.show_stats) {
            lv_obj_clear_flag(stats_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(stats_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    fps_start_ms = monotonic_ms64();
    fps_frames = 0;
    maybe_show_splashscreen();
}

static void cleanup_resources(void)
{
    clear_splashscreen();
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

static void splash_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (splash_timer) {
        lv_timer_del(splash_timer);
        splash_timer = NULL;
    }
    destroy_asset_visual(&splash_asset);
}

static void clear_splashscreen(void)
{
    if (splash_timer) {
        lv_timer_del(splash_timer);
        splash_timer = NULL;
    }
    destroy_asset_visual(&splash_asset);
}

static void maybe_show_splashscreen(void)
{
    clear_splashscreen();
    if (!g_splash_cfg.enabled || g_splash_cfg.duration_ms <= 0) return;
    splash_asset.cfg = g_splash_cfg.cfg;
    create_asset_visual(&splash_asset);
    if (splash_asset.obj) {
        lv_obj_move_foreground(splash_asset.obj);
        splash_timer = lv_timer_create(splash_timer_cb, g_splash_cfg.duration_ms, NULL);
    }
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
    char buf[512];
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
    lv_obj_align(stats_label, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_text(stats_label, "OSD stats");

    // Timers (throttled to ~10 Hz)
    stats_timer = lv_timer_create(stats_timer_cb, 250, NULL);

    update_assets_from_udp();

    maybe_show_splashscreen();

    idle_cap_ms = clamp_int(g_cfg.idle_ms, 10, 1000);
    idle_ms_applied = idle_cap_ms;

    // Main loop paced by a simple UDP poll cap
    while (!stop_requested) {
        if (reload_requested) {
            reload_requested = 0;
            reload_config_runtime();
        }

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
