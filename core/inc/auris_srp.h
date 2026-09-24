#ifndef AURIS_SRP_H
#define AURIS_SRP_H

#include "auris_array.h"
#include "auris_spectrum.h"
#include "auris_status.h"
#include <stdint.h>

#define AURIS_SRP_MAX_DIRECTIONS 4096U
#define AURIS_SRP_GRID_SEARCH 1
#define AURIS_SRP_MAX_PAIRS                                                    \
  (AURIS_AUDIO_CHANNELS * (AURIS_AUDIO_CHANNELS - 1U) / 2U)

typedef struct auris_srp auris_srp_t;

typedef struct {
  size_t directions;
  size_t pairs;
  size_t best;
  size_t evaluated;
  bool approximate;
  uint64_t first_frame;
  float scores[AURIS_SRP_MAX_DIRECTIONS];
  float contributions[AURIS_SRP_MAX_PAIRS];
  float predicted_lags[AURIS_SRP_MAX_PAIRS];
  float peak;
  float contrast;
  bool valid;
} auris_srp_result_t;

auris_srp_t *auris_srp_create(const auris_array_t *array,
                              unsigned int sample_rate,
                              const auris_vec3f_t *directions, size_t count);
void auris_srp_destroy(auris_srp_t *state);
auris_status_t auris_srp_apply(auris_srp_t *state, auris_fft_t *fft,
                               const auris_spectrum_t *spectrum,
                               auris_srp_result_t *out);
auris_status_t auris_srp_set_smoothing(auris_srp_t *s, float seconds);
void auris_srp_reset(auris_srp_t *state);
auris_status_t auris_srp_set_grid(auris_srp_t *state,
    size_t columns, size_t rows, size_t step);

#endif
