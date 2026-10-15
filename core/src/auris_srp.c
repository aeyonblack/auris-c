#include "auris_srp.h"
#include "auris_analysis.h"
#include "auris_spectrum.h"
#include "auris_status.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A direction's lag for one pair: `index` counts from the pair's lowest
   needed fine lag (lag_first), so the curve is read at index and index + 1. */
typedef struct {
  uint32_t index;
  float fraction;
} lookup_t;

struct auris_srp {
  unsigned int sample_rate;
  size_t channels;
  size_t count;
  size_t pair_count;
  size_t columns, rows, step;
  auris_mic_pair_t pairs[AURIS_SRP_MAX_PAIRS];
  auris_vec3f_t directions[AURIS_SRP_MAX_DIRECTIONS];
  lookup_t *table; /* pair_count rows of count entries */
  size_t frame_size, frame_hop, upsample, curve_size;
  /* Each pair needs its curve only between its lowest and highest lag over
     the grid (plus one): lag_count[p] fine lags from lag_first[p], listed in
     lags[p * lag_span ..]. max_lag bounds them all. */
  int lag_first[AURIS_SRP_MAX_PAIRS];
  size_t lag_count[AURIS_SRP_MAX_PAIRS];
  size_t lag_span, max_lag;
  int *lags;
  float *curves;  /* pair_count rows of lag_span floats */
  float *scratch; /* lag_span floats */
  float *sums;    /* count floats */
  float speed_of_sound_mps;
  float *gains; /* pair_count rows of frame_size / 2 + 1 bin gains, or NULL */
  /* Height band: `curves` then holds the bins below split_bin only. */
  size_t split_bin;   /* 0: off */
  float height_keep, height_share;
  float low_weight, high_weight; /* over the last second of frames, in bins */
  float weight_alpha;
  float keep_now; /* the low bins' say between rows for this frame */
  float *curves_high; /* pair_count rows: the bins from split_bin up */
  float *low, *high;  /* count scores of each part */
  float *column_best; /* columns values: each column's best low score */
  float smoothing_alpha;
  float weight; /* smoothed mean pair weight, in bins */
  uint64_t next_frame;
  bool history_ready;
};

static float dot(auris_vec3f_t a, auris_vec3f_t b) {
  return a.x_m * b.x_m + a.y_m * b.y_m + a.z_m * b.z_m;
}

void auris_srp_reset(auris_srp_t *s) {
  if (s != NULL) {
    s->history_ready = false;
  }
}

auris_status_t auris_srp_set_smoothing(auris_srp_t *s, float seconds) {
  if (s == NULL || !isfinite(seconds) || seconds < 0.0f || seconds > 60.0f) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float dt = (float)s->frame_hop / (float)s->sample_rate;
  s->smoothing_alpha = seconds == 0.0f ? 1.0f : -expm1f(-dt / seconds);
  auris_srp_reset(s);
  return AURIS_OK;
}

auris_status_t auris_srp_set_diffuse_rejection(auris_srp_t *s, float strength) {
  if (s == NULL || !isfinite(strength) || strength < 0.0f || strength > 1.0f) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  auris_srp_reset(s);
  if (strength == 0.0f) {
    free(s->gains);
    s->gains = NULL;
    return AURIS_OK;
  }
  const size_t bins = s->frame_size / 2U + 1U;
  if (s->gains == NULL) {
    s->gains = calloc(s->pair_count * bins, sizeof(*s->gains));
    if (s->gains == NULL) {
      return AURIS_ERROR_INTERNAL;
    }
  }
  const double bin_hz = (double)s->sample_rate / (double)s->frame_size;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    const double radians_per_hz = 6.283185307179586 *
                                  (double)s->pairs[p].distance_m /
                                  (double)s->speed_of_sound_mps;
    for (size_t k = 0U; k < bins; ++k) {
      const double x = radians_per_hz * (double)k * bin_hz;
      const double coherence = x < 1.0e-6 ? 1.0 : sin(x) / x;
      s->gains[p * bins + k] =
          (float)(1.0 - (double)strength * coherence * coherence);
    }
  }
  return AURIS_OK;
}

void auris_srp_destroy(auris_srp_t *state) {
  if (state != NULL) {
    free(state->table);
    free(state->lags);
    free(state->sums);
    free(state->curves);
    free(state->scratch);
    free(state->gains);
    free(state->curves_high);
    free(state->low);
    free(state->high);
    free(state->column_best);
    free(state);
  }
}

