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
  uint32_t fw=w, fh=h;
  if ((uint64_t)w*rh <= (uint64_t)h*rw) fh=(uint32_t)((uint64_t)w*rh/rw);
  else fw=(uint32_t)((uint64_t)h*rw/rh);
  if (!fw) fw=1U;
  if (!fh) fh=1U;
  const uint32_t ox=(w-fw)/2U, oy=(h-fh)/2U;
  for (uint32_t y=0U; y<h; ++y) {
    uint32_t *line=dst+(size_t)y*(pitch/4U);
    memset(line, 0, dest_row);
    if (y<oy || y>=oy+fh) continue;
    const uint32_t ry=(uint32_t)(((uint64_t)(y-oy)*2U+1U)*rh/(2U*fh));
    for (uint32_t x=0U; x<fw; ++x) {
      const uint32_t rx=(uint32_t)(((uint64_t)x*2U+1U)*rw/(2U*fw));
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
