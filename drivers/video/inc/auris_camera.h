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
} auris_camera_config_t;

typedef struct {
  uint32_t width, height, reported_stride, stride, sizeimage;
} auris_camera_format_t;

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
void auris_camera_close(auris_camera_t *camera);

#endif
