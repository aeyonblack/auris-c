#include "auris_array_configs.h"
#include "auris_srp.h"

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

static auris_vec3f_t direction(float degrees) {
  const float a = degrees * 0.01745329251994329577f;
  return (auris_vec3f_t){cosf(a), sinf(a), 0.0f};
}

static float angle_error(size_t index, float target) {
  float error = fabsf((float)index - target);
  return error > 180.0f ? 360.0f - error : error;
}

int main(void) {
  int result = EXIT_FAILURE;
  const auris_array_t *array = auris_config_respeaker_usb_4();
  auris_fft_t *fft = auris_fft_create();
  auris_srp_t *srp = NULL;
  auris_vec3f_t grid[360];
  for (size_t q = 0U; q < 360U; ++q) {
    grid[q] = direction((float)q);
  }
  srp = auris_srp_create(array, 16000U, grid, 360U);
  CHECK(fft != NULL && srp != NULL);
  auris_spectrum_t spectrum = {.sample_rate = 16000U, .channels = 4U};
  auris_srp_result_t out;
  float fine[AURIS_GCC_SIZE];
  bool valid = false;
  auris_gcc_t coarse;

  /* An exact +3-sample spectral shift checks fine lag scaling. */
  for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
    const float phase = -6.283185307179586f * (float)k * 3.0f / 512.0f;
    spectrum.bins[0][k][0] = 1.0f;
    spectrum.bins[1][k][0] = cosf(phase);
    spectrum.bins[1][k][1] = sinf(phase);
  }
  CHECK(auris_gcc_phat_curve(fft, &spectrum, 0U, 1U, fine, &valid) == AURIS_OK);
  CHECK(valid && fabsf(fine[24] - 510.0f / 512.0f) < 1.0e-4f);
  CHECK(auris_gcc_phat(fft, &spectrum, 0U, 1U, 8, &coarse) == AURIS_OK);
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    CHECK(fabsf(fine[n * AURIS_GCC_UPSAMPLE] - coarse.correlation[n]) <
          1.0e-5f);
  }

  /* Sweep every grid direction with analytically shifted spectra. */
  for (size_t target = 0U; target < 360U; ++target) {
    const auris_vec3f_t u = grid[target];
    for (size_t m = 0U; m < 4U; ++m) {
      const auris_vec3f_t p = array->positions[m];
      const float advance = 16000.0f / array->speed_of_sound_mps *
                            (p.x_m * u.x_m + p.y_m * u.y_m);
      for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
        const float phase = 6.283185307179586f * (float)k * advance / 512.0f;
        spectrum.bins[m][k][0] = cosf(phase);
        spectrum.bins[m][k][1] = sinf(phase);
      }
    }
    CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
    CHECK(out.valid && angle_error(out.best, (float)target) <= 1.0f);
    float sum = 0.0f;
    for (size_t p = 0U; p < out.pairs; ++p) {
      sum += out.contributions[p];
    }
    CHECK(fabsf(sum / (float)out.pairs - out.peak) < 1.0e-6f);
    /* Right-side source: B=mic1 is earlier than A=mic0. */
    if (target == 0U) {
      CHECK(auris_gcc_phat(fft, &spectrum, 0U, 1U, 4, &coarse) == AURIS_OK);
      CHECK(coarse.lag_samples == -2);
      CHECK(out.predicted_lags[0] < -2.0f && out.predicted_lags[0] > -2.2f);
    }
  }

  /* Full time-domain path, including mean removal and windowing. */
  const float targets[] = {0.0f,   17.5f,  45.0f,  73.0f,  90.0f,
                           123.0f, 180.0f, 225.0f, 270.0f, 359.0f};
  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  for (size_t t = 0U; t < sizeof(targets) / sizeof(targets[0]); ++t) {
    const auris_vec3f_t u = direction(targets[t]);
    auris_analysis_frame_t frame = {.sample_rate = 16000U, .channels = 4U};
    for (size_t m = 0U; m < 4U; ++m) {
      const auris_vec3f_t p = array->positions[m];
      const double advance = (double)(p.x_m * u.x_m + p.y_m * u.y_m) /
                             (double)array->speed_of_sound_mps;
      for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
        double value = 0.2;
        for (size_t k = 5U; k < 225U; k += 7U) {
          const double f = (double)k * 16000.0 / 512.0;
          value += 0.006 *
                   sin(6.283185307179586 * f * ((double)n / 16000.0 + advance) +
                       (double)(k * k));
        }
        frame.samples[m][n] = (float)value;
      }
    }
    CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
    CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
    CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
    CHECK(out.valid && angle_error(out.best, targets[t]) <= 3.0f);
  }

  spectrum = (auris_spectrum_t){.sample_rate = 16000U, .channels = 4U};
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
  CHECK(!out.valid && out.peak == 0.0f);
  spectrum.sample_rate = 48000U;
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  grid[0] = (auris_vec3f_t){0.0f, 0.0f, 0.0f};
  CHECK(auris_srp_create(array, 16000U, grid, 360U) == NULL);
  puts("SRP tests: ok");
  result = EXIT_SUCCESS;
done:
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return result;
}
