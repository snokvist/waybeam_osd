#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_image_decoder.h"
#include "lvgl/src/draw/lv_image_decoder_private.h"
#include <stdio.h>
void test(void) {
    lv_image_decoder_dsc_t dsc;
    dsc.decoded = NULL;
    printf("%p", dsc.decoded);
}
