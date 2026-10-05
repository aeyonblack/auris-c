#include "auris_srp.h"
#include "auris_analysis.h"
#include "auris_spectrum.h"
#include "auris_status.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t index;
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
  lookup_t *table;
  size_t frame_size, frame_hop, upsample, curve_size;
  float *curves;  /* pair_count rows of curve_size floats */
  float *scratch; /* curve_size floats */
  float speed_of_sound_mps;
  float *gains; /* pair_count rows of frame_size / 2 + 1 bin gains, or NULL */
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
    free(state->curves);
    free(state->scratch);
    free(state->gains);
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
  s->curves = calloc(s->pair_count * s->curve_size, sizeof(*s->curves));
  s->scratch = calloc(s->curve_size, sizeof(*s->scratch));
  if (s->table == NULL || s->curves == NULL || s->scratch == NULL) {
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
    for (size_t q = 0U; q < count; ++q) {
      const float lag = -scale * dot(baseline, directions[q]);
      const float fine = lag * (float)upsample;
      const int left = (int)floorf(fine);
      lookup_t *entry = &s->table[q * s->pair_count + p];
      entry->index =
          (size_t)(left < 0 ? (int)s->curve_size + left : left);
      entry->fraction = fine - (float)left;
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

static float pair_score(const auris_srp_t *s, size_t q, size_t p) {
  const lookup_t entry = s->table[q * s->pair_count + p];
  const float *curve = &s->curves[p * s->curve_size];
  const size_t next = (entry.index + 1U) % s->curve_size;
  const float a = curve[entry.index];
  const float b = curve[next];
  return a + entry.fraction * (b - a);
}

auris_status_t auris_srp_set_grid(auris_srp_t *s,
    size_t columns, size_t rows, size_t step) {
  if (s == NULL || columns < 2U || rows < 2U || step == 0U ||
      columns > AURIS_SRP_MAX_DIRECTIONS / rows ||
      columns * rows != s->count ||
      (columns - 1U) % step != 0U || (rows - 1U) % step != 0U) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  s->columns = columns;
  s->rows = rows;
  s->step = step;
  return AURIS_OK;
}

static void evaluate(const auris_srp_t *s, size_t q,
                     auris_srp_result_t *out) {
  float sum = 0.0f;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    sum += pair_score(s, q, p);
  }
  const float score = sum / (float)s->pair_count;
  out->scores[q] = score;
  ++out->evaluated;
  if (score > out->peak || (score == out->peak && q < out->best)) {
    out->peak = score;
    out->best = q;
  }
}

static void scan(const auris_srp_t *s, auris_srp_result_t *out) {
  out->peak = -INFINITY;
  if (s->step <= 1U) {
    for (size_t q = 0U; q < s->count; ++q) {
      evaluate(s, q, out);
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
  bool all_valid = true;
  float frame_weight = 0.0f;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    bool valid = false;
    float pair_weight = 0.0f;
    const float *gains =
        s->gains == NULL ? NULL : &s->gains[p * (s->frame_size / 2U + 1U)];
    if (auris_gcc_phat_curve_gained(fft, spectrum, s->pairs[p].first,
                                    s->pairs[p].second, gains, s->scratch,
                                    &valid, &pair_weight) != AURIS_OK) {
      auris_srp_reset(s);
      return AURIS_ERROR_INTERNAL;
    }
    frame_weight += pair_weight / (float)s->pair_count;
    float *curve = &s->curves[p * s->curve_size];
    for (size_t n = 0U; n < s->curve_size; ++n) {
      if (!s->history_ready || s->smoothing_alpha == 1.0f) {
        curve[n] = s->scratch[n];
      } else {
        curve[n] += s->smoothing_alpha * (s->scratch[n] - curve[n]);
      }
    }
    all_valid = all_valid && valid;
  }
  /* The weight is smoothed exactly like the curves it scales. */
  if (!s->history_ready || s->smoothing_alpha == 1.0f) {
    s->weight = frame_weight;
  } else {
    s->weight += s->smoothing_alpha * (frame_weight - s->weight);
  }
  s->history_ready = all_valid;
  s->next_frame = spectrum->first_frame + (uint64_t)s->frame_hop;
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
    out->contributions[p] = pair_score(s, out->best, p);
    const lookup_t entry = s->table[out->best * s->pair_count + p];
    const int left = entry.index >= s->curve_size / 2U
                         ? (int)entry.index - (int)s->curve_size
                         : (int)entry.index;
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