auris_srp_t *auris_srp_create_sized(const auris_array_t *array,
                                    unsigned int sample_rate,
                                    size_t frame_size, size_t frame_hop,
                                    size_t upsample,
                                    const auris_vec3f_t *directions,
                                    size_t count) {
  if (auris_array_validate(array) != AURIS_OK ||
      array->microphone_count > AURIS_AUDIO_CHANNELS || sample_rate == 0U ||
      !auris_analysis_size_valid(frame_size, frame_hop) ||
      (upsample != 1U && upsample != 2U && upsample != 4U && upsample != 8U) ||
      directions == NULL || count < 2U || count > AURIS_SRP_MAX_DIRECTIONS) {
    return NULL;
  }
  for (size_t q = 0U; q < count; ++q) {
    const float length2 = dot(directions[q], directions[q]);
    if (!isfinite(length2) || fabsf(length2 - 1.0f) > 1.0e-4f) {
      return NULL;
    }
  }
  auris_srp_t *s = calloc(1U, sizeof(*s));
  if (s == NULL) {
    return NULL;
  }
  s->sample_rate = sample_rate;
  s->frame_size = frame_size;
  s->frame_hop = frame_hop;
  s->upsample = upsample;
  s->curve_size = frame_size * upsample;
  s->smoothing_alpha = 1.0f;
  s->speed_of_sound_mps = array->speed_of_sound_mps;
  s->channels = array->microphone_count;
  s->count = count;
  memcpy(s->directions, directions, count * sizeof(*directions));
  if (auris_array_generate_pairs(array, s->pairs, AURIS_SRP_MAX_PAIRS,
                                 &s->pair_count) != AURIS_OK) {
    auris_srp_destroy(s);
    return NULL;
  }
  s->table = calloc(count * s->pair_count, sizeof(*s->table));
  s->sums = calloc(count, sizeof(*s->sums));
  int *left_of = calloc(count, sizeof(*left_of));
  if (s->table == NULL || s->sums == NULL || left_of == NULL) {
    free(left_of);
    auris_srp_destroy(s);
    return NULL;
  }
  const float scale = (float)sample_rate / array->speed_of_sound_mps;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    /* Circular lags beyond half the frame would alias. */
    if (!isfinite(s->pairs[p].distance_m * scale) ||
        s->pairs[p].distance_m * scale >= (float)frame_size / 2.0f - 1.0f) {
      auris_srp_destroy(s);
      return NULL;
    }
    const auris_vec3f_t a = array->positions[s->pairs[p].first];
    const auris_vec3f_t b = array->positions[s->pairs[p].second];
    const auris_vec3f_t baseline = {b.x_m - a.x_m, b.y_m - a.y_m,
                                    b.z_m - a.z_m};
    int lowest = 0, highest = 0;
    for (size_t q = 0U; q < count; ++q) {
      const float lag = -scale * dot(baseline, directions[q]);
      const float fine = lag * (float)upsample;
      const int left = (int)floorf(fine);
      left_of[q] = left;
      s->table[p * count + q].fraction = fine - (float)left;
      lowest = q == 0U || left < lowest ? left : lowest;
      highest = q == 0U || left > highest ? left : highest;
    }
    for (size_t q = 0U; q < count; ++q) {
      s->table[p * count + q].index = (uint32_t)(left_of[q] - lowest);
    }
    /* The interpolation also reads one lag past the highest. */
    s->lag_first[p] = lowest;
    s->lag_count[p] = (size_t)(highest - lowest) + 2U;
    s->lag_span = s->lag_count[p] > s->lag_span ? s->lag_count[p] : s->lag_span;
    const size_t reach_low = (size_t)(lowest < 0 ? -lowest : lowest);
    const size_t reach_high = (size_t)(highest + 1 < 0 ? -(highest + 1) : highest + 1);
    s->max_lag = reach_low > s->max_lag ? reach_low : s->max_lag;
    s->max_lag = reach_high > s->max_lag ? reach_high : s->max_lag;
  }
  free(left_of);
  s->lags = calloc(s->pair_count * s->lag_span, sizeof(*s->lags));
  s->curves = calloc(s->pair_count * s->lag_span, sizeof(*s->curves));
  s->scratch = calloc(s->lag_span, sizeof(*s->scratch));
  if (s->lags == NULL || s->curves == NULL || s->scratch == NULL) {
    auris_srp_destroy(s);
    return NULL;
  }
  for (size_t p = 0U; p < s->pair_count; ++p) {
    for (size_t j = 0U; j < s->lag_count[p]; ++j) {
      s->lags[p * s->lag_span + j] = s->lag_first[p] + (int)j;
    }
  }
  return s;
}

