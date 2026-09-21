#include "auris_srp.h"

#include <math.h>
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
  auris_mic_pair_t pairs[AURIS_SRP_MAX_PAIRS];
  auris_vec3f_t directions[AURIS_SRP_MAX_DIRECTIONS];
  lookup_t *table;
  float curves[AURIS_SRP_MAX_PAIRS][AURIS_GCC_SIZE];
};

static float dot(auris_vec3f_t a, auris_vec3f_t b) {
  return a.x_m * b.x_m + a.y_m * b.y_m + a.z_m * b.z_m;
}

void auris_srp_destroy(auris_srp_t *state) {
  if (state != NULL) {
    free(state->table);
    free(state);
  }
}

auris_srp_t *auris_srp_create(const auris_array_t *array,
                              unsigned int sample_rate,
                              const auris_vec3f_t *directions, size_t count) {
  if (auris_array_validate(array) != AURIS_OK ||
      array->microphone_count > AURIS_AUDIO_CHANNELS || sample_rate == 0U ||
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
  s->channels = array->microphone_count;
  s->count = count;
  memcpy(s->directions, directions, count * sizeof(*directions));
  if (auris_array_generate_pairs(array, s->pairs, AURIS_SRP_MAX_PAIRS,
                                 &s->pair_count) != AURIS_OK) {
    auris_srp_destroy(s);
    return NULL;
  }
  s->table = calloc(count * s->pair_count, sizeof(*s->table));
  if (s->table == NULL) {
    auris_srp_destroy(s);
    return NULL;
  }
  const float scale = (float)sample_rate / array->speed_of_sound_mps;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    if (!isfinite(s->pairs[p].distance_m * scale) ||
        s->pairs[p].distance_m * scale >=
            (float)AURIS_ANALYSIS_SIZE / 2.0f - 1.0f) {
      auris_srp_destroy(s);
      return NULL;
    }
    const auris_vec3f_t a = array->positions[s->pairs[p].first];
    const auris_vec3f_t b = array->positions[s->pairs[p].second];
    const auris_vec3f_t baseline = {b.x_m - a.x_m, b.y_m - a.y_m,
                                    b.z_m - a.z_m};
    for (size_t q = 0U; q < count; ++q) {
      const float lag = -scale * dot(baseline, directions[q]);
      const float fine = lag * (float)AURIS_GCC_UPSAMPLE;
      const int left = (int)floorf(fine);
      lookup_t *entry = &s->table[q * s->pair_count + p];
      entry->index = (size_t)(left < 0 ? (int)AURIS_GCC_SIZE + left : left);
      entry->fraction = fine - (float)left;
    }
  }
  return s;
}

static float pair_score(const auris_srp_t *s, size_t q, size_t p) {
  const lookup_t entry = s->table[q * s->pair_count + p];
  const size_t next = (entry.index + 1U) % AURIS_GCC_SIZE;
  const float a = s->curves[p][entry.index];
  const float b = s->curves[p][next];
  return a + entry.fraction * (b - a);
}

auris_status_t auris_srp_apply(auris_srp_t *s, auris_fft_t *fft,
                               const auris_spectrum_t *spectrum,
                               auris_srp_result_t *out) {
  if (s == NULL || fft == NULL || spectrum == NULL || out == NULL ||
      spectrum->sample_rate != s->sample_rate ||
      spectrum->channels != s->channels) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *out = (auris_srp_result_t){0};
  out->directions = s->count;
  out->pairs = s->pair_count;
  out->first_frame = spectrum->first_frame;
  bool all_valid = true;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    bool valid = false;
    if (auris_gcc_phat_curve(fft, spectrum, s->pairs[p].first,
                             s->pairs[p].second, s->curves[p],
                             &valid) != AURIS_OK) {
      return AURIS_ERROR_INTERNAL;
    }
    all_valid = all_valid && valid;
  }
  float best = -INFINITY;
  float minimum = INFINITY;
  for (size_t q = 0U; q < s->count; ++q) {
    float sum = 0.0f;
    for (size_t p = 0U; p < s->pair_count; ++p) {
      sum += pair_score(s, q, p);
    }
    const float score = sum / (float)s->pair_count;
    out->scores[q] = score;
    minimum = fminf(minimum, score);
    if (score > best) {
      best = score;
      out->best = q;
    }
  }
  out->peak = best;
  for (size_t p = 0U; p < s->pair_count; ++p) {
    out->contributions[p] = pair_score(s, out->best, p);
    const lookup_t entry = s->table[out->best * s->pair_count + p];
    const int left = entry.index >= AURIS_GCC_SIZE / 2U
                         ? (int)entry.index - (int)AURIS_GCC_SIZE
                         : (int)entry.index;
    out->predicted_lags[p] =
        ((float)left + entry.fraction) / (float)AURIS_GCC_UPSAMPLE;
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
