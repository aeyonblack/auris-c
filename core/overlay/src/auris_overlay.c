#include "auris_overlay.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>

#define MAX_POINTS 4096U
#define NO_CELL UINT32_MAX

typedef struct {
  uint32_t cell;
  float tx, ty;
} sample_t;

struct auris_overlay {
  auris_overlay_config_t config;
  sample_t *samples;
  auris_projected_point_t *points;
  uint8_t palette[256][4];
};

void auris_overlay_close(auris_overlay_t *s) {
  if (s == NULL) return;
  free(s->samples);
  free(s->points);
  free(s);
}

int auris_overlay_open(auris_overlay_t **out,
                       const auris_overlay_config_t *c) {
  if (out == NULL || *out != NULL || c == NULL) return -EINVAL;
  const auris_projection_layout_t *l = &c->layout;
  if (c->columns < 2U || c->rows < 2U ||
      c->columns > MAX_POINTS / c->rows ||
      l->canvas_width == 0U || l->canvas_width > 4096U ||
      l->canvas_height == 0U || l->canvas_height > 4096U ||
      l->content_width == 0U || l->content_height == 0U ||
      l->content_x >= l->canvas_width || l->content_y >= l->canvas_height ||
      l->content_width > l->canvas_width - l->content_x ||
      l->content_height > l->canvas_height - l->content_y ||
      !isfinite(c->cutoff) || c->cutoff < 0.0f || c->cutoff >= 1.0f ||
      !isfinite(c->opacity) || c->opacity < 0.0f || c->opacity > 1.0f)
    return -EINVAL;
  const size_t count = c->columns * c->rows;
  const size_t pixels = l->content_width * l->content_height;
  auris_overlay_t *s = calloc(1U, sizeof(*s));
  if (s == NULL) return -ENOMEM;
  s->config = *c;
  s->samples = malloc(pixels * sizeof(*s->samples));
  s->points = malloc(count * sizeof(*s->points));
  auris_vec3f_t *directions = malloc(count * sizeof(*directions));
  int err = -ENOMEM;
  if (s->samples == NULL || s->points == NULL || directions == NULL) goto fail;
  err = -EINVAL;
  if (auris_view_grid(&c->search, c->columns, c->rows, directions, count) !=
          AURIS_OK ||
      auris_projection_build(&c->camera, l, directions, count, s->points,
                              count) != AURIS_OK) goto fail;

  const float sx = (float)c->camera.width / (float)l->content_width;
  const float sy = (float)c->camera.height / (float)l->content_height;
  const float gx = (float)(c->columns - 1U) / (float)(c->search.width - 1U);
  const float gy = (float)(c->rows - 1U) / (float)(c->search.height - 1U);
  for (size_t y = 0U; y < l->content_height; ++y) {
    for (size_t x = 0U; x < l->content_width; ++x) {
      sample_t *p = &s->samples[y * l->content_width + x];
      *p = (sample_t){.cell = NO_CELL};
      /* Match the image resizer's pixel-centre sampling and edge clamp. */
      const float u = fminf((float)(c->camera.width - 1U),
                           fmaxf(0.0f, ((float)x + 0.5f) * sx - 0.5f));
      const float v = fminf((float)(c->camera.height - 1U),
                           fmaxf(0.0f, ((float)y + 0.5f) * sy - 0.5f));
      auris_vec3f_t ray;
      float a, b;
      bool visible;
      if (auris_view_ray(&c->camera, u, v, &ray) != AURIS_OK ||
          auris_view_project(&c->search, ray, &a, &b, &visible) != AURIS_OK)
        goto fail;
      if (!visible) continue;
      a = fminf((float)(c->columns - 1U), fmaxf(0.0f, a * gx));
      b = fminf((float)(c->rows - 1U), fmaxf(0.0f, b * gy));
      size_t col = (size_t)a, row = (size_t)b;
      if (col == c->columns - 1U) --col;
      if (row == c->rows - 1U) --row;
      *p = (sample_t){(uint32_t)(row * c->columns + col),
                       a - (float)col, b - (float)row};
    }
  }
  static const uint8_t anchors[5][3] = {
      {25, 87, 255}, {0, 217, 255}, {45, 255, 101},
      {255, 230, 0}, {255, 29, 22}};
  for (size_t i = 0U; i < 256U; ++i) {
    const float t = (float)i / 255.0f;
    const float position = t * 4.0f;
    size_t k = (size_t)position;
    if (k > 3U) k = 3U;
    const float fraction = position - (float)k;
    for (size_t ch = 0U; ch < 3U; ++ch) {
      s->palette[i][ch] = (uint8_t)lroundf(
          (1.0f - fraction) * (float)anchors[k][ch] +
          fraction * (float)anchors[k + 1U][ch]);
    }
    s->palette[i][3] = (uint8_t)lroundf(
        255.0f * c->opacity * (0.22f + 0.78f * t));
  }
  free(directions);
  *out = s;
  return 0;
fail:
  free(directions);
  auris_overlay_close(s);
  return err;
}