auris_srp_t *auris_srp_create(const auris_array_t *array,
                              unsigned int sample_rate,
                              const auris_vec3f_t *directions, size_t count) {
  return auris_srp_create_sized(array, sample_rate, AURIS_ANALYSIS_SIZE,
                                AURIS_ANALYSIS_HOP, AURIS_GCC_UPSAMPLE,
                                directions, count);
}

static inline float lerp(const float *curve, lookup_t entry) {
  const float a = curve[entry.index];
  const float b = curve[entry.index + 1U];
  return a + entry.fraction * (b - a);
}

static float pair_score(const auris_srp_t *s, const float *curves, size_t q,
                        size_t p) {
  return lerp(&curves[p * s->lag_span], s->table[p * s->count + q]);
}

/* sums[q] = sum over pairs, in pair order, of each pair's curve at cell q.
   Pair by pair, so one pair's short curve and its row of the table are read
   straight through. */
static void sum_pairs(const auris_srp_t *s, const float *curves, float *sums) {
  const size_t count = s->count;
  memset(sums, 0, count * sizeof(*sums));
  for (size_t p = 0U; p < s->pair_count; ++p) {
    const float *curve = &curves[p * s->lag_span];
    const lookup_t *row = &s->table[p * count];
    for (size_t q = 0U; q < count; ++q) {
      sums[q] += lerp(curve, row[q]);
    }
  }
}

auris_status_t auris_srp_set_grid(auris_srp_t *s,
    size_t columns, size_t rows, size_t step) {
  if (s == NULL || columns < 2U || rows < 2U || step == 0U ||
      columns > AURIS_SRP_MAX_DIRECTIONS / rows ||
      columns * rows != s->count ||
      (columns - 1U) % step != 0U || (rows - 1U) % step != 0U ||
      /* The height band scores every cell. */
      (s->split_bin != 0U &&
       (step != 1U || columns != s->columns || rows != s->rows))) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  s->columns = columns;
  s->rows = rows;
  s->step = step;
  return AURIS_OK;
}

auris_status_t auris_srp_set_height_band(auris_srp_t *s, float split_hz,
                                         float keep, float share) {
  if (s == NULL || !isfinite(split_hz) || !isfinite(keep) ||
      !isfinite(share) || split_hz < 0.0f || keep <= 0.0f || keep > 1.0f ||
      share < 0.0f || share > 100.0f) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const size_t bins = s->frame_size / 2U + 1U;
  const double bin_hz = (double)s->sample_rate / (double)s->frame_size;
  const double first = ceil((double)split_hz / bin_hz - 1.0e-6);
  if (split_hz == 0.0f || keep == 1.0f) {
    auris_srp_reset(s);
    s->split_bin = 0U;
    return AURIS_OK;
  }
  /* Both parts need at least one interior bin. */
  if (first < 2.0 || first > (double)(bins - 2U) || s->step != 1U ||
      s->columns < 2U || s->columns * s->rows != s->count) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  if (s->curves_high == NULL) {
    s->curves_high = calloc(s->pair_count * s->lag_span, sizeof(float));
    s->low = calloc(s->count, sizeof(float));
    s->high = calloc(s->count, sizeof(float));
    s->column_best = calloc(s->columns, sizeof(float));
    if (s->curves_high == NULL || s->low == NULL || s->high == NULL ||
        s->column_best == NULL) {
      free(s->curves_high);
      free(s->low);
      free(s->high);
      free(s->column_best);
      s->curves_high = s->low = s->high = s->column_best = NULL;
      s->split_bin = 0U;
      return AURIS_ERROR_INTERNAL;
    }
  }
  auris_srp_reset(s);
  s->split_bin = (size_t)first;
  s->height_keep = keep;
  s->height_share = share;
  s->low_weight = 0.0f;
  s->high_weight = 0.0f;
  s->weight_alpha =
      -expm1f(-(float)s->frame_hop / (float)s->sample_rate / 1.0f);
  s->keep_now = share == 0.0f ? keep : 1.0f;
  return AURIS_OK;
}

static void evaluate(const auris_srp_t *s, size_t q,
                     auris_srp_result_t *out) {
  float sum = 0.0f;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    sum += pair_score(s, s->curves, q, p);
  }
  const float score = sum / (float)s->pair_count;
  out->scores[q] = score;
  ++out->evaluated;
  if (score > out->peak || (score == out->peak && q < out->best)) {
    out->peak = score;
    out->best = q;
  }
}

