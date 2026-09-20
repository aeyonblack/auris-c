#include "auris_image.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t lo, hi;
  float weight;
} axis_t;
struct auris_image {
  uint32_t sw, sh, width, height;
  auris_image_rect_t content;
  axis_t *xmap, *ymap;
  uint16_t *linear;
  uint16_t correction[3][256];
  uint8_t encoding[4096];
};

static float clamp(float x, float upper) {
  return x < 0.0f ? 0.0f : (x > upper ? upper : x);
}

static void axis_init(axis_t *map, uint32_t count, uint32_t source) {
  for (uint32_t i = 0U; i < count; ++i) {
    const float p =
        clamp(((float)i + 0.5f) * (float)source / (float)count - 0.5f,
              (float)(source - 1U));
    const uint32_t lo = (uint32_t)p;
    map[i] = (axis_t){lo, lo + (lo + 1U < source ? 1U : 0U), p - (float)lo};
  }
}

void auris_image_close(auris_image_t *s) {
  if (s == NULL)
    return;
  free(s->linear);
  free(s->xmap);
  free(s->ymap);
  free(s);
}

int auris_image_open(auris_image_t **out, const auris_image_config_t *c) {
  if (out == NULL || *out != NULL || c == NULL || c->source_width < 2U ||
      c->source_height < 2U || c->source_width > 4096U ||
      c->source_height > 4096U || c->width == 0U || c->height == 0U ||
      c->width > 4096U || c->height > 4096U || !isfinite(c->black) ||
      c->black < 0.0f || c->black >= 255.0f || !isfinite(c->exposure_ev) ||
      fabsf(c->exposure_ev) > 8.0f || !isfinite(c->gamma) || c->gamma < 0.1f ||
      c->gamma > 5.0f)
    return -EINVAL;
  for (size_t k = 0U; k < 3U; ++k)
    if (!isfinite(c->gain[k]) || c->gain[k] <= 0.0f || c->gain[k] > 16.0f)
      return -EINVAL;
  auris_image_t *s = calloc(1U, sizeof(*s));
  if (s == NULL)
    return -ENOMEM;
  s->sw = c->source_width;
  s->sh = c->source_height;
  s->width = c->width;
  s->height = c->height;
  uint32_t w = c->width, h = c->height;
  if ((uint64_t)c->width * s->sh <= (uint64_t)c->height * s->sw)
    h = (uint32_t)(((uint64_t)c->width * s->sh + s->sw / 2U) / s->sw);
  else
    w = (uint32_t)(((uint64_t)c->height * s->sw + s->sh / 2U) / s->sh);
  if (w == 0U)
    w = 1U;
  if (h == 0U)
    h = 1U;
  s->content =
      (auris_image_rect_t){(c->width - w) / 2U, (c->height - h) / 2U, w, h};
  s->linear = malloc((size_t)s->sw * s->sh * 3U * sizeof(*s->linear));
  s->xmap = malloc((size_t)w * sizeof(*s->xmap));
  s->ymap = malloc((size_t)h * sizeof(*s->ymap));
  if (s->linear == NULL || s->xmap == NULL || s->ymap == NULL) {
    auris_image_close(s);
    return -ENOMEM;
  }
  axis_init(s->xmap, w, s->sw);
  axis_init(s->ymap, h, s->sh);
  const float exposure = exp2f(c->exposure_ev);
  for (size_t k = 0U; k < 3U; ++k)
    for (size_t i = 0U; i < 256U; ++i) {
      const float value =
          ((float)i - c->black) / (255.0f - c->black) * c->gain[k] * exposure;
      s->correction[k][i] = (uint16_t)(4095.0f * clamp(value, 1.0f) + 0.5f);
    }
  for (size_t i = 0U; i < 4096U; ++i)
    s->encoding[i] =
        (uint8_t)(255.0f * powf((float)i / 4095.0f, 1.0f / c->gamma) + 0.5f);
  *out = s;
  return 0;
}

