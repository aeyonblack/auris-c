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
  /* Bins' worth of weight behind the map (smoothed like the curves), and
     the peak as a share of what full agreement of that weight would give:
     0 for unrelated signals, 1 for one source heard cleanly by every pair. */
  float weight;
  float coherence;
  bool valid;
} auris_srp_result_t;

/* For AURIS_ANALYSIS_SIZE frames, AURIS_ANALYSIS_HOP hop, AURIS_GCC_UPSAMPLE x. */
auris_srp_t *auris_srp_create(const auris_array_t *array,
                              unsigned int sample_rate,
                              const auris_vec3f_t *directions, size_t count);
/* For frame_size-sample frames every frame_hop samples, scored on GCC curves
   refined `upsample` times. Use it with auris_fft_create_size(frame_size,
   upsample); auris_srp_apply refuses any other transform or spectrum. */
auris_srp_t *auris_srp_create_sized(const auris_array_t *array,
                                    unsigned int sample_rate,
                                    size_t frame_size, size_t frame_hop,
                                    size_t upsample,
                                    const auris_vec3f_t *directions,
                                    size_t count);
void auris_srp_destroy(auris_srp_t *state);
auris_status_t auris_srp_apply(auris_srp_t *state, auris_fft_t *fft,
                               const auris_spectrum_t *spectrum,
                               auris_srp_result_t *out);
auris_status_t auris_srp_set_smoothing(auris_srp_t *s, float seconds);
void auris_srp_reset(auris_srp_t *state);
auris_status_t auris_srp_set_grid(auris_srp_t *state,
    size_t columns, size_t rows, size_t step);

#endif
