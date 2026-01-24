#ifndef MSP_GLYPH_ATLAS_H
#define MSP_GLYPH_ATLAS_H

#include <stdint.h>

typedef struct {
    int grid_cols;
    int grid_rows;
    int page_count;
    int glyph_w;
    int glyph_h;
} glyph_layout_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} glyph_rect_t;

typedef struct {
    unsigned width;
    unsigned height;
    uint8_t *rgba;
    glyph_layout_t layout;
} glyph_atlas_t;

int glyph_atlas_load_png(glyph_atlas_t *atlas, const char *path, const glyph_layout_t *layout_override);
void glyph_atlas_release(glyph_atlas_t *atlas);

int glyph_atlas_rect_for_id(const glyph_atlas_t *atlas, int glyph_id, glyph_rect_t *out_rect);
int glyph_blit_glyph(const glyph_atlas_t *atlas, int glyph_id, int grid_row, int grid_col,
                     uint32_t *dst, int dst_w, int dst_h, int dst_stride,
                     int origin_x, int origin_y);

#endif
