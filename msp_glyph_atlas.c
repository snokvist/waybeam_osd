#include "msp_glyph_atlas.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl/src/libs/lodepng/lodepng.h"

#define DEFAULT_GRID_COLS 16
#define DEFAULT_GRID_ROWS 16
static glyph_layout_t layout_from_png(unsigned width, unsigned height, const glyph_layout_t *layout_override)
{
    glyph_layout_t layout = {
        .grid_cols = DEFAULT_GRID_COLS,
        .grid_rows = DEFAULT_GRID_ROWS,
        .page_count = 0,
        .glyph_w = 0,
        .glyph_h = 0,
    };

    if (layout_override) {
        if (layout_override->grid_cols > 0) layout.grid_cols = layout_override->grid_cols;
        if (layout_override->grid_rows > 0) layout.grid_rows = layout_override->grid_rows;
        if (layout_override->page_count > 0) layout.page_count = layout_override->page_count;
        if (layout_override->glyph_w > 0) layout.glyph_w = layout_override->glyph_w;
        if (layout_override->glyph_h > 0) layout.glyph_h = layout_override->glyph_h;
    }

    if (layout.glyph_w <= 0 && layout.grid_cols > 0) {
        layout.glyph_w = (int)width / layout.grid_cols;
    }

    if (layout.page_count <= 0) {
        if (layout.glyph_w > 0 && layout.grid_rows > 0) {
            int page_height = layout.glyph_w * layout.grid_rows;
            if (page_height > 0) {
                layout.page_count = (int)height / page_height;
            }
        }
        if (layout.page_count <= 0) layout.page_count = 1;
    }

    if (layout.glyph_h <= 0 && layout.grid_rows > 0 && layout.page_count > 0) {
        layout.glyph_h = (int)height / (layout.grid_rows * layout.page_count);
    }

    return layout;
}

static void blend_pixel(uint32_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a == 0) return;
    if (a == 255) {
        *dst = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        return;
    }

    uint32_t d = *dst;
    uint8_t da = (d >> 24) & 0xFF;
    uint8_t dr = (d >> 16) & 0xFF;
    uint8_t dg = (d >> 8) & 0xFF;
    uint8_t db = d & 0xFF;

    uint32_t inv = 255 - a;
    uint8_t out_a = (uint8_t)((a + (da * inv + 127) / 255));
    uint8_t out_r = (uint8_t)((r * a + dr * inv + 127) / 255);
    uint8_t out_g = (uint8_t)((g * a + dg * inv + 127) / 255);
    uint8_t out_b = (uint8_t)((b * a + db * inv + 127) / 255);

    *dst = ((uint32_t)out_a << 24) | ((uint32_t)out_r << 16) | ((uint32_t)out_g << 8) | out_b;
}

int glyph_atlas_load_png(glyph_atlas_t *atlas, const char *path, const glyph_layout_t *layout_override)
{
    if (!atlas || !path) return -1;
    memset(atlas, 0, sizeof(*atlas));

    unsigned width = 0;
    unsigned height = 0;
    uint8_t *rgba = NULL;
    unsigned error = lodepng_decode32_file(&rgba, &width, &height, path);
    if (error != 0 || !rgba || width == 0 || height == 0) {
        free(rgba);
        return -1;
    }

    atlas->width = width;
    atlas->height = height;
    atlas->rgba = rgba;
    atlas->layout = layout_from_png(width, height, layout_override);
    return 0;
}

void glyph_atlas_release(glyph_atlas_t *atlas)
{
    if (!atlas) return;
    free(atlas->rgba);
    atlas->rgba = NULL;
    atlas->width = 0;
    atlas->height = 0;
    memset(&atlas->layout, 0, sizeof(atlas->layout));
}

int glyph_atlas_rect_for_id(const glyph_atlas_t *atlas, int glyph_id, glyph_rect_t *out_rect)
{
    if (!atlas || !out_rect) return -1;
    const glyph_layout_t *layout = &atlas->layout;
    if (layout->grid_cols <= 0 || layout->grid_rows <= 0) return -1;
    if (layout->glyph_w <= 0 || layout->glyph_h <= 0) return -1;

    int per_page = layout->grid_cols * layout->grid_rows;
    if (per_page <= 0) return -1;

    int page = glyph_id / per_page;
    int tile_index = glyph_id % per_page;
    if (tile_index < 0) tile_index += per_page;
    int tile_x = tile_index % layout->grid_cols;
    int tile_y = tile_index / layout->grid_cols;

    int x = tile_x * layout->glyph_w;
    int y = (page * layout->grid_rows + tile_y) * layout->glyph_h;

    if (x < 0 || y < 0) return -1;
    if (x + layout->glyph_w > (int)atlas->width) return -1;
    if (y + layout->glyph_h > (int)atlas->height) return -1;

    out_rect->x = x;
    out_rect->y = y;
    out_rect->w = layout->glyph_w;
    out_rect->h = layout->glyph_h;
    return 0;
}

int glyph_blit_glyph(const glyph_atlas_t *atlas, int glyph_id, int grid_row, int grid_col,
                     uint32_t *dst, int dst_w, int dst_h, int dst_stride,
                     int origin_x, int origin_y)
{
    if (!atlas || !atlas->rgba || !dst) return -1;
    if (dst_w <= 0 || dst_h <= 0 || dst_stride <= 0) return -1;

    glyph_rect_t rect;
    if (glyph_atlas_rect_for_id(atlas, glyph_id, &rect) != 0) return -1;

    int dst_x = origin_x + grid_col * rect.w;
    int dst_y = origin_y + grid_row * rect.h;

    if (dst_x >= dst_w || dst_y >= dst_h) return 0;
    if (dst_x + rect.w <= 0 || dst_y + rect.h <= 0) return 0;

    int src_x = rect.x;
    int src_y = rect.y;
    int copy_w = rect.w;
    int copy_h = rect.h;

    if (dst_x < 0) {
        int skip = -dst_x;
        src_x += skip;
        copy_w -= skip;
        dst_x = 0;
    }
    if (dst_y < 0) {
        int skip = -dst_y;
        src_y += skip;
        copy_h -= skip;
        dst_y = 0;
    }
    if (dst_x + copy_w > dst_w) copy_w = dst_w - dst_x;
    if (dst_y + copy_h > dst_h) copy_h = dst_h - dst_y;

    if (copy_w <= 0 || copy_h <= 0) return 0;

    const uint8_t *src_base = atlas->rgba;
    int src_stride = (int)atlas->width * 4;

    for (int row = 0; row < copy_h; row++) {
        const uint8_t *src_row = src_base + (src_y + row) * src_stride + src_x * 4;
        uint32_t *dst_row = dst + (dst_y + row) * dst_stride + dst_x;
        for (int col = 0; col < copy_w; col++) {
            uint8_t r = src_row[col * 4 + 0];
            uint8_t g = src_row[col * 4 + 1];
            uint8_t b = src_row[col * 4 + 2];
            uint8_t a = src_row[col * 4 + 3];
            blend_pixel(&dst_row[col], r, g, b, a);
        }
    }

    return 0;
}
