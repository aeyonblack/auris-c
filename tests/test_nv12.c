/* NV12 pictures (as the chip's image processor gives them) drawn into the
   RGB canvas: BT.601 video-range levels, one colour sample per 2 x 2 block,
   placed at an offset, and the mean brightness used for status lines. */
#include "auris_nv12.h"

#include <errno.h>
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

static int near(int a, int b) { return abs(a - b) <= 1; }

int main(void) {
  /* A 4 x 2 picture with a padded stride: the left 2 x 2 block is
     black and white rows, the right block pure red. */
  uint8_t y[2][8] = {{16, 16, 81, 81, 99, 99, 99, 99},
                     {235, 235, 81, 81, 99, 99, 99, 99}};
  uint8_t uv[1][8] = {{128, 128, 90, 240, 7, 7, 7, 7}};
  const auris_nv12_t pic = {&y[0][0], &uv[0][0], 4U, 2U, 8U, 8U};
  uint8_t rgb[3][6][3];
  memset(rgb, 0xAB, sizeof(rgb));
  const auris_rgb24_t canvas = {&rgb[0][0][0], 6U, 3U, 18U, sizeof(rgb)};
  CHECK(auris_nv12_to_rgb24(&pic, &canvas, 1U, 1U) == 0);
  /* Outside the picture the canvas is left alone. */
  CHECK(rgb[0][0][0] == 0xAB && rgb[0][5][2] == 0xAB && rgb[2][0][1] == 0xAB);
  CHECK(rgb[1][5][0] == 0xAB);
  /* Black (Y 16) and white (Y 235) with no colour. */
  CHECK(rgb[1][1][0] == 0 && rgb[1][1][1] == 0 && rgb[1][1][2] == 0);
  CHECK(rgb[1][2][0] == 0 && rgb[1][2][1] == 0 && rgb[1][2][2] == 0);
  CHECK(rgb[2][1][0] == 255 && rgb[2][1][1] == 255 && rgb[2][1][2] == 255);
  /* BT.601 red: Y 81, Cb 90, Cr 240. */
  for (int r = 1; r <= 2; ++r) {
    for (int c = 3; c <= 4; ++c) {
      CHECK(near(rgb[r][c][0], 255) && near(rgb[r][c][1], 0) &&
            near(rgb[r][c][2], 0));
    }
  }
  /* A mid grey and the levels between. */
  uint8_t gy[2][2] = {{126, 126}, {126, 126}};
  uint8_t guv[1][2] = {{128, 128}};
  const auris_nv12_t grey = {&gy[0][0], &guv[0][0], 2U, 2U, 2U, 2U};
  uint8_t out[2][2][3];
  const auris_rgb24_t small = {&out[0][0][0], 2U, 2U, 6U, sizeof(out)};
  CHECK(auris_nv12_to_rgb24(&grey, &small, 0U, 0U) == 0);
  CHECK(near(out[1][1][0], 128) && out[1][1][0] == out[1][1][1] &&
        out[1][1][1] == out[1][1][2]);

  /* Mean brightness, 0..1 of the video range, over the picture only. */
  float mean = -1.0f;
  CHECK(auris_nv12_mean(&pic, &mean) == 0);
  {
    const float expected =
        ((0.0f + 0.0f + 65.0f + 65.0f) + (219.0f + 219.0f + 65.0f + 65.0f)) /
        (8.0f * 219.0f);
    CHECK(mean > expected - 1.0e-4f && mean < expected + 1.0e-4f);
  }

  /* Pictures that do not fit, and bad arguments. */
  CHECK(auris_nv12_to_rgb24(&pic, &canvas, 3U, 1U) == -EINVAL);
  CHECK(auris_nv12_to_rgb24(&pic, &canvas, 1U, 2U) == -EINVAL);
  CHECK(auris_nv12_to_rgb24(NULL, &canvas, 0U, 0U) == -EINVAL);
  CHECK(auris_nv12_to_rgb24(&pic, NULL, 0U, 0U) == -EINVAL);
  const auris_rgb24_t short_buffer = {&rgb[0][0][0], 6U, 3U, 18U, 29U};
  CHECK(auris_nv12_to_rgb24(&pic, &short_buffer, 0U, 0U) == -EINVAL);
  const auris_nv12_t odd = {&y[0][0], &uv[0][0], 3U, 2U, 8U, 8U};
  CHECK(auris_nv12_to_rgb24(&odd, &canvas, 0U, 0U) == -EINVAL);
  const auris_nv12_t narrow = {&y[0][0], &uv[0][0], 4U, 2U, 3U, 8U};
  CHECK(auris_nv12_to_rgb24(&narrow, &canvas, 0U, 0U) == -EINVAL);
  CHECK(auris_nv12_mean(NULL, &mean) == -EINVAL);
  CHECK(auris_nv12_mean(&pic, NULL) == -EINVAL);
  puts("NV12 pictures draw into the canvas");
  return EXIT_SUCCESS;
}
