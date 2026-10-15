#include "auris_nv12.h"

#include <errno.h>

static bool valid(const auris_nv12_t *p) {
  return p != NULL && p->y != NULL && p->uv != NULL && p->width >= 2U &&
         p->height >= 2U && p->width % 2U == 0U && p->height % 2U == 0U &&
         p->y_stride >= p->width && p->uv_stride >= p->width;
}

/* Fixed-point BT.601 video range to full-range RGB, in 1/256 steps. */
static uint8_t level(int32_t v) {
  v += 128;
  if (v <= 0)
    return 0U;
  v >>= 8;
  return v > 255 ? 255U : (uint8_t)v;
}

int auris_nv12_to_rgb24(const auris_nv12_t *p, const auris_rgb24_t *canvas,
                        uint32_t x, uint32_t y) {
  if (!valid(p) || canvas == NULL || canvas->data == NULL ||
      x > canvas->width || p->width > canvas->width - x ||
      y > canvas->height || p->height > canvas->height - y ||
      canvas->stride < (size_t)canvas->width * 3U ||
      canvas->bytes < (size_t)(y + p->height - 1U) * canvas->stride +
                          (size_t)(x + p->width) * 3U)
    return -EINVAL;
  for (uint32_t row = 0U; row < p->height; ++row) {
    const uint8_t *luma = p->y + (size_t)row * p->y_stride;
    const uint8_t *chroma = p->uv + (size_t)(row / 2U) * p->uv_stride;
    uint8_t *out =
        canvas->data + (size_t)(y + row) * canvas->stride + (size_t)x * 3U;
    for (uint32_t col = 0U; col < p->width; col += 2U) {
      const int32_t d = (int32_t)chroma[col] - 128;
      const int32_t e = (int32_t)chroma[col + 1U] - 128;
      const int32_t red = 409 * e, green = -100 * d - 208 * e, blue = 516 * d;
      for (uint32_t k = 0U; k < 2U; ++k) {
        const int32_t c = 298 * ((int32_t)luma[col + k] - 16);
        out[0] = level(c + red);
        out[1] = level(c + green);
        out[2] = level(c + blue);
        out += 3;
      }
    }
  }
  return 0;
}

int auris_nv12_mean(const auris_nv12_t *p, float *mean) {
  if (!valid(p) || mean == NULL)
    return -EINVAL;
  uint64_t sum = 0U;
  for (uint32_t row = 0U; row < p->height; ++row) {
    const uint8_t *luma = p->y + (size_t)row * p->y_stride;
    for (uint32_t col = 0U; col < p->width; ++col) {
      const uint32_t v = luma[col];
      sum += v <= 16U ? 0U : v >= 235U ? 219U : v - 16U;
    }
  }
  *mean = (float)((double)sum / (219.0 * (double)p->width * p->height));
  return 0;
}
