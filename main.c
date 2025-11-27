#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include "lvgl/lvgl.h"
#include "lvgl/demos/widgets/lv_demo_widgets.h"
#include "lvgl/demos/music/lv_demo_music.h"

#include "mi_sys.h"
#include "mi_rgn.h"

#define OSD_WIDTH 800
#define OSD_HEIGHT 600
#define BUF_ROWS 60  // partial buffer height

// 2 LVGL buffers
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
    uint16_t *src = (uint16_t *)px_map;

    for (int y = 0; y < h; y++) {
        uint16_t *dest = (uint16_t *)(info.virtAddr +
                                      (area->y1 + y) * info.u32Stride +
                                      area->x1 * 2);
        for (int x = 0; x < w; x++) {
            uint16_t c565 = src[y * w + x];
            uint8_t r = (c565 >> 11) & 0x1F;
            uint8_t g = (c565 >> 5) & 0x3F;
            uint8_t b = c565 & 0x1F;
            g >>= 1; // 6-bit → 5-bit
            dest[x] = 0x8000 | (r << 10) | (g << 5) | b;
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
    stRgnAttr.stOsdInitParam.ePixelFmt = E_MI_RGN_PIXEL_FORMAT_ARGB1555;
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
    stRgnChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8BgAlpha = 0;
    stRgnChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8FgAlpha = 255;

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
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_flush_cb);

    // Initialize default theme
    // lv_theme_t * th = lv_theme_default_init(disp);
    // lv_display_set_theme(disp, th);
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
    lv_demo_music();  // run default LVGL music demo

    // Main loop
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
