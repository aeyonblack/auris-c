#include "auris_view.h"
#include "auris_array.h"
#include "auris_status.h"

#include <math.h>
#include <stdbool.h>

static bool view_valid(const auris_view_t *v) {
  if (v == NULL || v->width < 2U || v->height < 2U || !isfinite(v->fx) ||
      !isfinite(v->fy) || !isfinite(v->cx) || !isfinite(v->cy) ||
      v->fx <= 0.0f || v->fy <= 0.0f) {
    return false;
  }
  for (size_t a = 0U; a < 3U; ++a) {
    for (size_t b = 0U; b < 3U; ++b) {
      float dot = 0.0f;
      for (size_t k = 0U; k < 3U; ++k) {
        dot += v->rotation[k][a] * v->rotation[k][b];
      }
      const float expected = a == b ? 1.0f : 0.0f;
      if (!isfinite(dot) || fabsf(dot - expected) > 1.0e-4f) {
        return false;
      }
    }
  }
  const float(*r)[3] = v->rotation;
  const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                    r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                    r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  return isfinite(det) && fabsf(det - 1.0f) <= 1.0e-4f;
}

static auris_status_t make_ray(const auris_view_t *v, float u, float w,
                               auris_vec3f_t *out) {
  const float x = (u - v->cx) / v->fx;
  const float y = (v->cy - w) / v->fy;
  const float length = hypotf(hypotf(x, y), 1.0f);
  if (!isfinite(length)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float a[3] = {x / length, y / length, 1.0f / length};
  float b[3] = {0};
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t col = 0U; col < 3U; ++col) {
      b[row] += v->rotation[row][col] * a[col];
    }
  }
  if (b[2] <= 0.0f) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float norm = hypotf(hypotf(b[0], b[1]), b[2]);
  *out = (auris_vec3f_t){b[0] / norm, b[1] / norm, b[2] / norm};
  return AURIS_OK;
}

auris_status_t auris_view_ray(const auris_view_t *view, float u, float v,
                              auris_vec3f_t *out) {
  if (!view_valid(view) || out == NULL || !isfinite(u) || !isfinite(v) ||
      u < 0.0f || v < 0.0f || u > (float)(view->width - 1U) ||
      v > (float)(view->height - 1U)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  return make_ray(view, u, v, out);
}

auris_status_t auris_view_grid(const auris_view_t *view, size_t columns,
                               size_t rows, auris_vec3f_t *out,
                               size_t capacity) {
  if (!view_valid(view) || out == NULL || columns < 2U || rows < 2U) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  if (columns > capacity / rows) {
    return AURIS_ERROR_CAPACITY;
  }
  for (size_t row = 0U; row < rows; ++row) {
    const float v =
        (float)row / (float)(rows - 1U) * (float)(view->height - 1U);
    for (size_t col = 0U; col < columns; ++col) {
      const float u =
          (float)col / (float)(columns - 1U) * (float)(view->width - 1U);
      const auris_status_t status =
          make_ray(view, u, v, &out[row * columns + col]);
      if (status != AURIS_OK) {
        return status;
      }
    }
  }
  return AURIS_OK;
}
