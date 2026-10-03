#include "auris_array_configs.h"
#include "auris_srp.h"
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

#define COLUMNS 65U
#define ROWS 49U
#define COUNT (COLUMNS * ROWS)

static const auris_view_t view = {
    .width = 640U, .height = 480U, .fx = 480.0f, .fy = 480.0f, .cx = 319.5f,
    .cy = 239.5f, .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

static auris_vec3f_t grid[COUNT];
static auris_spectrum_t spectrum;
static auris_srp_result_t dense, fast;

static double angle_deg(auris_vec3f_t a, auris_vec3f_t b) {
  double d = (double)a.x_m * b.x_m + (double)a.y_m * b.y_m +
             (double)a.z_m * b.z_m;
  d = d > 1.0 ? 1.0 : (d < -1.0 ? -1.0 : d);
  return acos(d) * 57.29577951308232;
}

/* Unit-phase spectra of a far source along `ray`, 500-3000 Hz only. */
static void source(const auris_array_t *array, auris_vec3f_t ray) {
  spectrum = (auris_spectrum_t){.sample_rate = 48000U, .channels = 8U,
                                .size = 1024U};
  for (size_t m = 0U; m < 8U; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance = 48000.0 / 343.0 *
                           ((double)p.x_m * ray.x_m + (double)p.y_m * ray.y_m +
                            (double)p.z_m * ray.z_m);
    for (size_t k = 11U; k <= 64U; ++k) {
      const double phase = 6.283185307179586 * (double)k * advance / 1024.0;
      spectrum.bins[m][k][0] = (float)cos(phase);
      spectrum.bins[m][k][1] = (float)sin(phase);
    }
  }
}

int main(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  CHECK(auris_view_grid(&view, COLUMNS, ROWS, grid, COUNT) == AURIS_OK);
  auris_fft_t *fft = auris_fft_create_size(1024U, 4U);
  auris_srp_t *exhaustive =
      auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, grid, COUNT);
  auris_srp_t *coarse =
      auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, grid, COUNT);
  CHECK(fft != NULL && exhaustive != NULL && coarse != NULL);
  CHECK(auris_srp_set_grid(exhaustive, COLUMNS, ROWS, 1U) == AURIS_OK);
  CHECK(auris_srp_set_grid(coarse, COLUMNS, ROWS, 4U) == AURIS_OK);

  /* The centre of the view is straight ahead: every pair's lag is zero. */
  source(cmb, grid[24U * COLUMNS + 32U]);
  CHECK(auris_srp_apply(exhaustive, fft, &spectrum, &dense) == AURIS_OK);
  CHECK(dense.valid && dense.best == 24U * COLUMNS + 32U);
  for (size_t p = 0U; p < 28U; ++p) {
    CHECK(fabsf(dense.predicted_lags[p]) < 1e-5f);
  }

  /* Every 4th grid direction: exhaustive search lands on the source's own
     cell, and coarse-to-fine search agrees with it. */
  double worst_fast = 0.0;
  for (size_t row = 0U; row < ROWS; row += 4U) {
    for (size_t col = 0U; col < COLUMNS; col += 4U) {
      const size_t q = row * COLUMNS + col;
      source(cmb, grid[q]);
      CHECK(auris_srp_apply(exhaustive, fft, &spectrum, &dense) == AURIS_OK);
      CHECK(auris_srp_apply(coarse, fft, &spectrum, &fast) == AURIS_OK);
      CHECK(dense.valid && fast.valid);
      CHECK(angle_deg(grid[dense.best], grid[q]) < 1.3);
      const double e = angle_deg(grid[fast.best], grid[q]);
      worst_fast = e > worst_fast ? e : worst_fast;
      CHECK(fast.evaluated <= 293U && dense.evaluated == COUNT);
    }
  }
  CHECK(worst_fast < 1.3);

  /* Off-grid directions, specified independently of the view. */
  const auris_vec3f_t rays[4] = {{0.2f, -0.3f, sqrtf(0.87f)},
                                 {-0.35f, 0.1f, sqrtf(1.0f - 0.1325f)},
                                 {0.05f, 0.4f, sqrtf(1.0f - 0.1625f)},
                                 {-0.45f, -0.3f, sqrtf(1.0f - 0.2925f)}};
  for (size_t r = 0U; r < 4U; ++r) {
    source(cmb, rays[r]);
    CHECK(auris_srp_apply(exhaustive, fft, &spectrum, &dense) == AURIS_OK);
    CHECK(auris_srp_apply(coarse, fft, &spectrum, &fast) == AURIS_OK);
    CHECK(angle_deg(grid[dense.best], rays[r]) < 1.5);
    CHECK(angle_deg(grid[fast.best], rays[r]) < 1.5);
  }

  /* A flat array cannot tell front from back: the mirrored source wins at
     the same cell with the same score. */
  source(cmb, rays[0]);
  CHECK(auris_srp_apply(exhaustive, fft, &spectrum, &dense) == AURIS_OK);
  const size_t front = dense.best;
  const float front_peak = dense.peak;
  const auris_vec3f_t behind = {rays[0].x_m, rays[0].y_m, -rays[0].z_m};
  source(cmb, behind);
  CHECK(auris_srp_apply(exhaustive, fft, &spectrum, &dense) == AURIS_OK);
  CHECK(dense.best == front && fabsf(dense.peak - front_peak) < 1e-5f);

  auris_srp_destroy(exhaustive);
  auris_srp_destroy(coarse);
  auris_fft_destroy(fft);
  printf("CMB front heatmap tests: ok (coarse-to-fine worst %.2f deg)\n",
         worst_fast);
  return EXIT_SUCCESS;
}
