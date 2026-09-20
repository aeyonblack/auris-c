#ifndef AURIS_CAMERA_CALIBRATION_H
#define AURIS_CAMERA_CALIBRATION_H
#include "auris_calibration.h"
#include <stdint.h>

uint64_t auris_calibration_array_id(const auris_array_t *array);
/* Returns -ENOENT for a missing file; malformed/mismatched files fail closed.
   Only rotation is replaced, and only after the entire file is validated. */
int auris_camera_calibration_load(const char *path, uint64_t array_id,
                                  auris_view_t *camera);
int auris_camera_calibration_save(const char *path, uint64_t array_id,
                                  const auris_view_t *camera);
#endif
