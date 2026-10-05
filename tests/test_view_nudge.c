#include "auris_view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

static auris_view_t camera(void) {
  return (auris_view_t){.width = 1640U, .height = 1232U,
                        .fx = 1358.5007f, .fy = 1356.8622f,
                        .cx = 819.5f, .cy = 615.5f,
                        .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

static float radians(float degrees) { return degrees * 0.017453292f; }

int main(void) {
  const auris_vec3f_t ahead = {0.0f, 0.0f, 1.0f};
  float u = 0.0f, v = 0.0f;
  bool visible = false;

  /* Pan moves what the array hears straight ahead to the right. */
  auris_view_t view = camera();
  CHECK(auris_view_nudge(&view, 3.0f, 0.0f) == AURIS_OK);
  CHECK(auris_view_project(&view, ahead, &u, &v, &visible) == AURIS_OK);
  CHECK(visible);
  CHECK(fabsf(u - (view.cx + view.fx * tanf(radians(3.0f)))) < 0.05f);
  CHECK(fabsf(v - view.cy) < 0.05f);

  /* Tilt moves it up: a smaller row. */
  view = camera();
  CHECK(auris_view_nudge(&view, 0.0f, 2.0f) == AURIS_OK);
  CHECK(auris_view_project(&view, ahead, &u, &v, &visible) == AURIS_OK);
  CHECK(visible);
  CHECK(fabsf(u - view.cx) < 0.05f);
  CHECK(fabsf(v - (view.cy - view.fy * tanf(radians(2.0f)))) < 0.05f);

  /* Negative values go left and down; both at once. */
  view = camera();
  CHECK(auris_view_nudge(&view, -4.0f, -1.5f) == AURIS_OK);
  CHECK(auris_view_project(&view, ahead, &u, &v, &visible) == AURIS_OK);
  CHECK(visible && u < view.cx - 90.0f && v > view.cy + 30.0f);
  CHECK(fabsf(v - (view.cy + view.fy * tanf(radians(1.5f)))) < 0.05f);

  /* Zero changes nothing. */
  view = camera();
  CHECK(auris_view_nudge(&view, 0.0f, 0.0f) == AURIS_OK);
  for (size_t i = 0U; i < 3U; ++i)
    for (size_t j = 0U; j < 3U; ++j)
      CHECK(fabsf(view.rotation[i][j] - (i == j ? 1.0f : 0.0f)) < 1.0e-6f);

  /* A nudge adds to a rotation that is already there, and undoing it
     gives that rotation back. */
  view = camera();
  CHECK(auris_view_nudge(&view, 5.0f, 0.0f) == AURIS_OK);
  const auris_view_t turned = view;
  CHECK(auris_view_nudge(&view, 2.0f, 0.0f) == AURIS_OK);
  CHECK(auris_view_project(&view, ahead, &u, &v, &visible) == AURIS_OK);
  CHECK(fabsf(u - (view.cx + view.fx * tanf(radians(7.0f)))) < 0.1f);
  CHECK(auris_view_nudge(&view, -2.0f, 0.0f) == AURIS_OK);
  for (size_t i = 0U; i < 3U; ++i)
    for (size_t j = 0U; j < 3U; ++j)
      CHECK(fabsf(view.rotation[i][j] - turned.rotation[i][j]) < 1.0e-5f);

  /* Bad arguments leave the view alone. */
  view = camera();
  CHECK(auris_view_nudge(NULL, 1.0f, 1.0f) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_view_nudge(&view, NAN, 0.0f) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_view_nudge(&view, 0.0f, 30.5f) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_view_nudge(&view, -31.0f, 0.0f) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(view.rotation[0][0] == 1.0f && view.rotation[0][2] == 0.0f);
  view.fx = 0.0f;
  CHECK(auris_view_nudge(&view, 1.0f, 0.0f) == AURIS_ERROR_INVALID_ARGUMENT);
  puts("view nudge: ok");
  return EXIT_SUCCESS;
}
