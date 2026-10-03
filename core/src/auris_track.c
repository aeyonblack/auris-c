#include "auris_track.h"

#include <math.h>

auris_track_config_t auris_track_defaults(void) {
  return (auris_track_config_t){.kappa = 0.5f,
                                .drift_cells = 5.8f, /* 0.6 per 10.7 ms hop */
                                .jump_per_s = 0.1f,
                                .cap = 6.0f,
                                .none_prior = 0.5f,
                                .chance_floor = 0.05f,
                                .chance_root = 0.5f,
                                .confidence_cells = 8U,
                                .mean_cells = 3U,
                                .show = 0.35f,
                                .hide = 0.2f,
                                .full = 0.6f,
                                .display_cells = 2.0f};
}

static bool in_range(float value, float low, float high) {
  return isfinite(value) && value >= low && value <= high;
}

void auris_track_reset(auris_track_t *t) {
  if (t == NULL || t->count == 0U) {
    return;
  }
  const float each = (1.0f - t->config.none_prior) / (float)t->count;
  for (size_t q = 0U; q < t->count; ++q) {
    t->posterior[q] = each;
  }
  t->none = t->config.none_prior;
  t->shown = false;
}

auris_status_t auris_track_init(auris_track_t *t,
                                const auris_track_config_t *c, size_t columns,
                                size_t rows, float hop_seconds) {
  if (t == NULL || c == NULL || columns < 2U || rows < 2U ||
      columns > AURIS_TRACK_MAX_CELLS / rows ||
      !in_range(hop_seconds, 1.0e-4f, 1.0f) ||
      !in_range(c->kappa, 1.0e-3f, 100.0f) ||
      !in_range(c->drift_cells, 0.0f, 100.0f) ||
      !in_range(c->jump_per_s, 0.0f, 10.0f) || !in_range(c->cap, 0.1f, 50.0f) ||
      !in_range(c->none_prior, 0.01f, 0.99f) ||
      !in_range(c->chance_floor, 0.0f, 1.0f) ||
      !in_range(c->chance_root, 0.0f, 10.0f) || c->confidence_cells == 0U ||
      c->mean_cells == 0U || !in_range(c->hide, 0.0f, 1.0f) ||
      !in_range(c->show, c->hide, 1.0f) || !in_range(c->full, c->show, 1.0f) ||
      c->full <= c->hide || !in_range(c->display_cells, 0.5f, 5.0f)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  t->config = *c;
  t->columns = columns;
  t->rows = rows;
  t->count = columns * rows;
  /* Random-walk variance per hop, split over passes that keep the
     three-tap kernel [share, 1 - 2 share, share] well inside its stable
     range (variance 2 share per pass). */
  const float variance = c->drift_cells * c->drift_cells * hop_seconds;
  t->passes = (size_t)ceilf(variance / 0.4f);
  if (t->passes > 64U) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  t->share = t->passes == 0U ? 0.0f : variance / (2.0f * (float)t->passes);
  t->jump = -expm1f(-c->jump_per_s * hop_seconds);
  t->kernel_radius = (size_t)ceilf(3.0f * c->display_cells);
  float total = 0.0f;
  for (size_t i = 0U; i <= 2U * t->kernel_radius; ++i) {
    const float x = ((float)i - (float)t->kernel_radius) / c->display_cells;
    t->kernel[i] = expf(-0.5f * x * x);
    total += t->kernel[i];
  }
  for (size_t i = 0U; i <= 2U * t->kernel_radius; ++i) {
    t->kernel[i] /= total;
  }
  auris_track_reset(t);
  return AURIS_OK;
}

/* One diffusion pass along rows then columns. Edge cells keep the share
   that would have left the grid, so no probability is lost. */
static void diffuse(auris_track_t *t) {
  const size_t w = t->columns, h = t->rows;
  const float a = t->share, keep = 1.0f - 2.0f * a;
  float *p = t->posterior, *s = t->scratch;
  for (size_t y = 0U; y < h; ++y) {
    const float *in = &p[y * w];
    float *out = &s[y * w];
    out[0] = (keep + a) * in[0] + a * in[1];
    for (size_t x = 1U; x + 1U < w; ++x) {
      out[x] = keep * in[x] + a * (in[x - 1U] + in[x + 1U]);
    }
    out[w - 1U] = (keep + a) * in[w - 1U] + a * in[w - 2U];
  }
  for (size_t x = 0U; x < w; ++x) {
    p[x] = (keep + a) * s[x] + a * s[w + x];
    for (size_t y = 1U; y + 1U < h; ++y) {
      p[y * w + x] =
          keep * s[y * w + x] + a * (s[(y - 1U) * w + x] + s[(y + 1U) * w + x]);
    }
    p[(h - 1U) * w + x] =
        (keep + a) * s[(h - 1U) * w + x] + a * s[(h - 2U) * w + x];
  }
}

auris_status_t auris_track_step(auris_track_t *t, const float *scores,
                                float scale, float weight,
                                auris_track_result_t *out) {
  if (t == NULL || out == NULL || t->count == 0U ||
      (scores != NULL &&
       (!isfinite(scale) || scale <= 0.0f || !isfinite(weight) ||
        weight < 0.0f))) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const auris_track_config_t *c = &t->config;
  float *p = t->posterior;
  /* Predict: drift, and the rare redraw from the prior. */
  for (size_t pass = 0U; pass < t->passes; ++pass) {
    diffuse(t);
  }
  const float stay = 1.0f - t->jump;
  const float anywhere = t->jump * (1.0f - c->none_prior) / (float)t->count;
  for (size_t q = 0U; q < t->count; ++q) {
    p[q] = stay * p[q] + anywhere;
  }
  t->none = stay * t->none + t->jump * c->none_prior;
  /* Update with this frame's evidence. */
  if (scores != NULL) {
    float peak = -INFINITY;
    for (size_t q = 0U; q < t->count; ++q) {
      if (!isfinite(scores[q])) {
        return AURIS_ERROR_INVALID_ARGUMENT;
      }
      peak = fmaxf(peak, scores[q]);
    }
    const float chance = c->chance_floor * weight + c->chance_root * sqrtf(weight);
    const float reference = fmaxf(peak * scale, chance);
    for (size_t q = 0U; q < t->count; ++q) {
      const float drop = c->kappa * (scores[q] * scale - reference);
      p[q] *= expf(fmaxf(drop, -c->cap));
    }
    t->none *= expf(fmaxf(c->kappa * (chance - reference), -c->cap));
  }
  double total = (double)t->none;
  for (size_t q = 0U; q < t->count; ++q) {
    total += (double)p[q];
  }
  if (!(total > 0.0) || !isfinite(total)) {
    auris_track_reset(t);
    total = 1.0;
  }
  const float normalize = (float)(1.0 / total);
  size_t best = 0U;
  for (size_t q = 0U; q < t->count; ++q) {
    p[q] *= normalize;
    if (p[q] > p[best]) {
      best = q;
    }
  }
  t->none *= normalize;

  const size_t bx = best % t->columns, by = best / t->columns;
  const size_t reach = c->confidence_cells > c->mean_cells ? c->confidence_cells
                                                           : c->mean_cells;
  const size_t x0 = bx > reach ? bx - reach : 0U;
  const size_t y0 = by > reach ? by - reach : 0U;
  const size_t x1 = bx + reach < t->columns ? bx + reach : t->columns - 1U;
  const size_t y1 = by + reach < t->rows ? by + reach : t->rows - 1U;
  float confidence = 0.0f, mass = 0.0f, mx = 0.0f, my = 0.0f;
  for (size_t y = y0; y <= y1; ++y) {
    for (size_t x = x0; x <= x1; ++x) {
      const float v = p[y * t->columns + x];
      const size_t dx = x > bx ? x - bx : bx - x;
      const size_t dy = y > by ? y - by : by - y;
      if (dx <= c->confidence_cells && dy <= c->confidence_cells) {
        confidence += v;
      }
      if (dx <= c->mean_cells && dy <= c->mean_cells) {
        mass += v;
        mx += v * (float)x;
        my += v * (float)y;
      }
    }
  }
  t->shown = confidence >= (t->shown ? c->hide : c->show);
  const float strength = (confidence - c->hide) / (c->full - c->hide);
  *out = (auris_track_result_t){
      .best = best,
      .column = mass > 0.0f ? mx / mass : (float)bx,
      .row = mass > 0.0f ? my / mass : (float)by,
      .confidence = confidence,
      .none = t->none,
      .strength = !t->shown ? 0.0f : fminf(1.0f, fmaxf(0.0f, strength)),
      .shown = t->shown};
  return AURIS_OK;
}

auris_status_t auris_track_display(auris_track_t *t, float *map,
                                   size_t capacity) {
  if (t == NULL || map == NULL || t->count == 0U || capacity < t->count) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const size_t w = t->columns, h = t->rows, r = t->kernel_radius;
  const float *p = t->posterior;
  float *s = t->scratch;
  /* Separable Gaussian. Near an edge the kernel is renormalized over the
     cells that exist, so a blob at the border is not dimmed. */
  for (size_t y = 0U; y < h; ++y) {
    for (size_t x = 0U; x < w; ++x) {
      const size_t lo = x > r ? x - r : 0U, hi = x + r < w ? x + r : w - 1U;
      float sum = 0.0f, used = 0.0f;
      for (size_t i = lo; i <= hi; ++i) {
        const float k = t->kernel[i + r - x];
        sum += k * p[y * w + i];
        used += k;
      }
      s[y * w + x] = sum / used;
    }
  }
  float peak = 0.0f;
  for (size_t x = 0U; x < w; ++x) {
    for (size_t y = 0U; y < h; ++y) {
      const size_t lo = y > r ? y - r : 0U, hi = y + r < h ? y + r : h - 1U;
      float sum = 0.0f, used = 0.0f;
      for (size_t i = lo; i <= hi; ++i) {
        const float k = t->kernel[i + r - y];
        sum += k * s[i * w + x];
        used += k;
      }
      map[y * w + x] = sum / used;
      peak = fmaxf(peak, map[y * w + x]);
    }
  }
  if (!(peak > 0.0f)) {
    return AURIS_ERROR_INTERNAL;
  }
  for (size_t q = 0U; q < t->count; ++q) {
    map[q] /= peak;
  }
  return AURIS_OK;
}
