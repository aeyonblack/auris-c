#include "auris_overlay.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

/* A 3 x 3 grid drawn onto a 5 x 5 picture inside a 7 x 5 canvas. */
static auris_overlay_config_t small(void) {
  const auris_view_t view = {.width = 5U, .height = 5U, .fx = 4, .fy = 4,
                             .cx = 2, .cy = 2,
                             .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  return (auris_overlay_config_t){.camera = view, .search = view,
                                  .layout = {7U, 5U, 1U, 0U, 5U, 5U},
                                  .columns = 3U, .rows = 3U, .cutoff = 0.5f,
                                  .opacity = 1.0f, .mark_peak = false};
}

static int test_strength(void) {
  uint8_t full[130], half[130], none[130], old[130];
  const float scores[9] = {0, 5, 10, 0, 5, 10, 0, 5, 10};
  auris_overlay_config_t c = small();
  auris_overlay_t *s = NULL;
  CHECK(auris_overlay_open(&s, &c) == 0);
  auris_rgb24_t frame = {full, 7U, 5U, 26U, sizeof(full)};
  memset(full, 40, sizeof(full));
  CHECK(auris_overlay_draw(s, scores, 9U, 2.0f, 1.0f, 1.0f, &frame) == 0);
  /* Full strength is exactly what auris_overlay_apply draws. */
  frame.data = old;
  memset(old, 40, sizeof(old));
  CHECK(auris_overlay_apply(s, scores, 9U, 5U, true, &frame) == 0);
  CHECK(memcmp(full, old, sizeof(full)) == 0);
  /* Zero strength draws nothing, and needs no scores. */
  frame.data = none;
  memset(none, 40, sizeof(none));
  CHECK(auris_overlay_draw(s, scores, 9U, 2.0f, 1.0f, 0.0f, &frame) == 0);
  CHECK(auris_overlay_draw(s, NULL, 0U, 0.0f, 0.0f, 0.0f, &frame) == 0);
  for (size_t i = 0U; i < sizeof(none); ++i) {
    CHECK(none[i] == 40U);
  }
  /* Half strength moves every pixel half as far from the picture. */
  frame.data = half;
  memset(half, 40, sizeof(half));
  CHECK(auris_overlay_draw(s, scores, 9U, 2.0f, 1.0f, 0.5f, &frame) == 0);
  size_t changed = 0U;
  for (size_t i = 0U; i < sizeof(half); ++i) {
    const double expected = 40.0 + ((double)full[i] - 40.0) / 2.0;
    CHECK(fabs((double)half[i] - expected) <= 1.5);
    changed += full[i] != 40U ? 1U : 0U;
  }
  CHECK(changed > 20U); /* the blob was really there */

  CHECK(auris_overlay_draw(s, scores, 9U, 2.0f, 1.0f, 1.1f, &frame) == -EINVAL);
  CHECK(auris_overlay_draw(s, scores, 9U, 2.0f, 1.0f, -0.1f, &frame) ==
        -EINVAL);
  CHECK(auris_overlay_draw(s, scores, 9U, 2.0f, 1.0f, NAN, &frame) == -EINVAL);
  CHECK(auris_overlay_draw(s, scores, 9U, 2.1f, 1.0f, 1.0f, &frame) == -EINVAL);
  CHECK(auris_overlay_draw(s, scores, 9U, 1.0f, -0.1f, 1.0f, &frame) ==
        -EINVAL);
  CHECK(auris_overlay_draw(s, scores, 8U, 1.0f, 1.0f, 1.0f, &frame) == -EINVAL);
  CHECK(auris_overlay_draw(s, NULL, 9U, 1.0f, 1.0f, 1.0f, &frame) == -EINVAL);
  CHECK(auris_overlay_draw(NULL, scores, 9U, 1.0f, 1.0f, 1.0f, &frame) ==
        -EINVAL);
  CHECK(auris_overlay_draw(s, scores, 9U, 1.0f, 1.0f, 1.0f, NULL) == -EINVAL);
  auris_overlay_close(s);
  return EXIT_SUCCESS;
}

/* A 3 x 3 grid on a 17 x 17 picture: cells are 8 pixels apart, so an
   estimate half-way between two cells is 4 pixels from each. */
static bool cyan(const uint8_t *bytes, size_t x, size_t y) {
  const uint8_t *p = bytes + y * 51U + x * 3U;
  return p[0] == 0U && p[1] == 255U && p[2] == 255U;
}

static int test_marker(void) {
  static uint8_t bytes[17U * 51U];
  const auris_view_t view = {.width = 17U, .height = 17U, .fx = 16, .fy = 16,
                             .cx = 8, .cy = 8,
                             .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  const auris_overlay_config_t c = {.camera = view, .search = view,
                                    .layout = {17U, 17U, 0U, 0U, 17U, 17U},
                                    .columns = 3U, .rows = 3U, .cutoff = 0.5f,
                                    .opacity = 0.0f, .mark_peak = true};
  /* Opacity 0 draws nothing at all, the marker included. */
  const float scores[9] = {0, 0, 0, 0, 1, 0, 0, 0, 0};
  auris_overlay_t *s = NULL;
  CHECK(auris_overlay_open(&s, &c) == 0);
  auris_rgb24_t frame = {bytes, 17U, 17U, 51U, sizeof(bytes)};
  memset(bytes, 40, sizeof(bytes));
  CHECK(auris_overlay_draw(s, scores, 9U, 1.0f, 1.0f, 1.0f, &frame) == 0);
  CHECK(!cyan(bytes, 8U, 8U));
  /* A blob too faint to see (opacity 0.004), so only the marker shows. */
  CHECK(auris_overlay_set_style(s, 0.5f, 0.004f) == 0);
  CHECK(auris_overlay_draw(s, scores, 9U, 1.0f, 1.0f, 1.0f, &frame) == 0);
  CHECK(cyan(bytes, 8U, 8U) && cyan(bytes, 8U, 5U) && cyan(bytes, 11U, 8U));
  CHECK(!cyan(bytes, 12U, 5U) && !cyan(bytes, 13U, 8U));
  /* Half a cell to the right: the cross is centred 4 pixels further. */
  memset(bytes, 40, sizeof(bytes));
  CHECK(auris_overlay_draw(s, scores, 9U, 1.5f, 1.0f, 1.0f, &frame) == 0);
  CHECK(cyan(bytes, 12U, 8U) && cyan(bytes, 12U, 5U) && cyan(bytes, 12U, 11U));
  CHECK(!cyan(bytes, 8U, 5U) && cyan(bytes, 8U, 8U)); /* (8, 8) is its arm */
  CHECK(!cyan(bytes, 7U, 8U));
  /* A quarter of a cell down and right. */
  memset(bytes, 40, sizeof(bytes));
  CHECK(auris_overlay_draw(s, scores, 9U, 1.25f, 1.25f, 1.0f, &frame) == 0);
  CHECK(cyan(bytes, 10U, 10U) && cyan(bytes, 10U, 7U) && !cyan(bytes, 8U, 8U));
  /* The last column and row are valid positions. */
  memset(bytes, 40, sizeof(bytes));
  CHECK(auris_overlay_draw(s, scores, 9U, 2.0f, 2.0f, 1.0f, &frame) == 0);
  CHECK(cyan(bytes, 16U, 16U) && cyan(bytes, 13U, 16U));
  /* A faded estimate has a faded marker. */
  memset(bytes, 40, sizeof(bytes));
  CHECK(auris_overlay_draw(s, scores, 9U, 1.0f, 1.0f, 0.5f, &frame) == 0);
  const uint8_t *p = bytes + 8U * 51U + 8U * 3U;
  CHECK(!cyan(bytes, 8U, 8U));
  CHECK(p[0] >= 18U && p[0] <= 22U && p[1] >= 146U && p[1] <= 150U);
  auris_overlay_close(s);
  return EXIT_SUCCESS;
}

int main(void) {
  if (test_strength() != EXIT_SUCCESS || test_marker() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("overlay draw tests: ok");
  return EXIT_SUCCESS;
}
