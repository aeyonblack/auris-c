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
  float black, base_gain[3], gain[3], base_exposure, exposure;
  float matrix[3][3];
  bool auto_white_balance, auto_exposure, adapted;
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

static void correction_init(auris_image_t *s) {
  for (size_t k = 0U; k < 3U; ++k)
    for (size_t i = 0U; i < 256U; ++i) {
      const float value = ((float)i - s->black) / (255.0f - s->black) *
                          s->gain[k] * s->exposure;
      s->correction[k][i] =
          (uint16_t)(4095.0f * clamp(value, 16.0f) + 0.5f);
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
  bool zero_matrix = true;
  for (size_t row = 0U; row < 3U; ++row)
    for (size_t column = 0U; column < 3U; ++column) {
      const float value = c->colour_matrix[row][column];
      if (!isfinite(value) || fabsf(value) > 8.0f)
        return -EINVAL;
      if (value != 0.0f)
        zero_matrix = false;
    }
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
  s->black = c->black;
  s->base_exposure = exp2f(c->exposure_ev);
  s->exposure = s->base_exposure;
  s->auto_white_balance = c->auto_white_balance;
  s->auto_exposure = c->auto_exposure;
  for (size_t k = 0U; k < 3U; ++k) {
    s->base_gain[k] = c->gain[k];
    s->gain[k] = c->gain[k];
    for (size_t j = 0U; j < 3U; ++j)
      s->matrix[k][j] = zero_matrix ? (k == j ? 1.0f : 0.0f)
                                    : c->colour_matrix[k][j];
  }
  correction_init(s);
  for (size_t i = 0U; i < 4096U; ++i)
    s->encoding[i] =
        (uint8_t)(255.0f * powf((float)i / 4095.0f, 1.0f / c->gamma) + 0.5f);
  *out = s;
  return 0;
}

const auris_image_rect_t *auris_image_content(const auris_image_t *s) {
  return s == NULL ? NULL : &s->content;
}

int auris_image_get_adjustment(const auris_image_t *s,
                               auris_image_adjustment_t *adjustment) {
  if (s == NULL || adjustment == NULL)
    return -EINVAL;
  memcpy(adjustment->gain, s->gain, sizeof(adjustment->gain));
  adjustment->exposure = s->exposure;
  return 0;
}

static bool fits(uint32_t w, uint32_t h, size_t stride, size_t bytes,
                 size_t bpp) {
  const size_t row = (size_t)w * bpp;
  return stride >= row && bytes >= row &&
         (size_t)(h - 1U) <= (bytes - row) / stride;
}

/* Estimate scene colour and brightness from a sparse set of complete RGGB
   cells. This is O(source pixels / 256), rather than another full-frame pass. */
static void adapt(auris_image_t *s, const auris_bayer8_t *in) {
  if (!s->auto_white_balance && !s->auto_exposure)
    return;
  uint64_t sum[3] = {0U, 0U, 0U};
  uint32_t histogram[256] = {0U};
  uint64_t count = 0U;
  uint32_t step = s->sw / 96U;
  if (step < 2U)
    step = 2U;
  if ((step & 1U) != 0U)
    ++step;
  for (uint32_t y = 0U; y + 1U < s->sh; y += step) {
    const uint8_t *top = in->data + (size_t)y * in->stride;
    const uint8_t *bottom = top + in->stride;
    for (uint32_t x = 0U; x + 1U < s->sw; x += step) {
      const unsigned int r = top[x];
      const unsigned int g = ((unsigned int)top[x + 1U] + bottom[x] + 1U) / 2U;
      const unsigned int b = bottom[x + 1U];
      ++histogram[g];
      if (r < 250U && g < 250U && b < 250U &&
          (float)r > s->black && (float)g > s->black &&
          (float)b > s->black) {
        sum[0] += r;
        sum[1] += g;
        sum[2] += b;
        ++count;
      }
    }
  }
  if (count == 0U)
    return;

  float target_gain[3];
  memcpy(target_gain, s->base_gain, sizeof(target_gain));
  if (s->auto_white_balance) {
    const float green = (float)sum[1] / (float)count - s->black;
    for (size_t k = 0U; k < 3U; k += 2U) {
      const float channel = (float)sum[k] / (float)count - s->black;
      target_gain[k] *= clamp(green / fmaxf(channel, 1.0f), 4.0f);
      if (target_gain[k] < 0.25f * s->base_gain[k])
        target_gain[k] = 0.25f * s->base_gain[k];
    }
  }

  float target_exposure = s->base_exposure;
  if (s->auto_exposure) {
    uint64_t samples = 0U;
    uint64_t green_sum = 0U;
    for (size_t i = 0U; i < 256U; ++i) {
      samples += histogram[i];
      if ((float)i > s->black)
        green_sum += (uint64_t)((float)i - s->black) * histogram[i];
    }
    uint64_t cumulative = 0U;
    size_t percentile = 255U;
    for (size_t i = 0U; i < 256U; ++i) {
      cumulative += histogram[i];
      if (cumulative * 100U >= samples * 95U) {
        percentile = i;
        break;
      }
    }
    const float green = (float)green_sum / (float)samples /
                        (255.0f - s->black) * s->base_gain[1];
    const float highlight = ((float)percentile - s->black) /
                            (255.0f - s->black) * s->base_gain[1];
    target_exposure = s->base_exposure * 0.18f / fmaxf(green, 1.0f / 255.0f);
    target_exposure = fminf(target_exposure,
                            s->base_exposure * 0.90f /
                                fmaxf(highlight, 1.0f / 255.0f));
    target_exposure = fmaxf(0.125f, fminf(target_exposure, 16.0f));
  }

  const float alpha = s->adapted ? 0.12f : 1.0f;
  for (size_t k = 0U; k < 3U; ++k)
    s->gain[k] += alpha * (target_gain[k] - s->gain[k]);
  s->exposure += alpha * (target_exposure - s->exposure);
  s->adapted = true;
  correction_init(s);
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
  adapt(s, in);
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
      float rgb[3];
      for (size_t k = 0U; k < 3U; ++k) {
        const size_t l = (size_t)u.lo * 3U + k, r = (size_t)u.hi * 3U + k;
        const float t = mix((float)top[l], (float)top[r], u.weight);
        const float b = mix((float)bottom[l], (float)bottom[r], u.weight);
        rgb[k] = mix(t, b, v.weight);
      }
      for (size_t k = 0U; k < 3U; ++k) {
        const float corrected = s->matrix[k][0] * rgb[0] +
                                s->matrix[k][1] * rgb[1] +
                                s->matrix[k][2] * rgb[2];
        const size_t level = (size_t)(clamp(corrected, 4095.0f) + 0.5f);
        *dst++ = s->encoding[level];
      }
    }
  }
  return 0;
}
