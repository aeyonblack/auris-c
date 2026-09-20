#include "auris_timeline.h"
#include <stdint.h>

size_t auris_timeline_match(const auris_timeline_point_t *points, size_t count,
                            uint64_t camera_us, uint64_t now_us,
                            uint64_t max_age_us, uint64_t max_skew_us) {
  if (points == NULL || count == 0U || camera_us == 0U || camera_us > now_us)
    return SIZE_MAX;
  size_t best = SIZE_MAX;
  uint64_t best_skew = UINT64_MAX;
  for (size_t i = 0U; i < count; ++i) {
    const auris_timeline_point_t point = points[i];
    if (!point.usable || point.sample_us == 0U || point.published_us == 0U ||
        point.sample_us > now_us || point.published_us > now_us ||
        now_us - point.sample_us > max_age_us ||
        now_us - point.published_us > max_age_us)
      continue;
    const uint64_t skew = camera_us > point.sample_us
                              ? camera_us - point.sample_us
                              : point.sample_us - camera_us;
    if (skew <= max_skew_us &&
        (skew < best_skew ||
         (skew == best_skew &&
          (best == SIZE_MAX || point.published_us > points[best].published_us)))) {
      best = i;
      best_skew = skew;
    }
  }
  return best;
}
