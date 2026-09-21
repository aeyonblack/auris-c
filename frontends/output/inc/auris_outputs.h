#ifndef AURIS_OUTPUTS_H
#define AURIS_OUTPUTS_H
#include "auris_output.h"
typedef struct {
  bool stream, display, explicit_selection;
  const char *drm_device;
  uint32_t connector;
  unsigned int rotation;
} auris_outputs_config_t;
typedef struct auris_outputs auris_outputs_t;
/* Consumes recognized --output/--drm-device/--connector/--rotation options.
   Returns 1 if consumed, 0 if unrelated, or negative errno if malformed. */
int auris_outputs_option(auris_outputs_config_t *config, int argc,
                         char **argv, int *index);
int auris_outputs_open(auris_outputs_t **out, const auris_outputs_config_t *config,
                       int stream_fd, uint32_t width, uint32_t height);
int auris_outputs_submit(auris_outputs_t *out, const auris_rgb24_t *rgb,
                         const auris_output_frame_t *frame);
int auris_outputs_error(auris_outputs_t *out);
int auris_outputs_flush(auris_outputs_t *out, unsigned int timeout_ms);
/* Stream metrics when selected; otherwise display conversion/flip metrics. */
void auris_outputs_stats(const auris_outputs_t *out, auris_output_stats_t *stats);
void auris_outputs_close(auris_outputs_t *out);
#endif
