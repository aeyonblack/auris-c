#ifndef IMAGER_CALIBRATION_H
#define IMAGER_CALIBRATION_H
#include "auris_camera_calibration.h"
#include "auris_overlay.h"

#define CAL_CAPTURE_SAMPLES 64U
typedef struct {
  bool active;
  uint64_t start_us, deadline_us;
  size_t count, rejected;
  auris_vec3f_t rays[CAL_CAPTURE_SAMPLES];
} imager_calibration_capture_t;

typedef struct {
  auris_overlay_config_t drawing;
  auris_overlay_t *overlay;
  auris_view_t baseline, candidate;
  auris_calibration_pair_t pairs[AURIS_CALIBRATION_MAX_POINTS];
  auris_calibration_pair_t checks[AURIS_CALIBRATION_MAX_POINTS];
  size_t count, check_count;
  bool solved, pending, checking;
  auris_vec3f_t clicked;
  const char *path;
  uint64_t array_id;
  struct { uint32_t sequence; uint64_t timestamp; bool valid; } frames[16];
  size_t next_frame;
} imager_calibration_t;

/* Capture state belongs to the mailbox mutex; all session state belongs to main. */
void imager_calibration_feed(imager_calibration_capture_t *capture,
    auris_vec3f_t ray, uint64_t sample_us, uint64_t now, bool evidence);
int imager_calibration_average(const imager_calibration_capture_t *capture,
                               auris_vec3f_t *ray, double *scatter_deg);
void imager_calibration_command(imager_calibration_t *s, const char *line,
                                uint64_t now, imager_calibration_capture_t *capture);
void imager_calibration_finish(imager_calibration_t *s,
                               const imager_calibration_capture_t *capture);
void imager_calibration_frame(imager_calibration_t *s, uint32_t sequence,
                              uint64_t timestamp, bool monotonic);
#endif
