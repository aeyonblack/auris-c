#include "auris_display.h"
#include <errno.h>
#include <string.h>
int auris_display_pack(const auris_rgb24_t *s, uint32_t *dst,
                       uint32_t w, uint32_t h, size_t pitch,
                       size_t capacity, unsigned int rotation) {
  if (!s || !s->data || !dst || (uintptr_t)dst % _Alignof(uint32_t) || !s->width || !s->height ||
      s->width > 4096U || s->height > 4096U || !w || !h ||
      w > 8192U || h > 8192U || rotation > 270U || rotation % 90U)
    return -EINVAL;
  const size_t row = (size_t)s->width * 3U, dest_row = (size_t)w * 4U;
  if (s->stride < row || s->bytes < row ||
      (size_t)(s->height-1U) > (s->bytes-row)/s->stride ||
      pitch < dest_row || pitch % 4U || capacity < dest_row ||
      (size_t)(h-1U) > (capacity-dest_row)/pitch) return -EINVAL;
  const bool turn = rotation == 90U || rotation == 270U;
  const uint32_t rw = turn ? s->height : s->width;
  const uint32_t rh = turn ? s->width : s->height;
  uint32_t ox=0U, oy=0U, fw=0U, fh=0U;
  if (auris_display_fit(rw,rh,w,h,&ox,&oy,&fw,&fh)) return -EINVAL;
  /* Source column of every output column, worked out once per call (a
     division per pixel cost more than the copy itself). */
  uint32_t columns[8192];
  for (uint32_t x=0U; x<fw; ++x)
    columns[x]=fw==rw ? x : (uint32_t)(((uint64_t)x*2U+1U)*rw/(2U*fw));
  for (uint32_t y=0U; y<h; ++y) {
    uint32_t *line=dst+(size_t)y*(pitch/4U);
    if (y<oy || y>=oy+fh) { memset(line, 0, dest_row); continue; }
    if (ox) memset(line, 0, (size_t)ox*4U);
    if (ox+fw<w) memset(line+ox+fw, 0, (size_t)(w-ox-fw)*4U);
    const uint32_t ry=fh==rh ? y-oy : (uint32_t)(((uint64_t)(y-oy)*2U+1U)*rh/(2U*fh));
    for (uint32_t x=0U; x<fw; ++x) {
      const uint32_t rx=columns[x];
      uint32_t sx=rx, sy=ry;
      if (rotation==90U) { sx=ry; sy=s->height-1U-rx; }
      else if (rotation==180U) { sx=s->width-1U-rx; sy=s->height-1U-ry; }
      else if (rotation==270U) { sx=s->width-1U-ry; sy=rx; }
      const uint8_t *p=s->data+(size_t)sy*s->stride+(size_t)sx*3U;
      line[ox+x]=((uint32_t)p[0]<<16U)|((uint32_t)p[1]<<8U)|p[2];
    }
  }
  return 0;
}

int auris_display_fit(uint32_t image_w, uint32_t image_h, uint32_t panel_w,
                      uint32_t panel_h, uint32_t *x, uint32_t *y, uint32_t *w,
                      uint32_t *h) {
  if (!x || !y || !w || !h || !image_w || !image_h || !panel_w || !panel_h ||
      image_w > 8192U || image_h > 8192U || panel_w > 8192U || panel_h > 8192U)
    return -EINVAL;
  uint32_t fw=panel_w, fh=panel_h;
  if ((uint64_t)panel_w*image_h <= (uint64_t)panel_h*image_w)
    fh=(uint32_t)((uint64_t)panel_w*image_h/image_w);
  else fw=(uint32_t)((uint64_t)panel_h*image_w/image_h);
  if (!fw) fw=1U;
  if (!fh) fh=1U;
  *x=(panel_w-fw)/2U; *y=(panel_h-fh)/2U; *w=fw; *h=fh;
  return 0;
}
