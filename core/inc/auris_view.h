#ifndef AURIS_VIEW_H
#define AURIS_VIEW_H

#include "auris_array.h"
#include "auris_status.h"

typedef struct {
  size_t width;
  size_t height;
  float fx, fy, cx, cy;
  float rotation[3][3];
} auris_view_t;

auris_status_t auris_view_ray(const auris_view_t *view, float u, float v,
                              auris_vec3f_t *out);
auris_status_t auris_view_grid(const auris_view_t *view, size_t columns,
                               size_t rows, auris_vec3f_t *out,
                               size_t capacity);

#endif
