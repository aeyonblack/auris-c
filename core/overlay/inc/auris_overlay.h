#ifndef AURIS_OVERLAY_H
#define AURIS_OVERLAY_H

#include "auris_image.h"
#include "auris_projection.h"

typedef struct auris_overlay auris_overlay_t;

typedef struct {
  auris_view_t camera, search;
  auris_projection_layout_t layout;
  size_t columns, rows;
  float cutoff, opacity;
  bool mark_peak;
} auris_overlay_config_t;

int auris_overlay_open(auris_overlay_t **out,
                       const auris_overlay_config_t *config);
/* Blend onto a freshly rendered canvas. Invalid maps leave it unchanged. */
int auris_overlay_apply(const auris_overlay_t *state, const float *scores,
                        size_t count, size_t best, bool valid,
                        auris_rgb24_t *frame);
void auris_overlay_close(auris_overlay_t *state);

#endif