int auris_overlay_apply(const auris_overlay_t *s, const float *scores,
                        size_t count, size_t best, bool valid,
                        auris_rgb24_t *frame) {
  if (s == NULL || frame == NULL || frame->data == NULL) return -EINVAL;
  const auris_overlay_config_t *c = &s->config;
  const auris_projection_layout_t *l = &c->layout;
  const size_t row_bytes = l->canvas_width * 3U;
  if (frame->width != l->canvas_width || frame->height != l->canvas_height ||
      frame->stride < row_bytes || frame->bytes < row_bytes ||
      (l->canvas_height - 1U) > (frame->bytes - row_bytes) / frame->stride)
    return -EINVAL;
  if (!valid || c->opacity == 0.0f) return 0;
  if (scores == NULL || count != c->columns * c->rows || best >= count)
    return -EINVAL;
  float low = scores[0], high = scores[0];
  for (size_t q = 0U; q < count; ++q) {
    if (!isfinite(scores[q])) return -EINVAL;
    low = fminf(low, scores[q]);
    high = fmaxf(high, scores[q]);
  }
  const double span = (double)high - (double)low;
  if (span <= 1.0e-6 * fmax(1.0, fmax(fabs((double)low), fabs((double)high))))
    return 0;
  float relative[MAX_POINTS];
  const double inverse_span = 1.0 / span;
  for (size_t q = 0U; q < count; ++q)
    relative[q] = (float)(((double)scores[q] - (double)low) * inverse_span);
  const float colour_scale = 255.0f / (1.0f - c->cutoff);
  for (size_t y = 0U; y < l->content_height; ++y) {
    uint8_t *row = frame->data + (l->content_y + y) * frame->stride +
                   l->content_x * 3U;
    for (size_t x = 0U; x < l->content_width; ++x) {
      const sample_t p = s->samples[y * l->content_width + x];
      if (p.cell == NO_CELL) continue;
      const size_t q = p.cell;
      const float top = relative[q] + p.tx * (relative[q + 1U] - relative[q]);
      const float bottom = relative[q + c->columns] + p.tx *
          (relative[q + c->columns + 1U] - relative[q + c->columns]);
      const float value = top + p.ty * (bottom - top);
      if (value < c->cutoff) continue;
      const size_t index = (size_t)lroundf(fminf(255.0f,
          fmaxf(0.0f, (value - c->cutoff) * colour_scale)));
      const uint8_t *colour = s->palette[index];
      const unsigned int alpha = colour[3];
      for (size_t ch = 0U; ch < 3U; ++ch) {
        const unsigned int mixed = alpha * colour[ch] +
            (255U - alpha) * row[x * 3U + ch];
        row[x * 3U + ch] = (uint8_t)((mixed + 127U) / 255U);
      }
    }
  }
  if (c->mark_peak && s->points[best].visible) {
    const long cx = lroundf(s->points[best].x);
    const long cy = lroundf(s->points[best].y);
    for (long d = -4; d <= 4; ++d) {
      for (size_t axis = 0U; axis < 2U; ++axis) {
        const long x = cx + (axis == 0U ? d : 0);
        const long y = cy + (axis == 1U ? d : 0);
        if (x < (long)l->content_x || y < (long)l->content_y ||
            x >= (long)(l->content_x + l->content_width) ||
            y >= (long)(l->content_y + l->content_height)) continue;
        uint8_t *pixel = frame->data + (size_t)y * frame->stride + (size_t)x * 3U;
        pixel[0] = 0U; pixel[1] = 255U; pixel[2] = 255U;
      }
    }
  }
  return 0;
}
