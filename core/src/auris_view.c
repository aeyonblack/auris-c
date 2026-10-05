#include "auris_view.h"
#include "auris_array.h"
#include "auris_status.h"

#include <math.h>
#include <stdbool.h>
#include <float.h>

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

auris_status_t auris_view_nudge(auris_view_t *view, float pan_deg,
                                float tilt_deg) {
  if (!view_valid(view) || !isfinite(pan_deg) || !isfinite(tilt_deg) ||
      fabsf(pan_deg) > 30.0f || fabsf(tilt_deg) > 30.0f) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float degree = 0.017453292519943295f;
  const float cp = cosf(pan_deg * degree), sp = sinf(pan_deg * degree);
  const float ct = cosf(tilt_deg * degree), st = sinf(tilt_deg * degree);
  /* In camera axes (x right, y up, z forward) the picture moves by
     tilt * pan. The view stores camera -> array, so the transpose of that
     product goes on the right. */
  const float turn[3][3] = {{cp, -sp * st, -sp * ct},
                            {0.0f, ct, -st},
                            {sp, cp * st, cp * ct}};
  float next[3][3];
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t col = 0U; col < 3U; ++col) {
      next[row][col] = 0.0f;
      for (size_t k = 0U; k < 3U; ++k) {
        next[row][col] += view->rotation[row][k] * turn[k][col];
      }
    }
  }
  for (size_t row = 0U; row < 3U; ++row) {
    for (size_t col = 0U; col < 3U; ++col) {
      view->rotation[row][col] = next[row][col];
    }
  }
  return AURIS_OK;
}

auris_status_t auris_view_project(const auris_view_t *view,
                                  auris_vec3f_t direction, float *u, float *v,
                                  bool *visible) {
  if (u == NULL || v == NULL || visible == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *u = 0.0f;
  *v = 0.0f;
  *visible = false;
  if (!view_valid(view) || !isfinite(direction.x_m) ||
      !isfinite(direction.y_m) || !isfinite(direction.z_m)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float scale = fmaxf(fabsf(direction.x_m),
      fmaxf(fabsf(direction.y_m), fabsf(direction.z_m)));
  if (scale == 0.0f) return AURIS_ERROR_INVALID_ARGUMENT;
  /* Rescale without a square root; projection depends only on ratios. */
  const float a[3] = {direction.x_m / scale, direction.y_m / scale,
                      direction.z_m / scale};
  float camera[3] = {0};
  for (size_t axis = 0U; axis < 3U; ++axis) {
    for (size_t row = 0U; row < 3U; ++row) {
      camera[axis] += view->rotation[row][axis] * a[row];
    }
  }
  if (camera[2] <= 1.0e-6f) {
    return AURIS_OK;
  }
  const float inverse_z = 1.0f / camera[2];
  const float x = view->fx * (camera[0] * inverse_z) + view->cx;
  const float y = view->cy - view->fy * (camera[1] * inverse_z);
  const float xmax = (float)(view->width - 1U);
  const float ymax = (float)(view->height - 1U);
  const float tolerance = 4.0f * FLT_EPSILON * fmaxf(xmax, ymax);
  if (!isfinite(x) || !isfinite(y) || x < -tolerance || y < -tolerance ||
      x > xmax + tolerance || y > ymax + tolerance) {
    return AURIS_OK;
  }
  /* Keep round-off at boundary rays from changing visibility. */
  *u = fminf(xmax, fmaxf(0.0f, x));
  *v = fminf(ymax, fmaxf(0.0f, y));
  *visible = true;
  return AURIS_OK;
}
