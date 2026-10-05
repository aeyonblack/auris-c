#include "auris_image.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* One axis of the area average: source cell i feeds the outputs
   index[start[i] .. start[i + 1]) with the matching weights, each the
   length of the overlap in source cells, in 1/256ths. */
typedef struct {
  uint32_t *start;
  uint16_t *index, *weight;
  float *inverse; /* per output: one over the weights it receives */
} axis_t;

struct auris_image {
  uint32_t sw, sh, bw, bh, width, height;
  unsigned int bits;
  auris_image_rect_t content;
  axis_t x, y;
  int32_t *row; /* content.width x 3 */
  float *sum;   /* content.height x content.width x 3 */
  uint16_t *shade; /* bh x bw x 3 gains in 1/1024ths, or NULL */
  /* On the scale of two 10-bit samples added together. */
  int32_t black2, white2, limit;
  float base_gain[3], gain[3], exposure, target, max_gain, knee;
  float matrix[3][3];
  bool auto_white_balance, auto_exposure, adapted;
  uint32_t step;
  size_t capacity;
  float *samples; /* r, g, b of the cells looked at, 0..1 */
  uint8_t *at_limit;
  auris_image_stats_t stats;
  uint8_t encoding[4096];
};

static const float LUMA[3] = {0.2126f, 0.7152f, 0.0722f};

static void axis_free(axis_t *a) {
  free(a->start);
  free(a->index);
  free(a->weight);
  free(a->inverse);
}

static bool axis_init(axis_t *a, uint32_t source, uint32_t count) {
  const size_t entries = (size_t)source + count + 2U;
  a->start = calloc((size_t)source + 1U, sizeof(*a->start));
  a->index = calloc(entries, sizeof(*a->index));
  a->weight = calloc(entries, sizeof(*a->weight));
  a->inverse = calloc(count, sizeof(*a->inverse));
  if (a->start == NULL || a->index == NULL || a->weight == NULL ||
      a->inverse == NULL) {
    return false;
  }
  const double span = (double)source / (double)count; /* cells per output */
  size_t used = 0U;
  for (uint32_t i = 0U; i < source; ++i) {
    a->start[i] = (uint32_t)used;
    uint32_t o = (uint32_t)floor((double)i / span);
    for (; o < count && (double)o * span < (double)i + 1.0; ++o) {
      const double low = fmax((double)i, (double)o * span);
      const double high = fmin((double)i + 1.0, ((double)o + 1.0) * span);
      const long w = lround(256.0 * (high - low));
      if (w <= 0L || used >= entries) {
        continue;
      }
      a->index[used] = (uint16_t)o;
      a->weight[used] = (uint16_t)w;
      a->inverse[o] += (float)w;
      ++used;
    }
  }
  a->start[source] = (uint32_t)used;
  for (uint32_t o = 0U; o < count; ++o) {
    if (a->inverse[o] <= 0.0f) {
      return false;
    }
    a->inverse[o] = 1.0f / a->inverse[o];
  }
  return true;
}

void auris_image_close(auris_image_t *s) {
  if (s == NULL)
    return;
  axis_free(&s->x);
  axis_free(&s->y);
  free(s->row);
  free(s->sum);
  free(s->shade);
  free(s->samples);
  free(s->at_limit);
  free(s);
}

