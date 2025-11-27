#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdint.h>

#include "lvgl/lvgl.h"
#include "mi_sys.h"
#include "mi_rgn.h"

#define OSD_WIDTH 800
#define OSD_HEIGHT 600

static lv_color_t buf1[OSD_WIDTH * 60];  // 60 rows buffer
static lv_color_t buf2[OSD_WIDTH * 60];

// Use the same palette table from the working code
MI_RGN_PaletteTable_t g_stPaletteTable = {};
MI_RGN_ChnPort_t stVpeChnPort;
MI_RGN_Attr_t stRgnAttr;
MI_RGN_ChnPortParam_t stRgnChnAttr;
MI_RGN_CanvasInfo_t stCanvasInfo;
MI_RGN_HANDLE hRgnHandle;


uint32_t my_get_milliseconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
}

// LVGL flush callback
void my_flush_cb(lv_display_t * disp,
                 const lv_area_t * area,
                 uint8_t * px_map)
{
    MI_RGN_CanvasInfo_t info;
    MI_RGN_GetCanvasInfo(hRgnHandle, &info);

    printf("Flush area: (%d,%d)-(%d,%d)\n",
           area->x1, area->y1, area->x2, area->y2);

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
            uint8_t b =  c565 & 0x1F;

            g >>= 1;

            uint16_t argb1555 = 0x8000 | (r << 10) | (g << 5) | b;

            dest[x] = argb1555;
        }
    }

    MI_RGN_UpdateCanvas(hRgnHandle);
    lv_display_flush_ready(disp);
}


void mi_region_init() {
    MI_S32 ret;
    
    // Initialize RGN with palette table
    ret = MI_RGN_Init(&g_stPaletteTable);
    printf("MI_RGN_Init returned: %d\n", ret);
    if (ret != MI_SUCCESS) {
        printf("MI_RGN_Init failed\n");
        return;
    }
    
    hRgnHandle = 0;

    // Create the OSD Region
    memset(&stRgnAttr, 0, sizeof(MI_RGN_Attr_t));
    stRgnAttr.eType = E_MI_RGN_TYPE_OSD;
    stRgnAttr.stOsdInitParam.ePixelFmt = E_MI_RGN_PIXEL_FORMAT_ARGB1555;
    stRgnAttr.stOsdInitParam.stSize.u32Width = OSD_WIDTH;
    stRgnAttr.stOsdInitParam.stSize.u32Height = OSD_HEIGHT;
    
    ret = MI_RGN_Create(hRgnHandle, &stRgnAttr);
    printf("MI_RGN_Create returned: %d\n", ret);
    if (ret != MI_SUCCESS) {
        printf("Failed to create RGN handle %d\n", hRgnHandle);
        return;
    }
    printf("Successfully created RGN handle %d\n", hRgnHandle);
    
    // Configure channel port
    stVpeChnPort.eModId = E_MI_RGN_MODID_VPE;
    stVpeChnPort.s32DevId = 0;
    stVpeChnPort.s32ChnId = 0;
    stVpeChnPort.s32OutputPortId = 0;
    
    memset(&stRgnChnAttr, 0, sizeof(MI_RGN_ChnPortParam_t));
    stRgnChnAttr.bShow = 1;  // Use 1 instead of TRUE
    stRgnChnAttr.stPoint.u32X = 100;
    stRgnChnAttr.stPoint.u32Y = 100;
    stRgnChnAttr.unPara.stOsdChnPort.u32Layer = 0;
    
    // Use alpha configuration
    stRgnChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode = E_MI_RGN_PIXEL_ALPHA;
    stRgnChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8BgAlpha = 0;
    stRgnChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8FgAlpha = 255;
    
    // Attach to channel 0
    ret = MI_RGN_AttachToChn(hRgnHandle, &stVpeChnPort, &stRgnChnAttr);
    printf("MI_RGN_AttachToChn (channel 0) returned: %d\n", ret);
    if (ret != MI_SUCCESS) {
        printf("Failed to attach RGN to channel 0\n");
    }
    
    printf("Region initialization completed\n");
}

// LVGL initialization
void init_lvgl(void)
{
    lv_init();

    static lv_display_t * disp;
    disp = lv_display_create(OSD_WIDTH, OSD_HEIGHT);
    if (!disp) {
        printf("Failed to create LVGL display\n");
        return;
    }

    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    // Add the tick callback
    lv_tick_set_cb(my_get_milliseconds);

}

int main(int argc, char **argv) {
    printf("Initializing OSD region...\n");
    mi_region_init();
    
    printf("Initializing LVGL...\n");
    init_lvgl();
    
    // Create LVGL UI
    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello Sigmastar SSC338Q!");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * cb = lv_checkbox_create(lv_screen_active());
    lv_obj_add_state(cb, LV_STATE_CHECKED);
    lv_obj_set_pos(cb, 200, 200);
    lv_checkbox_set_text(cb, "Test Checkbox");

    printf("Entering main loop...\n");
    
    // Main loop
    while(1) {
        lv_timer_handler();
        usleep(5000);
        
        static int counter = 0;
        if (counter++ % 60 == 0) {
            if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
                lv_obj_remove_state(cb, LV_STATE_CHECKED);
                printf("lv_obj_remove_state\n");
            } else {
                lv_obj_add_state(cb, LV_STATE_CHECKED);
                printf("lv_obj_add_state\n");
            }
        }
    }

    // Cleanup
    MI_RGN_DetachFromChn(hRgnHandle, &stVpeChnPort);
    MI_RGN_Destroy(hRgnHandle);
    MI_RGN_DeInit();
    
    return 0;
}