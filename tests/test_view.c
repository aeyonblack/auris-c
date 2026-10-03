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
      goto done;                                                               \
    }                                                                          \
  } while (0)

static void source_spectrum(const auris_array_t *array, auris_vec3f_t ray,
                            auris_spectrum_t *out) {
  *out = (auris_spectrum_t){.size = AURIS_ANALYSIS_SIZE, .sample_rate = 16000U,
                            .channels = array->microphone_count};
  for (size_t m = 0U; m < out->channels; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const float advance = 16000.0f / array->speed_of_sound_mps *
                          (p.x_m * ray.x_m + p.y_m * ray.y_m + p.z_m * ray.z_m);
    for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
      const float phase =
          6.283185307179586f * (float)k * advance / (float)AURIS_ANALYSIS_SIZE;
      out->bins[m][k][0] = cosf(phase);
      out->bins[m][k][1] = sinf(phase);
    }
  }
}

int main(void) {
  int result = EXIT_FAILURE;
  auris_fft_t *fft = auris_fft_create();
  auris_srp_t *srp = NULL;
  const auris_array_t *array = auris_config_respeaker_usb_4();
  auris_view_t view = {.width = 640U,
                       .height = 480U,
                       .fx = 480.0f,
                       .fy = 480.0f,
                       .cx = 319.5f,
                       .cy = 239.5f,
                       .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  auris_vec3f_t grid[825];
  auris_vec3f_t ray;
  CHECK(auris_view_grid(&view, 33U, 25U, grid, 824U) == AURIS_ERROR_CAPACITY);
  CHECK(auris_view_grid(&view, 33U, 25U, grid, 825U) == AURIS_OK);
  CHECK(fabsf(grid[412].x_m) < 1.0e-6f && fabsf(grid[412].y_m) < 1.0e-6f &&
        grid[412].z_m == 1.0f);
  CHECK(grid[0].x_m < 0.0f && grid[0].y_m > 0.0f);
  CHECK(grid[824].x_m > 0.0f && grid[824].y_m < 0.0f);
  for (size_t q = 0U; q < 825U; ++q) {
    const auris_vec3f_t s = grid[q];
    CHECK(s.z_m > 0.0f);
    CHECK(fabsf(s.x_m * s.x_m + s.y_m * s.y_m + s.z_m * s.z_m - 1.0f) <
          1.0e-5f);
  }
  CHECK(auris_view_ray(&view, NAN, 0.0f, &ray) == AURIS_ERROR_INVALID_ARGUMENT);
  view.fx = 0.0f;
  CHECK(auris_view_ray(&view, 0.0f, 0.0f, &ray) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  view.fx = 480.0f;
  view.rotation[0][0] = -1.0f; /* Reflection is not a mounting rotation. */
  CHECK(auris_view_ray(&view, 0.0f, 0.0f, &ray) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  view.rotation[0][0] = 1.0f;
  /* Forward axis tilted toward +X: centre ray must follow that axis. */
  view.rotation[0][0] = 0.8660254f;
  view.rotation[0][2] = 0.5f;
  view.rotation[2][0] = -0.5f;
  view.rotation[2][2] = 0.8660254f;
  CHECK(auris_view_ray(&view, 319.5f, 239.5f, &ray) == AURIS_OK);
  CHECK(fabsf(ray.x_m - 0.5f) < 1.0e-6f);

  srp = auris_srp_create(array, 16000U, grid, 825U);
  CHECK(fft != NULL && srp != NULL);
  auris_spectrum_t spectrum;
  auris_srp_result_t out;
  /* Every cell, including centre and corners, has a known source ray. */
  for (size_t q = 0U; q < 825U; ++q) {
    source_spectrum(array, grid[q], &spectrum);
    CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
    CHECK(out.valid);
    const auris_vec3f_t a = grid[q], b = grid[out.best];
    CHECK(a.x_m * b.x_m + a.y_m * b.y_m + a.z_m * b.z_m > 0.9993908f);
    if (q == 412U) {
      CHECK(out.best == 412U);
      for (size_t p = 0U; p < out.pairs; ++p) {
        CHECK(fabsf(out.predicted_lags[p]) < 1.0e-6f);
      }
    }
  }
  /* Off-grid direction, specified independently of the pixel-to-ray code. */
  ray = (auris_vec3f_t){0.2f, -0.3f, sqrtf(0.87f)};
  source_spectrum(array, ray, &spectrum);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
  const auris_vec3f_t best = grid[out.best];
  CHECK(out.valid &&
        best.x_m * ray.x_m + best.y_m * ray.y_m + best.z_m * ray.z_m >
            0.9986295f); /* Within three degrees. */
  const size_t front_best = out.best;
  const float front_peak = out.peak;
  ray.z_m = -ray.z_m;
  source_spectrum(array, ray, &spectrum);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
  CHECK(out.best == front_best && fabsf(out.peak - front_peak) < 1.0e-6f);
  spectrum = (auris_spectrum_t){.size = AURIS_ANALYSIS_SIZE, .sample_rate = 16000U, .channels = 4U};
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
  CHECK(!out.valid);
  puts("view tests: ok");
  result = EXIT_SUCCESS;
done:
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return result;
}