static bool shade_init(auris_image_t *s, const auris_shading_t *g) {
  const size_t nodes = (size_t)g->columns * g->rows * 3U;
  float least = INFINITY, most = 0.0f;
  for (size_t i = 0U; i < nodes; ++i) {
    if (!isfinite(g->gains[i]) || g->gains[i] <= 0.0f)
      return false;
    least = fminf(least, g->gains[i]);
    most = fmaxf(most, g->gains[i]);
  }
  if (most > 8.0f * least)
    return false;
  s->shade = malloc((size_t)s->bw * s->bh * 3U * sizeof(*s->shade));
  if (s->shade == NULL)
    return false;
  /* The least gain becomes one, so a sample at the sensor's limit never
     falls below white. */
  for (uint32_t y = 0U; y < s->bh; ++y) {
    const float fy = s->bh > 1U ? (float)y * (float)(g->rows - 1U) /
                                      (float)(s->bh - 1U) : 0.0f;
    uint32_t y0 = (uint32_t)fy;
    if (y0 > g->rows - 2U) y0 = g->rows - 2U;
    const float wy = fy - (float)y0;
    for (uint32_t x = 0U; x < s->bw; ++x) {
      const float fx = s->bw > 1U ? (float)x * (float)(g->columns - 1U) /
                                        (float)(s->bw - 1U) : 0.0f;
      uint32_t x0 = (uint32_t)fx;
      if (x0 > g->columns - 2U) x0 = g->columns - 2U;
      const float wx = fx - (float)x0;
      const float *a = &g->gains[((size_t)y0 * g->columns + x0) * 3U];
      const float *b = a + 3U;
      const float *c = a + (size_t)g->columns * 3U;
      const float *d = c + 3U;
      for (size_t k = 0U; k < 3U; ++k) {
        const float top = a[k] + wx * (b[k] - a[k]);
        const float bottom = c[k] + wx * (d[k] - c[k]);
        s->shade[((size_t)y * s->bw + x) * 3U + k] =
            (uint16_t)(1024.0f * (top + wy * (bottom - top)) / least + 0.5f);
      }
    }
  }
  return true;
}

int auris_image_open(auris_image_t **out, const auris_image_config_t *c) {
  if (out == NULL || *out != NULL || c == NULL || c->source_width < 2U ||
      c->source_height < 2U || c->source_width > 4096U ||
      c->source_height > 4096U || (c->source_width & 1U) != 0U ||
      (c->source_height & 1U) != 0U || c->width == 0U || c->height == 0U ||
      c->width > 4096U || c->height > 4096U ||
      (c->bits != 8U && c->bits != 10U) || !isfinite(c->black) ||
      c->black < 0.0f || c->black >= (c->bits == 8U ? 128.0f : 512.0f) ||
      !isfinite(c->target) || c->target <= 0.0f || c->target > 1.0f ||
      !isfinite(c->max_gain) || c->max_gain < 1.0f || c->max_gain > 64.0f ||
      !isfinite(c->shadow_knee) || c->shadow_knee < 0.0f ||
      c->shadow_knee > 1.0f || !isfinite(c->contrast) || c->contrast < 0.0f ||
      c->contrast > 1.0f ||
      (c->shading.columns != 0U &&
       (c->shading.columns < 2U || c->shading.rows < 2U ||
        c->shading.columns > 64U || c->shading.rows > 64U ||
        c->shading.gains == NULL)))
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
  s->bw = s->sw / 2U;
  s->bh = s->sh / 2U;
  s->width = c->width;
  s->height = c->height;
  s->bits = c->bits;
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
  /* Samples are handled as the sum of two 10-bit ones: the two greens of a
     cell added, red and blue doubled. 8-bit samples are scaled up first. */
  const int32_t full = c->bits == 8U ? 255 * 4 : 1023;
  s->black2 = (int32_t)lroundf(2.0f * c->black * (c->bits == 8U ? 4.0f : 1.0f));
  s->white2 = 2 * full - s->black2;
  s->limit = (int32_t)((float)full * 0.985f);
  s->step = s->bw / 200U;
  if (s->step == 0U)
    s->step = 1U;
  s->capacity = ((size_t)s->bw / s->step + 1U) * ((size_t)s->bh / s->step + 1U);
  s->row = malloc((size_t)w * 3U * sizeof(*s->row));
  s->sum = malloc((size_t)w * h * 3U * sizeof(*s->sum));
  s->samples = malloc(s->capacity * 3U * sizeof(*s->samples));
  s->at_limit = malloc(s->capacity);
  if (s->row == NULL || s->sum == NULL || s->samples == NULL ||
      s->at_limit == NULL || !axis_init(&s->x, s->bw, w) ||
      !axis_init(&s->y, s->bh, h)) {
    auris_image_close(s);
    return -ENOMEM;
  }
  if (c->shading.columns != 0U && !shade_init(s, &c->shading)) {
    auris_image_close(s);
    return -EINVAL;
  }
  s->target = c->target;
  s->max_gain = c->max_gain;
  s->knee = c->shadow_knee;
  s->exposure = 1.0f;
  s->auto_white_balance = c->auto_white_balance;
  s->auto_exposure = c->auto_exposure;
  for (size_t k = 0U; k < 3U; ++k) {
    s->base_gain[k] = c->gain[k] / c->gain[1];
    s->gain[k] = s->base_gain[k];
    for (size_t j = 0U; j < 3U; ++j)
      s->matrix[k][j] = zero_matrix ? (k == j ? 1.0f : 0.0f)
                                    : c->colour_matrix[k][j];
  }
  for (size_t i = 0U; i < 4096U; ++i) {
    const float v = (float)i / 4095.0f;
    const float coded = v <= 0.0031308f
                            ? 12.92f * v
                            : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    /* The S-curve keeps black, mid-grey and white where they are. */
    const float bent =
        coded + c->contrast * (coded * coded * (3.0f - 2.0f * coded) - coded);
    s->encoding[i] = (uint8_t)(255.0f * bent + 0.5f);
  }
  memcpy(s->stats.gain, s->gain, sizeof(s->stats.gain));
  s->stats.exposure = s->exposure;
  *out = s;
  return 0;
}

