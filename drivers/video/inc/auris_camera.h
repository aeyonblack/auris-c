#ifndef AURIS_CAMERA_H
#define AURIS_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct auris_camera auris_camera_t;

typedef struct {
  const char *sensor, *bridge, *device;
  uint32_t width, height;
  bool ti_5_10_packed;
  unsigned int bits; /* 8 or 10 bits per sample (0 means 8) */
} auris_camera_config_t;

typedef struct {
  uint32_t width, height, reported_stride, stride, sizeimage;
  unsigned int bits; /* 10-bit samples arrive as little-endian uint16_t */
} auris_camera_format_t;

/* What the sensor's exposure can be set to. Gains are multipliers. */
typedef struct {
  float line_us; /* duration of one exposure line */
  uint32_t min_lines, max_lines;
  float max_analogue, max_digital;
} auris_camera_exposure_range_t;

typedef struct {
  const uint8_t *data;
  size_t bytes;
  uint32_t sequence, flags;
  uint64_t timestamp_us;
} auris_camera_frame_t;

int auris_camera_open(auris_camera_t **out,
                      const auris_camera_config_t *config);
const auris_camera_format_t *auris_camera_format(const auris_camera_t *camera);
int auris_camera_start(auris_camera_t *camera);
int auris_camera_acquire(auris_camera_t *camera, auris_camera_frame_t *frame,
                         int timeout_ms);
int auris_camera_release(auris_camera_t *camera);
/* Returns -ENOTSUP when the sensor does not offer the controls. */
int auris_camera_exposure_range(const auris_camera_t *camera,
                                auris_camera_exposure_range_t *range);
/* Sets the exposure time in lines and the analogue and digital gains
   (IMX219 register coding). The sensor rounds: what it took is written
   back. A new setting shows in the picture two or three frames later. */
int auris_camera_set_exposure(auris_camera_t *camera, uint32_t *lines,
                              float *analogue, float *digital);
void auris_camera_close(auris_camera_t *camera);

#endif
