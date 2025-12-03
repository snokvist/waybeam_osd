#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>

#include "lvgl/lvgl.h"
/*
 * git clone -b ARGB4444 --recurse-submodules https://github.com/henkwiedig/lvgltest.git
#include "lvgl/demos/widgets/lv_demo_widgets.h"
#include "lvgl/demos/music/lv_demo_music.h"
#include "lvgl/demos/render/lv_demo_render.h"
*/
#include "mi_sys.h"
#include "mi_rgn.h"

#define SCREEN_WIDTH 1280   // Set to your panel resolution
#define SCREEN_HEIGHT 720
#define OSD_WIDTH SCREEN_WIDTH
#define OSD_HEIGHT SCREEN_HEIGHT
#define BUF_ROWS 60  // partial buffer height

// 2 LVGL buffers - now for ARGB8888 (32-bit per pixel)
static lv_color_t buf1[OSD_WIDTH * BUF_ROWS];
static lv_color_t buf2[OSD_WIDTH * BUF_ROWS];

// Sigmastar RGN
static MI_RGN_PaletteTable_t g_stPaletteTable = {};
static MI_RGN_HANDLE hRgnHandle;
static MI_RGN_ChnPort_t stVpeChnPort;
static MI_RGN_Attr_t stRgnAttr;
static MI_RGN_ChnPortParam_t stRgnChnAttr;

// Animation variables
static int child_x = 100, child_y = 100;
static int child_dx = 3, child_dy = 2;
static lv_obj_t *animated_obj = NULL;

// Stats/OSD label
static lv_obj_t *stats_label = NULL;
static uint32_t frame_counter = 0;
static uint32_t last_fps_time = 0;
static uint32_t last_frame_ms = 0;
static uint32_t fps_value = 0;
static volatile sig_atomic_t stop_requested = 0;
static lv_timer_t *bounce_timer = NULL;
static lv_timer_t *stats_timer = NULL;

// -------------------------
// LVGL tick function
// -------------------------
uint32_t my_get_milliseconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
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
    stRgnAttr.stOsdInitParam.stSize.u32Width = OSD_WIDTH;
    stRgnAttr.stOsdInitParam.stSize.u32Height = OSD_HEIGHT;

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

    lv_display_t * disp = lv_display_create(OSD_WIDTH, OSD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);  // Changed to LV_COLOR_FORMAT_ARGB8888
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_flush_cb);
}


// Animation callback function
void bounce_callback(lv_timer_t *timer) {
    if (!animated_obj) {
        return;
    }
    
    // Update position
    child_x += child_dx;
    child_y += child_dy;
    
    // Get current child size
    int current_width = lv_obj_get_width(animated_obj);
    int current_height = lv_obj_get_height(animated_obj);
    
    // Get screen dimensions
    int screen_width = lv_disp_get_hor_res(NULL);
    int screen_height = lv_disp_get_ver_res(NULL);
    
    // Bounce off screen edges
    if (child_x <= 0) {
        child_x = 0;
        child_dx = -child_dx;
    } else if (child_x + current_width >= screen_width) {
        child_x = screen_width - current_width;
        child_dx = -child_dx;
    }
    
    if (child_y <= 0) {
        child_y = 0;
        child_dy = -child_dy;
    } else if (child_y + current_height >= screen_height) {
        child_y = screen_height - current_height;
        child_dy = -child_dy;
    }
    
    // Update position
    lv_obj_set_pos(animated_obj, child_x, child_y);
}

static void handle_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void cleanup_resources(void)
{
    if (bounce_timer) {
        lv_timer_del(bounce_timer);
        bounce_timer = NULL;
    }
    if (stats_timer) {
        lv_timer_del(stats_timer);
        stats_timer = NULL;
    }

    // Tear down OSD region cleanly
    MI_RGN_DetachFromChn(hRgnHandle, &stVpeChnPort);
    MI_RGN_Destroy(hRgnHandle);
}

static void stats_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint32_t now = my_get_milliseconds();
    if (last_fps_time == 0) {
        last_fps_time = now;
    }

    uint32_t elapsed = now - last_fps_time;
    if (elapsed >= 500) {
        fps_value = (uint32_t)((frame_counter * 1000U) / (elapsed ? elapsed : 1));
        frame_counter = 0;
        last_fps_time = now;
    }

    int current_width = animated_obj ? lv_obj_get_width(animated_obj) : 0;
    int current_height = animated_obj ? lv_obj_get_height(animated_obj) : 0;

    char buf[160];
    lv_snprintf(buf, sizeof(buf),
                "OSD %dx%d, screen %dx%d\n"
                "Obj pos %d,%d size %d,%d\n"
                "FPS %u, frame %ums",
                OSD_WIDTH, OSD_HEIGHT,
                lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL),
                child_x, child_y, current_width, current_height,
                fps_value, last_frame_ms);
    if (stats_label) {
        lv_label_set_text(stats_label, buf);
    }
}



// -------------------------
// Main
// -------------------------
int main(void)
{
    signal(SIGINT, handle_sigint);

    printf("Initializing OSD region...\n");
    mi_region_init();

    printf("Initializing LVGL...\n");
    init_lvgl();


    printf("Running LVGL demo...\n");

    // Transparent screen
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_TRANSP, LV_PART_MAIN);

    // Simple animated object (small blue box)
    animated_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(animated_obj, 160, 96);
    lv_obj_set_style_bg_color(animated_obj, lv_color_hex(0x2266CC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(animated_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(animated_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(animated_obj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(animated_obj, child_x, child_y);

    // Static reference graphics
    lv_obj_t *indicator_a = lv_obj_create(lv_scr_act());
    lv_obj_set_size(indicator_a, 32, 32);
    lv_obj_set_style_bg_color(indicator_a, lv_color_hex(0x2ECC71), LV_PART_MAIN); // green
    lv_obj_set_style_bg_opa(indicator_a, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(indicator_a, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(indicator_a, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(indicator_a, LV_ALIGN_TOP_RIGHT, -12, 12);

    lv_obj_t *indicator_b = lv_obj_create(lv_scr_act());
    lv_obj_set_size(indicator_b, 48, 16);
    lv_obj_set_style_bg_color(indicator_b, lv_color_hex(0xE67E22), LV_PART_MAIN); // orange
    lv_obj_set_style_bg_opa(indicator_b, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(indicator_b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(indicator_b, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(indicator_b, LV_ALIGN_BOTTOM_LEFT, 12, -12);

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
    bounce_timer = lv_timer_create(bounce_callback, 100, NULL);
    stats_timer = lv_timer_create(stats_timer_cb, 250, NULL);

    // Main loop at ~10 Hz
    while (!stop_requested) {
        uint32_t loop_start = my_get_milliseconds();
        lv_timer_handler();
        frame_counter++;
        last_frame_ms = my_get_milliseconds() - loop_start;
        usleep(100000); // 100 ms
    }

    cleanup_resources();

    return 0;
}
