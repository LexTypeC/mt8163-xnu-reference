#ifndef FIRE_SEED_COVERAGE_H
#define FIRE_SEED_COVERAGE_H
#include <stdint.h>
/* Board buffer layout, deliberately independent of the donor's dimensions. */
#define FSC_PANEL_WIDTH 800u
#define FSC_PANEL_HEIGHT 1280u
#define FSC_ROW_BYTES 3200u
#define FSC_FRAME_BYTES 4096000u
#define FSC_REPLACE_ROWS 1200u
#define FSC_REPLACE_BYTES 3840000u
/* Pure metadata predicate. The renderer retains the immutable surface and its
 * real read lease while planning and drawing. A positive result promises all
 * viewport bytes will be overwritten before cache preparation/publication.
 * Target geometry is independent of donor identity. Unsupported cases return
 * zero and retain the complete predecessor seed. */
struct fsc_view {
    uint32_t width,height,panel_width,panel_height,row_bytes;
    uint32_t x,y,viewport_width,viewport_height;
};
struct fsc_surface {
    uint32_t width,height,stride,offset,allocation,bytes_per_element,planes;
};
static inline unsigned fsc_overwritten_rows(const struct fsc_view *v,const struct fsc_surface *s,
    const int32_t src[4],const int32_t dst[4],unsigned previous_pixels_read,unsigned valid_pixels)
{
    if(!v || !s || !src || !dst || previous_pixels_read || !valid_pixels)return 0;
    if(!v->width || !v->height || v->width>INT32_MAX || v->height>INT32_MAX ||
       v->panel_width!=FSC_PANEL_WIDTH || v->panel_height!=FSC_PANEL_HEIGHT ||
       v->row_bytes!=FSC_ROW_BYTES || v->x || v->y ||
       v->viewport_width!=v->panel_width || !v->viewport_height ||
       v->viewport_height!=FSC_REPLACE_ROWS)return 0;
    if(s->width!=v->width || s->height!=v->height || s->bytes_per_element!=4 || s->planes ||
       (uint64_t)s->width*4>s->stride)return 0;
    if(src[0] || src[1] || src[2]!=(int32_t)s->width || src[3]!=(int32_t)s->height ||
       dst[0] || dst[1] || dst[2]!=(int32_t)v->width || dst[3]!=(int32_t)v->height)return 0;
    uint64_t last=(uint64_t)s->offset+(uint64_t)(s->height-1)*s->stride+(uint64_t)s->width*4;
    return last<=s->allocation?v->viewport_height:0;
}
/* The current Karnak buffered contract is800x1280/3200B. Only the exact
 * full-width1200-row viewport is admitted;other plans copy every byte. */
static inline unsigned fsc_seed_offset(unsigned covered_rows)
{
    return covered_rows==FSC_REPLACE_ROWS?FSC_REPLACE_BYTES:0;
}
#endif