const auris_image_rect_t *auris_image_content(const auris_image_t *s) {
  return s == NULL ? NULL : &s->content;
}

int auris_image_stats(const auris_image_t *s, auris_image_stats_t *stats) {
  if (s == NULL || stats == NULL)
    return -EINVAL;
  *stats = s->stats;
  return 0;
}

static bool fits(uint32_t w, uint32_t h, size_t stride, size_t bytes,
                 size_t bpp) {
  const size_t row = (size_t)w * bpp;
  return stride >= row && bytes >= row &&
         (size_t)(h - 1U) <= (bytes - row) / stride;
}

/* The four samples of the cell at (x, y), in cells, on the 10-bit scale. */
static inline void cell(const auris_image_t *s, const auris_bayer_t *in,
                        uint32_t x, uint32_t y, int32_t v[4]) {
  const uint8_t *top = (const uint8_t *)in->data + (size_t)(2U * y) * in->stride;
  const uint8_t *bottom = top + in->stride;
  if (s->bits == 8U) {
    v[0] = top[2U * x] << 2;
    v[1] = top[2U * x + 1U] << 2;
    v[2] = bottom[2U * x] << 2;
    v[3] = bottom[2U * x + 1U] << 2;
  } else {
    uint16_t a[2], b[2];
    memcpy(a, top + 4U * (size_t)x, sizeof(a));
    memcpy(b, bottom + 4U * (size_t)x, sizeof(b));
    v[0] = a[0] & 1023;
    v[1] = a[1] & 1023;
    v[2] = b[0] & 1023;
    v[3] = b[1] & 1023;
  }
}

/* Looks at one cell in `step` each way: how bright the frame is for the
   sensor, and what colour the light is. */
