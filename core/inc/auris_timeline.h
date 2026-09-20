#ifndef AURIS_TIMELINE_H
#define AURIS_TIMELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint64_t sample_us;
  uint64_t published_us;
  bool usable;
} auris_timeline_point_t;

/* Returns SIZE_MAX when no usable observation is sufficiently fresh and
   close to the camera timestamp. Equal-skew ties prefer the newer result. */
size_t auris_timeline_match(const auris_timeline_point_t *points, size_t count,
                            uint64_t camera_us, uint64_t now_us,
                            uint64_t max_age_us, uint64_t max_skew_us);

#endif
