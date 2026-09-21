#ifndef AURIS_PROJECTION_H
#define AURIS_PROJECTION_H

#include "auris_view.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  size_t canvas_width, canvas_height;
  size_t content_x, content_y, content_width, content_height;
} auris_projection_layout_t;

typedef struct {
  /* Continuous pixel-centre coordinates; clip rasterization to content.
     Border rays can lie within half a pixel of its outermost centres. */
  float x, y;
  bool visible;
} auris_projected_point_t;

auris_status_t auris_projection_build(
    const auris_view_t *camera, const auris_projection_layout_t *layout,
    const auris_vec3f_t *directions, size_t count,
    auris_projected_point_t *points, size_t capacity);

#endif