static void measure(auris_image_t *s, const auris_bayer_t *in) {
  uint32_t histogram[256] = {0U};
  double mean = 0.0;
  size_t count = 0U, limited = 0U;
  const float scale = 1.0f / (float)s->white2;
  for (uint32_t y = 0U; y < s->bh; y += s->step) {
    for (uint32_t x = 0U; x < s->bw && count < s->capacity; x += s->step) {
      int32_t v[4];
      cell(s, in, x, y, v);
      const bool at_limit = v[0] >= s->limit || v[1] >= s->limit ||
                            v[2] >= s->limit || v[3] >= s->limit;
      float rgb[3] = {(float)(2 * v[0] - s->black2),
                      (float)(v[1] + v[2] - s->black2),
                      (float)(2 * v[3] - s->black2)};
      const uint16_t *shade =
          s->shade == NULL ? NULL : &s->shade[((size_t)y * s->bw + x) * 3U];
      for (size_t k = 0U; k < 3U; ++k) {
        rgb[k] *= scale * (shade == NULL ? 1.0f : (float)shade[k] / 1024.0f);
        if (rgb[k] < 0.0f) rgb[k] = 0.0f;
        s->samples[count * 3U + k] = rgb[k];
      }
      s->at_limit[count] = at_limit ? 1U : 0U;
      const float level = at_limit || rgb[1] > 1.0f ? 1.0f : rgb[1];
      mean += (double)level;
      ++histogram[(size_t)(level * 255.0f)];
      limited += at_limit ? 1U : 0U;
      ++count;
    }
  }
  if (count == 0U)
    return;
  s->stats.mean = (float)(mean / (double)count);
  s->stats.clipped = (float)limited / (float)count;
  size_t below = 0U, bin = 0U;
  for (; bin < 255U; ++bin) {
    below += histogram[bin];
    if (below * 10U >= count * 9U)
      break;
  }
  s->stats.bright = (float)(bin + 1U) / 256.0f;

  if (s->auto_white_balance) {
    /* Grey world over the cells that can say something about the light:
       not at the limit, not in the noise. A second pass keeps only those
       the first balance makes nearly grey, so a large coloured object does
       not pull it. */
    const float low = 0.5f * s->stats.mean, high = 0.9f;
    float gain_r = 0.0f, gain_b = 0.0f;
    size_t used = 0U;
    for (int pass = 0; pass < 2; ++pass) {
      double sum[3] = {0.0, 0.0, 0.0};
      size_t n = 0U;
      for (size_t i = 0U; i < count; ++i) {
        const float *p = &s->samples[i * 3U];
        if (s->at_limit[i] != 0U || p[1] <= low || p[1] >= high ||
            p[0] <= 0.0f || p[2] <= 0.0f)
          continue;
        if (pass == 1 && (fabsf(p[0] * gain_r - p[1]) > 0.25f * p[1] ||
                          fabsf(p[2] * gain_b - p[1]) > 0.25f * p[1]))
          continue;
        sum[0] += (double)p[0];
        sum[1] += (double)p[1];
        sum[2] += (double)p[2];
        ++n;
      }
      if (pass == 0) {
        if (n < 32U || n * 50U < count)
          break;
        used = n;
      } else if (n * 5U < used) {
        break; /* little of it is grey: keep the first answer */
      }
      gain_r = (float)(sum[1] / sum[0]);
      gain_b = (float)(sum[1] / sum[2]);
    }
    if (used != 0U) {
      const float wanted[3] = {fminf(fmaxf(gain_r, 0.5f), 4.0f), 1.0f,
                               fminf(fmaxf(gain_b, 0.5f), 4.0f)};
      const float alpha = s->adapted ? 0.15f : 1.0f;
      for (size_t k = 0U; k < 3U; ++k)
        s->gain[k] += alpha * (wanted[k] - s->gain[k]);
    }
  }

  if (s->auto_exposure) {
    const float least = fminf(s->gain[0], fminf(s->gain[1], s->gain[2]));
    double luma = 0.0;
    for (size_t i = 0U; i < count; ++i) {
      const float *p = &s->samples[i * 3U];
      for (size_t k = 0U; k < 3U; ++k) {
        const float v = s->at_limit[i] != 0U ? 1.0f : p[k] * s->gain[k] / least;
        luma += (double)(LUMA[k] * (v > 1.0f ? 1.0f : v));
      }
    }
    float wanted = s->target / fmaxf((float)(luma / (double)count), 1.0e-5f);
    wanted = fminf(fmaxf(wanted, 1.0f), s->max_gain);
    /* A step in the sensor's exposure is followed at once, so the picture
       does not flash; anything smaller is followed gradually. */
    const float change = wanted / s->exposure;
    if (!s->adapted || change > 1.25f || change < 0.8f)
      s->exposure = wanted;
    else
      s->exposure += 0.25f * (wanted - s->exposure);
  }
  s->adapted = true;
  memcpy(s->stats.gain, s->gain, sizeof(s->stats.gain));
  s->stats.exposure = s->exposure;
}

static bool input_valid(const auris_image_t *s, const auris_bayer_t *in) {
  return s != NULL && in != NULL && in->data != NULL && in->width == s->sw &&
         in->height == s->sh && in->bits == s->bits &&
         fits(in->width, in->height, in->stride, in->bytes,
              s->bits == 8U ? 1U : 2U);
}

