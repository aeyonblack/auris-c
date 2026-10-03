#ifndef AURIS_ACTIVITY_H
#define AURIS_ACTIVITY_H

#include "auris_spectrum.h"
#include "auris_status.h"
#include <stdint.h>

typedef struct {
  bool enabled;
  float low_hz, high_hz;
  float open_db, close_db, minimum_dbfs;
  float calibration_s, attack_s, hold_s;
  float noise_up_s, noise_down_s;
  /* Per-bin SNR weighting. Each in-band bin gets a PHAT weight
     xi / (1 + xi), with xi = max(P / (noise_bias * N) - 1, 0), where P is
     the bin's power averaged over the microphones and N its learned noise.
     The gate then opens and closes on the summed weight (bins' worth of
     source) instead of the band level. */
  bool weighting;
  float noise_bias, weight_open, weight_close, noise_bin_s;
} auris_activity_config_t;

typedef struct {
  float level_dbfs, noise_dbfs, open_dbfs, close_dbfs;
  float weight_sum; /* bins' worth of source; 0 without weighting */
  bool ready, active, evidence;
} auris_activity_result_t;

typedef struct {
  unsigned int rate;
  size_t channels, size, hop, first_bin, last_bin;
  size_t warm_frames, attack_frames, hold_frames;
  size_t warm_seen, attack_seen, quiet_seen;
  float denominator, open_ratio, close_ratio, minimum_power;
  float noise_up, noise_down, noise_power;
  uint64_t next_frame;
  bool enabled, started, active;
  bool weighting;
  float noise_bias, weight_open, weight_close, bin_alpha;
  float noise_bins[AURIS_FFT_MAX_BINS]; /* per-bin noise power */
  float weights[AURIS_FFT_MAX_BINS];    /* this frame's PHAT weights */
} auris_activity_t;

auris_activity_config_t auris_activity_defaults(void);
/* The defaults, measuring the profile's localization band with the
   profile's absolute floor. */
auris_activity_config_t
auris_activity_defaults_for(const auris_capture_profile_t *profile);
/* For AURIS_ANALYSIS_SIZE frames every AURIS_ANALYSIS_HOP samples. */
auris_status_t auris_activity_init(auris_activity_t *state,
                                   const auris_activity_config_t *config,
                                   unsigned int rate, size_t channels,
                                   float window_energy);
/* For `frame_size`-sample frames every `frame_hop` samples. Durations in the
   config stay in seconds; they are converted to whole hops here. */
auris_status_t auris_activity_init_sized(auris_activity_t *state,
                                         const auris_activity_config_t *config,
                                         unsigned int rate, size_t channels,
                                         size_t frame_size, size_t frame_hop,
                                         float window_energy);
void auris_activity_reset(auris_activity_t *state);
auris_status_t auris_activity_apply(auris_activity_t *state,
                                    auris_spectrum_t *spectrum,
                                    auris_activity_result_t *out);

#endif
