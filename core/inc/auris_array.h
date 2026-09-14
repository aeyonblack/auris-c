#ifndef AURIS_ARRAY_H
#define AURIS_ARRAY_H

#include "auris_status.h"

#include <stddef.h>

#define AURIS_MAX_MICROPHONES 16U
#define AURIS_MAX_PAIRS                                                        \
  ((AURIS_MAX_MICROPHONES * (AURIS_MAX_MICROPHONES - 1U)) / 2U)

typedef struct {
  float x_m;
  float y_m;
  float z_m;
} auris_vec3f_t;

typedef struct {
  const char *name;
  size_t microphone_count;
  auris_vec3f_t positions[AURIS_MAX_MICROPHONES];
  size_t capture_channels[AURIS_MAX_MICROPHONES];
  float speed_of_sound_mps;
} auris_array_t;

typedef struct {
  size_t first;
  size_t second;
  float distance_m;
} auris_mic_pair_t;

size_t auris_array_pair_count(size_t microphone_count);

auris_status_t auris_array_validate(const auris_array_t *array);

auris_status_t auris_array_generate_pairs(const auris_array_t *array,
                                          auris_mic_pair_t *pairs,
                                          size_t pair_capacity,
                                          size_t *pair_count);

auris_status_t auris_array_spacing(const auris_array_t *array,
                                   float *minimum_spacing_m, float *aperture_m);

auris_status_t auris_array_aliasing_estimate_hz(const auris_array_t *array,
                                                float *frequency_hz);

auris_status_t auris_pair_max_tdoa_samples(const auris_mic_pair_t *pair,
                                           float speed_of_sound_mps,
                                           float sample_rate_hz,
                                           float *maximum_delay_samples);

#endif