const auris_image_rect_t *auris_image_content(const auris_image_t *s) {
  return s == NULL ? NULL : &s->content;
}

static bool fits(uint32_t w, uint32_t h, size_t stride, size_t bytes,
                 size_t bpp) {
  const size_t row = (size_t)w * bpp;
  return stride >= row && bytes >= row &&
         (size_t)(h - 1U) <= (bytes - row) / stride;
}

static void demosaic(auris_image_t *s, const auris_bayer8_t *in) {
  for (uint32_t y = 0U; y < s->sh; ++y) {
    const uint32_t up = y == 0U ? 1U : y - 1U;
    const uint32_t down = y + 1U == s->sh ? y - 1U : y + 1U;
    const uint8_t *a = in->data + (size_t)up * in->stride;
    const uint8_t *b = in->data + (size_t)y * in->stride;
    const uint8_t *c = in->data + (size_t)down * in->stride;
    uint16_t *dst = s->linear + (size_t)y * s->sw * 3U;
    for (uint32_t x = 0U; x < s->sw; ++x) {
      const uint32_t left = x == 0U ? 1U : x - 1U;
      const uint32_t right = x + 1U == s->sw ? x - 1U : x + 1U;
      const unsigned int horizontal =
          ((unsigned int)b[left] + b[right] + 1U) / 2U;
      const unsigned int vertical = ((unsigned int)a[x] + c[x] + 1U) / 2U;
      unsigned int r, g, blue;
      if ((x & 1U) == (y & 1U)) {
        const unsigned int diagonal =
            ((unsigned int)a[left] + a[right] + c[left] + c[right] + 2U) / 4U;
        g = ((unsigned int)b[left] + b[right] + a[x] + c[x] + 2U) / 4U;
        r = (y & 1U) == 0U ? b[x] : diagonal;
        blue = (y & 1U) == 0U ? diagonal : b[x];
      } else {
        g = b[x];
        r = (y & 1U) == 0U ? horizontal : vertical;
        blue = (y & 1U) == 0U ? vertical : horizontal;
      }
      *dst++ = s->correction[0][r];
      *dst++ = s->correction[1][g];
      *dst++ = s->correction[2][blue];
    }
  }
}

static float mix(float a, float b, float weight) {
  return a + weight * (b - a);
}

int auris_image_render(auris_image_t *s, const auris_bayer8_t *in,
                       auris_rgb24_t *out) {
  if (s == NULL || in == NULL || out == NULL || in->data == NULL ||
      out->data == NULL || in->width != s->sw || in->height != s->sh ||
      out->width != s->width || out->height != s->height ||
      !fits(in->width, in->height, in->stride, in->bytes, 1U) ||
      !fits(out->width, out->height, out->stride, out->bytes, 3U))
    return -EINVAL;
  demosaic(s, in);
  for (uint32_t y = 0U; y < out->height; ++y)
    memset(out->data + (size_t)y * out->stride, 0, (size_t)out->width * 3U);
  for (uint32_t y = 0U; y < s->content.height; ++y) {
    const axis_t v = s->ymap[y];
    const uint16_t *top = s->linear + (size_t)v.lo * s->sw * 3U;
    const uint16_t *bottom = s->linear + (size_t)v.hi * s->sw * 3U;
    uint8_t *dst = out->data + (size_t)(s->content.y + y) * out->stride +
                   (size_t)s->content.x * 3U;
    for (uint32_t x = 0U; x < s->content.width; ++x) {
      const axis_t u = s->xmap[x];
      for (size_t k = 0U; k < 3U; ++k) {
        const size_t l = (size_t)u.lo * 3U + k, r = (size_t)u.hi * 3U + k;
        const float t = mix((float)top[l], (float)top[r], u.weight);
        const float b = mix((float)bottom[l], (float)bottom[r], u.weight);
        const size_t level =
            (size_t)(clamp(mix(t, b, v.weight), 4095.0f) + 0.5f);
        *dst++ = s->encoding[level];
      }
    }
  }
  return 0;
}
