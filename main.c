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
#include <limits.h>
#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_private.h"
#include "lvgl/src/draw/lv_image_decoder_private.h"
#include <dirent.h>
#include "mi_sys.h"
#include "mi_rgn.h"
#include "mi_vpe.h"

#define DEFAULT_SCREEN_WIDTH 1280   // fallback resolution if config is absent
#define DEFAULT_SCREEN_HEIGHT 420
#define BUF_ROWS 60  // partial buffer height
#define CONFIG_PATH "/etc/waybeam_osd.json"
#define UDP_PORT 7777
#define UDP_MAX_PACKET 1280
#define MAX_ASSETS 8
#define UDP_TEXT_LEN 96
// LVGL buffers - allocated at runtime for ARGB8888 (32-bit per pixel)
static lv_color32_t *buf1 = NULL;
static lv_color32_t *buf2 = NULL;

#if LV_USE_FS_STDIO
void lv_fs_stdio_init(void);
#endif

#if LV_USE_LODEPNG
void lv_lodepng_init(void);
#else
void lv_lodepng_opt_init(void);
#endif
typedef struct {
    uint8_t *data;
    size_t size;
} anim_frame_t;
typedef enum {
    ASSET_BAR = 0,
    ASSET_TEXT,
    ASSET_IMAGE,
    ASSET_ANIMATION,
} asset_type_t;

typedef enum {
    ORIENTATION_RIGHT = 0,
    ORIENTATION_LEFT,
} asset_orientation_t;