/* Every cell, with the low and high bins scored apart. A cell's low score
   counts as its column's best, less `keep_now` of its shortfall. */
static void scan_height(const auris_srp_t *s, auris_srp_result_t *out) {
  const size_t columns = s->columns;
  const float keep = s->keep_now;
  for (size_t x = 0U; x < columns; ++x) {
    s->column_best[x] = -INFINITY;
  }
  sum_pairs(s, s->curves, s->low);
  sum_pairs(s, s->curves_high, s->high);
  for (size_t q = 0U; q < s->count; ++q) {
    s->low[q] /= (float)s->pair_count;
    s->high[q] /= (float)s->pair_count;
    s->column_best[q % columns] =
        fmaxf(s->column_best[q % columns], s->low[q]);
  }
  out->peak = -INFINITY;
  for (size_t q = 0U; q < s->count; ++q) {
    const float best = s->column_best[q % columns];
    const float score = s->high[q] + best - keep * (best - s->low[q]);
    out->scores[q] = score;
    ++out->evaluated;
    if (score > out->peak) {
      out->peak = score;
      out->best = q;
    }
  }
}

static void scan(const auris_srp_t *s, auris_srp_result_t *out) {
  if (s->split_bin != 0U) {
    scan_height(s, out);
    return;
  }
  out->peak = -INFINITY;
  if (s->step <= 1U) {
    sum_pairs(s, s->curves, s->sums);
    for (size_t q = 0U; q < s->count; ++q) {
      const float score = s->sums[q] / (float)s->pair_count;
      out->scores[q] = score;
      ++out->evaluated;
      if (score > out->peak || (score == out->peak && q < out->best)) {
        out->peak = score;
        out->best = q;
      }
    }
    return;
  }
  const size_t step = s->step, columns = s->columns, rows = s->rows;
  out->approximate = true;
  for (size_t y = 0U; y < rows; y += step) {
    for (size_t x = 0U; x < columns; x += step) {
      evaluate(s, y * columns + x, out);
    }
  }
  /* Save the coarse winner before refinement changes out->best. */
  const size_t cx = out->best % columns, cy = out->best / columns;
  const size_t x0 = cx >= step ? cx - step : 0U;
  const size_t y0 = cy >= step ? cy - step : 0U;
  const size_t x1 = cx + step < columns ? cx + step : columns - 1U;
  const size_t y1 = cy + step < rows ? cy + step : rows - 1U;
  for (size_t y = y0; y <= y1; ++y) {
    for (size_t x = x0; x <= x1; ++x) {
      if (x % step != 0U || y % step != 0U) {
        evaluate(s, y * columns + x, out);
      }
    }
  }
  /* Complete the rectangular map from coarse anchors. These estimates
     never participate in choosing the winner. */
  for (size_t y = 0U; y < rows; ++y) {
    for (size_t x = 0U; x < columns; ++x) {
      if ((x % step == 0U && y % step == 0U) ||
          (x >= x0 && x <= x1 && y >= y0 && y <= y1)) {
        continue;
      }
      const size_t left = x == columns - 1U ? x - step : x / step * step;
      const size_t top = y == rows - 1U ? y - step : y / step * step;
      const float fx = (float)(x - left) / (float)step;
      const float fy = (float)(y - top) / (float)step;
      const float a = out->scores[top * columns + left];
      const float b = out->scores[top * columns + left + step];
      const float c = out->scores[(top + step) * columns + left];
      const float d = out->scores[(top + step) * columns + left + step];
      out->scores[y * columns + x] =
          (1.0f - fy) * ((1.0f - fx) * a + fx * b) +
          fy * ((1.0f - fx) * c + fx * d);
    }
  }
}

