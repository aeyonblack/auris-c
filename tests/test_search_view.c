#include "auris_srp.h"
#include "auris_track.h"
#include "auris_view.h"
#include "imager_audio.h"

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

static auris_vec3f_t grid[IMAGER_DIRECTIONS], old[65U * 49U];

static double degrees(double radians) { return radians * 57.29577951308232; }
static double right_of(auris_vec3f_t d) {
  return degrees(atan2((double)d.x_m, (double)d.z_m));
}
static double up_of(auris_vec3f_t d) {
  return degrees(atan2((double)d.y_m, (double)d.z_m));
}

int main(void) {
  const auris_view_t view = imager_search_view();
  CHECK(IMAGER_DIRECTIONS == IMAGER_COLUMNS * IMAGER_ROWS);
  CHECK(IMAGER_DIRECTIONS <= AURIS_SRP_MAX_DIRECTIONS &&
        IMAGER_DIRECTIONS <= AURIS_TRACK_MAX_CELLS);
  CHECK(IMAGER_COLUMNS % 2U == 1U && IMAGER_ROWS % 2U == 1U);
  CHECK(auris_view_grid(&view, IMAGER_COLUMNS, IMAGER_ROWS, grid,
                        IMAGER_DIRECTIONS) == AURIS_OK);
  const size_t mid_row = IMAGER_ROWS / 2U, mid_column = IMAGER_COLUMNS / 2U;
  const auris_vec3f_t centre = grid[mid_row * IMAGER_COLUMNS + mid_column];
  CHECK(fabsf(centre.x_m) < 1.0e-6f && fabsf(centre.y_m) < 1.0e-6f &&
        fabsf(centre.z_m - 1.0f) < 1.0e-6f);

  /* The camera picture is 62.2 x 48.8 degrees. Sideways the search is as
     it was, 2.6 degrees past each edge. */
  const double right = right_of(grid[mid_row * IMAGER_COLUMNS +
                                     IMAGER_COLUMNS - 1U]);
  CHECK(fabs(right - 33.65) < 0.05);
  CHECK(fabs(right_of(grid[mid_row * IMAGER_COLUMNS]) + right) < 1.0e-3);
  /* Above and below it reaches about 6 degrees past the picture, so a
     sound just outside is placed there and not on the picture's edge. */
  const double top = up_of(grid[mid_column]);
  const double bottom = up_of(grid[(IMAGER_ROWS - 1U) * IMAGER_COLUMNS +
                                   mid_column]);
  printf("search: +-%.1f deg sideways, +-%.1f deg up and down, %u cells\n",
         right, top, (unsigned)IMAGER_DIRECTIONS);
  CHECK(top >= 24.4 + 5.0 && top <= 24.4 + 8.0);
  CHECK(fabs(top + bottom) < 1.0e-3);

  /* Cells are the same size both ways. */
  const double across =
      right_of(grid[mid_row * IMAGER_COLUMNS + mid_column + 1U]);
  const double upward =
      up_of(grid[(mid_row - 1U) * IMAGER_COLUMNS + mid_column]);
  CHECK(across > 1.15 && across < 1.25);
  CHECK(fabs(across - upward) < 0.01 * across);

  /* The earlier 65 x 49 grid is the middle of this one: nothing inside
     the picture has moved. */
  const auris_view_t before = {
      .width = 640U, .height = 480U, .fx = 480.0f, .fy = 480.0f, .cx = 319.5f,
      .cy = 239.5f, .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  CHECK(auris_view_grid(&before, 65U, 49U, old, 65U * 49U) == AURIS_OK);
  CHECK(IMAGER_COLUMNS == 65U && IMAGER_ROWS >= 49U);
  const size_t added = (IMAGER_ROWS - 49U) / 2U;
  double worst = 0.0;
  for (size_t row = 0U; row < 49U; ++row) {
    for (size_t column = 0U; column < 65U; ++column) {
      const auris_vec3f_t a = old[row * 65U + column];
      const auris_vec3f_t b = grid[(row + added) * IMAGER_COLUMNS + column];
      const double dot = (double)a.x_m * b.x_m + (double)a.y_m * b.y_m +
                         (double)a.z_m * b.z_m;
      worst = fmax(worst, degrees(acos(fmin(1.0, dot))));
    }
  }
  CHECK(worst < 0.05);
  puts("search view: ok");
  return EXIT_SUCCESS;
}
