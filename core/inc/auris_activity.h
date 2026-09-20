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
} auris_activity_config_t;

typedef struct {
  float level_dbfs, noise_dbfs, open_dbfs, close_dbfs;
  bool ready, active, evidence;
} auris_activity_result_t;

typedef struct {
  unsigned int rate;
  size_t channels, first_bin, last_bin;
  size_t warm_frames, attack_frames, hold_frames;
  size_t warm_seen, attack_seen, quiet_seen;
  float denominator, open_ratio, close_ratio, minimum_power;
  float noise_up, noise_down, noise_power;
  uint64_t next_frame;
  bool enabled, started, active;
} auris_activity_t;

auris_activity_config_t auris_activity_defaults(void);
auris_status_t auris_activity_init(auris_activity_t *state,
                                   const auris_activity_config_t *config,
                                   unsigned int rate, size_t channels,
                                   float window_energy);
void auris_activity_reset(auris_activity_t *state);
auris_status_t auris_activity_apply(auris_activity_t *state,
                                    auris_spectrum_t *spectrum,
                                    auris_activity_result_t *out);

#endif
