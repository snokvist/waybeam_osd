#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include "lvgl/lvgl.h"
#include "lvgl/demos/widgets/lv_demo_widgets.h"
#include "lvgl/demos/music/lv_demo_music.h"
#include "lvgl/demos/render/lv_demo_render.h"

#include "mi_sys.h"
#include "mi_rgn.h"

#define OSD_WIDTH 1920
#define OSD_HEIGHT 1080
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
    lv_obj_t *main_child = lv_obj_get_child(lv_scr_act(), 0);
    
    // Update position
    child_x += child_dx;
    child_y += child_dy;
    
    // Get current child size
    int current_width = lv_obj_get_width(main_child);
    int current_height = lv_obj_get_height(main_child);
    
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
    lv_obj_set_pos(main_child, child_x, child_y);
}



// -------------------------
// Main
// -------------------------
int main(void)
{
    printf("Initializing OSD region...\n");
    mi_region_init();

    printf("Initializing LVGL...\n");
    init_lvgl();


    printf("Running LVGL demo...\n");

    lv_demo_render(LV_DEMO_RENDER_SCENE_FILL, LV_OPA_COVER);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_TRANSP, LV_PART_MAIN);  // 50% opacity

    // Get the first child of the screen (main child from demo)
    lv_obj_t *main_child = lv_obj_get_child(lv_scr_act(), 0);
    lv_obj_set_style_bg_opa(main_child, LV_OPA_TRANSP, LV_PART_MAIN);

    // Animation variables for main_child
    static int child_x = 100, child_y = 100;
    static int child_dx = 3, child_dy = 2;

    // Set initial position
    lv_obj_set_pos(main_child, child_x, child_y);

    // Create animation timer
    lv_timer_t *bounce_timer = lv_timer_create(bounce_callback, 16, NULL);

    // Main loop
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }

    lv_timer_del(bounce_timer);

    return 0;
}