int auris_image_measure(auris_image_t *s, const auris_bayer_t *in) {
  if (!input_valid(s, in))
    return -EINVAL;
  measure(s, in);
  return 0;
}

int auris_image_render(auris_image_t *s, const auris_bayer_t *in,
                       auris_rgb24_t *out) {
  if (!input_valid(s, in) || out == NULL || out->data == NULL ||
      out->width != s->width || out->height != s->height ||
      !fits(out->width, out->height, out->stride, out->bytes, 3U))
    return -EINVAL;
  measure(s, in);
  const uint32_t cw = s->content.width, ch = s->content.height;
  /* White balance with its least gain at one: a channel at the sensor's
     limit then reaches white whatever the others do. */
  const float least = fminf(s->gain[0], fminf(s->gain[1], s->gain[2]));
  int32_t balance[3];
  for (size_t k = 0U; k < 3U; ++k)
    balance[k] = (int32_t)lroundf(1024.0f * s->gain[k] / least);
  memset(s->sum, 0, (size_t)cw * ch * 3U * sizeof(*s->sum));
  for (uint32_t y = 0U; y < s->bh; ++y) {
    memset(s->row, 0, (size_t)cw * 3U * sizeof(*s->row));
    const uint16_t *shade =
        s->shade == NULL ? NULL : &s->shade[(size_t)y * s->bw * 3U];
    for (uint32_t x = 0U; x < s->bw; ++x) {
      int32_t raw[4];
      cell(s, in, x, y, raw);
      int32_t v[3] = {2 * raw[0] - s->black2, raw[1] + raw[2] - s->black2,
                      2 * raw[3] - s->black2};
      for (size_t k = 0U; k < 3U; ++k) {
        if (shade != NULL)
          v[k] = v[k] * (int32_t)shade[(size_t)x * 3U + k] / 1024;
        v[k] = v[k] * balance[k] / 1024;
        if (v[k] > s->white2)
          v[k] = s->white2;
      }
      for (uint32_t i = s->x.start[x]; i < s->x.start[x + 1U]; ++i) {
        int32_t *cellsum = &s->row[(size_t)s->x.index[i] * 3U];
        const int32_t w = (int32_t)s->x.weight[i];
        cellsum[0] += v[0] * w;
        cellsum[1] += v[1] * w;
        cellsum[2] += v[2] * w;
      }
    }
    for (uint32_t i = s->y.start[y]; i < s->y.start[y + 1U]; ++i) {
      float *dst = &s->sum[(size_t)s->y.index[i] * cw * 3U];
      const float w = (float)s->y.weight[i];
      for (size_t j = 0U; j < (size_t)cw * 3U; ++j)
        dst[j] += (float)s->row[j] * w;
    }
  }
  for (uint32_t y = 0U; y < out->height; ++y)
    memset(out->data + (size_t)y * out->stride, 0, (size_t)out->width * 3U);
  const float to_unit = s->exposure / (float)s->white2;
  for (uint32_t y = 0U; y < ch; ++y) {
    const float *src = &s->sum[(size_t)y * cw * 3U];
    uint8_t *dst = out->data + (size_t)(s->content.y + y) * out->stride +
                   (size_t)s->content.x * 3U;
    const float fy = s->y.inverse[y] * to_unit;
    for (uint32_t x = 0U; x < cw; ++x) {
      const float f = fy * s->x.inverse[x];
      float rgb[3], luma = 0.0f;
      for (size_t k = 0U; k < 3U; ++k) {
        const float v = src[(size_t)x * 3U + k] * f;
        /* Clipped before the matrix, so what is white stays white. */
        rgb[k] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        luma += LUMA[k] * rgb[k];
      }
      const float colour = s->knee > 0.0f ? luma / (luma + s->knee) : 1.0f;
      for (size_t k = 0U; k < 3U; ++k) {
        const float corrected = s->matrix[k][0] * rgb[0] +
                                s->matrix[k][1] * rgb[1] +
                                s->matrix[k][2] * rgb[2];
        float v = luma + colour * (corrected - luma);
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        *dst++ = s->encoding[(size_t)(v * 4095.0f + 0.5f)];
      }
    }
  }
  return 0;
}