auris_status_t auris_srp_apply(auris_srp_t *s, auris_fft_t *fft,
                               const auris_spectrum_t *spectrum,
                               auris_srp_result_t *out) {
  if (s == NULL || fft == NULL || spectrum == NULL || out == NULL ||
      spectrum->sample_rate != s->sample_rate ||
      spectrum->channels != s->channels ||
      /* The transform and spectrum must match this object's tables. */
      auris_fft_size(fft) != s->frame_size ||
      auris_fft_upsample(fft) != s->upsample ||
      spectrum->size != s->frame_size) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *out = (auris_srp_result_t){0};
  out->directions = s->count;
  out->pairs = s->pair_count;
  out->first_frame = spectrum->first_frame;
  if (s->history_ready && spectrum->first_frame != s->next_frame) {
    auris_srp_reset(s);
  }
  if (auris_fft_reserve_lags(fft, s->max_lag) != AURIS_OK) {
    auris_srp_reset(s);
    return AURIS_ERROR_INTERNAL;
  }
  bool all_valid = true;
  float frame_weight = 0.0f, frame_high = 0.0f;
  const size_t last_bin = s->frame_size / 2U;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    const float *gains =
        s->gains == NULL ? NULL : &s->gains[p * (s->frame_size / 2U + 1U)];
    const size_t a = s->pairs[p].first, b = s->pairs[p].second;
    /* One curve of every bin, or one of the bins below split_bin and one
       of the rest. */
    const size_t parts = s->split_bin == 0U ? 1U : 2U;
    bool pair_valid = false;
    for (size_t part = 0U; part < parts; ++part) {
      bool valid = false;
      float weight = 0.0f;
      const size_t first = part == 0U ? 1U : s->split_bin;
      const size_t last = parts == 2U && part == 0U ? s->split_bin - 1U
                                                    : last_bin;
      if (auris_gcc_phat_lags(fft, spectrum, a, b, gains, first, last,
                              &s->lags[p * s->lag_span], s->lag_count[p],
                              s->scratch, &valid, &weight) != AURIS_OK) {
        auris_srp_reset(s);
        return AURIS_ERROR_INTERNAL;
      }
      frame_weight += weight / (float)s->pair_count;
      if (part == 1U) {
        frame_high += weight / (float)s->pair_count;
      }
      pair_valid = pair_valid || valid;
      float *curve = part == 0U ? &s->curves[p * s->lag_span]
                                : &s->curves_high[p * s->lag_span];
      for (size_t n = 0U; n < s->lag_count[p]; ++n) {
        if (!s->history_ready || s->smoothing_alpha == 1.0f) {
          curve[n] = s->scratch[n];
        } else {
          curve[n] += s->smoothing_alpha * (s->scratch[n] - curve[n]);
        }
      }
    }
    all_valid = all_valid && pair_valid;
  }
  /* The weight is smoothed exactly like the curves it scales. */
  if (!s->history_ready || s->smoothing_alpha == 1.0f) {
    s->weight = frame_weight;
  } else {
    s->weight += s->smoothing_alpha * (frame_weight - s->weight);
  }
  s->history_ready = all_valid;
  s->next_frame = spectrum->first_frame + (uint64_t)s->frame_hop;
  if (s->split_bin != 0U && s->height_share > 0.0f) {
    /* Both averages start from nothing, so the first frame's own ratio
       counts at once. They are kept through pauses. */
    s->low_weight += s->weight_alpha * (frame_weight - frame_high - s->low_weight);
    s->high_weight += s->weight_alpha * (frame_high - s->high_weight);
    const float reached =
        s->high_weight >= s->height_share * s->low_weight
            ? 1.0f
            : s->high_weight / (s->height_share * s->low_weight);
    s->keep_now = 1.0f - (1.0f - s->height_keep) * reached;
  }
  scan(s, out);
  out->weight = s->weight;
  out->coherence =
      s->weight > 1.0e-6f
          ? fmaxf(0.0f, out->peak * (float)s->frame_size / (2.0f * s->weight))
          : 0.0f;
  const float best = out->peak;
  float minimum = INFINITY;
  for (size_t q = 0U; q < s->count; ++q) {
    minimum = fminf(minimum, out->scores[q]);
  }
  for (size_t p = 0U; p < s->pair_count; ++p) {
    out->contributions[p] =
        pair_score(s, s->curves, out->best, p) +
        (s->split_bin == 0U ? 0.0f
                            : pair_score(s, s->curves_high, out->best, p));
    const lookup_t entry = s->table[p * s->count + out->best];
    const int left = s->lag_first[p] + (int)entry.index;
    out->predicted_lags[p] =
        ((float)left + entry.fraction) / (float)s->upsample;
  }
  /* Compare against the strongest candidate at least 20 degrees away. */
  float rival = -INFINITY;
  for (size_t q = 0U; q < s->count; ++q) {
    if (dot(s->directions[q], s->directions[out->best]) <= 0.93969262f) {
      rival = fmaxf(rival, out->scores[q]);
    }
  }
  if (isfinite(rival)) {
    out->contrast = (best - rival) / (fabsf(best) + fabsf(rival) + 1.0e-12f);
  }
  out->valid = all_valid && best > 0.0f && best - minimum > 1.0e-5f;
  return AURIS_OK;
}
