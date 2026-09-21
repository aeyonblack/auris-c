#include "auris_projection.h"

static bool layout_valid(const auris_projection_layout_t *layout) {
  return layout != NULL && layout->canvas_width > 0U &&
         layout->canvas_height > 0U && layout->content_width > 0U &&
         layout->content_height > 0U &&
         layout->content_x < layout->canvas_width &&
         layout->content_y < layout->canvas_height &&
         layout->content_width <= layout->canvas_width - layout->content_x &&
         layout->content_height <= layout->canvas_height - layout->content_y;
}

auris_status_t auris_projection_build(
    const auris_view_t *camera, const auris_projection_layout_t *layout,
    const auris_vec3f_t *directions, size_t count,
    auris_projected_point_t *points, size_t capacity) {
  if (camera == NULL || camera->width < 2U || camera->height < 2U ||
      !layout_valid(layout) || directions == NULL || points == NULL ||
      count == 0U) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  if (count > capacity) {
    return AURIS_ERROR_CAPACITY;
  }
  /* Inverse of Module 11's pixel-centre resize, calculated once. */
  const float x_scale = (float)layout->content_width / (float)camera->width;
  const float y_scale = (float)layout->content_height / (float)camera->height;
  for (size_t q = 0U; q < count; ++q) {
    float u, v;
    bool visible;
    const auris_status_t status =
        auris_view_project(camera, directions[q], &u, &v, &visible);
    if (status != AURIS_OK) {
      return status;
    }
    points[q] = (auris_projected_point_t){0};
    if (!visible) {
      continue;
    }
    points[q].x = (float)layout->content_x + (u + 0.5f) * x_scale - 0.5f;
    points[q].y = (float)layout->content_y + (v + 0.5f) * y_scale - 0.5f;
    points[q].visible = true;
  }
  return AURIS_OK;
}
