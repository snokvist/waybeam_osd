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

#define OSD_WIDTH 800
#define OSD_HEIGHT 600
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
    // lv_demo_widgets();  // run default LVGL demo
    // lv_demo_music();  // run default LVGL music demo

    //lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0000), LV_PART_MAIN);
    

    lv_demo_render(LV_DEMO_RENDER_SCENE_FILL, LV_OPA_50);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_50, LV_PART_MAIN);  // 50% opacity
    lv_obj_set_style_bg_opa(lv_obj_get_child(lv_scr_act(),0), LV_OPA_50, LV_PART_MAIN);  // 50% opacity

    // // Set semi-transparent background
    // lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0000), LV_PART_MAIN);
    // lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_50, LV_PART_MAIN);  // 50% opacity

    // // Create a semi-transparent button
    // lv_obj_t *button = lv_button_create(lv_screen_active());
    // lv_obj_set_pos(button, 200, 200);
    // lv_obj_set_size(button, 100, 50);
    // lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_PART_MAIN);  // 70% opacity
    
    // // Add a label with some transparency
    // lv_obj_t *label = lv_label_create(button);
    // lv_label_set_text(label, "Alpha Test");
    // lv_obj_set_style_text_opa(label, LV_OPA_80, LV_PART_MAIN);  // 80% opacity
    // lv_obj_center(label);

    // Main loop
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
