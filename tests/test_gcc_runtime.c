#include "auris_activity.h"
#include "auris_array_configs.h"
#include "auris_spectrum.h"
#include "auris_srp.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

static auris_analysis_frame_t frame;
static auris_spectrum_t spectrum;
static auris_gcc_t gcc;
static float fine[AURIS_GCC_MAX_SIZE];

static int argmax_fine(const float *curve, size_t fine_size, int window) {
  int best_lag = 0;
  float best = -INFINITY;
  for (int lag = -window; lag <= window; ++lag) {
    const float v = curve[(size_t)(lag < 0 ? (int)fine_size + lag : lag)];
    if (v > best) {
      best = v;
      best_lag = lag;
    }
  }
  return best_lag;
}

static int test_create(void) {
  auris_fft_t *fft = auris_fft_create();
  CHECK(fft != NULL);
  CHECK(auris_fft_size(fft) == AURIS_ANALYSIS_SIZE &&
        auris_fft_upsample(fft) == AURIS_GCC_UPSAMPLE &&
        auris_fft_curve_size(fft) == AURIS_GCC_SIZE);
  auris_fft_destroy(fft);
  CHECK(auris_fft_create_size(1536U, 4U) == NULL);
  CHECK(auris_fft_create_size(4096U, 4U) == NULL);
  CHECK(auris_fft_create_size(1024U, 3U) == NULL);
  CHECK(auris_fft_create_size(1024U, 16U) == NULL);
  CHECK(auris_fft_size(NULL) == 0U && auris_fft_curve_size(NULL) == 0U);
  CHECK(auris_capture_cmb()->gcc_upsample == 4U);
  CHECK(auris_capture_respeaker_usb()->gcc_upsample == 8U);
  return EXIT_SUCCESS;
}

/* Integer impulse delays at every size and refinement factor. */
static int test_impulses(void) {
  const size_t sizes[3] = {512U, 1024U, 2048U};
  const size_t ups[4] = {1U, 2U, 4U, 8U};
  for (size_t s = 0U; s < 3U; ++s) {
    for (size_t u = 0U; u < 4U; ++u) {
      const size_t n = sizes[s];
      auris_fft_t *fft = auris_fft_create_size(n, ups[u]);
      CHECK(fft != NULL);
      for (int delay = -14; delay <= 14; delay += 7) {
        frame = (auris_analysis_frame_t){.sample_rate = 48000U,
                                         .channels = 2U, .size = n};
        frame.samples[0][n / 2U] = 0.5f;
        frame.samples[1][(size_t)((int)(n / 2U) + delay)] = 0.5f;
        CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
        CHECK(spectrum.size == n);
        CHECK(auris_gcc_phat(fft, &spectrum, 0U, 1U, 15, &gcc) == AURIS_OK);
        CHECK(gcc.valid && gcc.size == n && gcc.lag_samples == delay);
        /* DC and Nyquist are excluded: (N - 2) of N bins contribute. */
        CHECK(fabsf(gcc.peak - (float)(n - 2U) / (float)n) < 1e-4f);
        bool valid = false;
        CHECK(auris_gcc_phat_curve(fft, &spectrum, 0U, 1U, fine, &valid) ==
              AURIS_OK && valid);
        const size_t fs = n * ups[u];
        for (size_t k = 0U; k < n; ++k) { /* the fine curve passes through */
          CHECK(fabsf(fine[k * ups[u]] - gcc.correlation[k]) < 1e-4f);
        }
        CHECK(argmax_fine(fine, fs, 15 * (int)ups[u]) == delay * (int)ups[u]);
        CHECK(auris_gcc_phat(fft, &spectrum, 1U, 0U, 15, &gcc) == AURIS_OK &&
              gcc.lag_samples == -delay);
      }
      auris_fft_destroy(fft);
    }
  }
  return EXIT_SUCCESS;
}

/* A fractional delay through real preprocessing at the CMB's 1024/4x. */
static int test_fractional(void) {
  auris_fft_t *fft = auris_fft_create_size(1024U, 4U);
  static auris_preprocess_t prep;
  CHECK(fft != NULL && auris_preprocess_init_size(&prep, 1024U) == AURIS_OK);
  const double delays[4] = {2.25, -5.5, 9.75, 0.0};
  for (size_t d = 0U; d < 4U; ++d) {
    frame = (auris_analysis_frame_t){.sample_rate = 48000U, .channels = 2U,
                                     .size = 1024U};
    for (size_t n = 0U; n < 1024U; ++n) {
      double a = 0.2, b = 0.2;
      for (size_t k = 11U; k < 500U; k += 3U) { /* 515 Hz .. 23 kHz tones */
        const double w = 6.283185307179586 * (double)k / 1024.0;
        a += 0.004 * sin(w * (double)n + (double)(k * k));
        b += 0.004 * sin(w * ((double)n - delays[d]) + (double)(k * k));
      }
      frame.samples[0][n] = (float)a;
      frame.samples[1][n] = (float)b;
    }
    CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
    CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
    bool valid = false;
    CHECK(auris_gcc_phat_curve(fft, &spectrum, 0U, 1U, fine, &valid) ==
          AURIS_OK && valid);
    const int lag = argmax_fine(fine, 4096U, 60);
    CHECK(fabs((double)lag / 4.0 - delays[d]) <= 0.25);
  }
  auris_fft_destroy(fft);
  return EXIT_SUCCESS;
}

/* Plane waves across the CMB ring: sign and size of the MIC1-MIC5 lag. */
static int test_cmb_geometry(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_fft_t *fft = auris_fft_create_size(1024U, 4U);
  CHECK(fft != NULL);
  const double bearings[4] = {0.0, 180.0, 90.0, 30.0};
  for (size_t b = 0U; b < 4U; ++b) {
    const double angle = bearings[b] * 0.017453292519943295;
    const double ux = cos(angle), uy = sin(angle);
    spectrum = (auris_spectrum_t){.sample_rate = 48000U, .channels = 8U,
                                  .size = 1024U};
    for (size_t m = 0U; m < 8U; ++m) {
      const auris_vec3f_t p = cmb->positions[m];
      const double advance = 48000.0 / 343.0 *
                             ((double)p.x_m * ux + (double)p.y_m * uy);
      for (size_t k = 1U; k < 512U; ++k) {
        const double phase = 6.283185307179586 * (double)k * advance / 1024.0;
        spectrum.bins[m][k][0] = (float)cos(phase);
        spectrum.bins[m][k][1] = (float)sin(phase);
      }
    }
    bool valid = false;
    CHECK(auris_gcc_phat_curve(fft, &spectrum, 0U, 4U, fine, &valid) ==
          AURIS_OK && valid);
    const double expected = -48000.0 / 343.0 * (-0.104 * ux);
    const int lag = argmax_fine(fine, 4096U, 60);
    CHECK(fabs((double)lag / 4.0 - expected) <= 0.25);
  }
  /* Source at 0 deg (beside MIC1): MIC5 hears it ~14.55 samples later. */
  CHECK(fabs(-48000.0 / 343.0 * -0.104 - 14.5539) < 1e-3);
  auris_fft_destroy(fft);
  return EXIT_SUCCESS;
}

/* The band mask, and why GCC needs it at 48 kHz: tones only up to ~7 kHz,
   so most of the 24 kHz spectrum holds no signal. */
static int test_band(void) {
  spectrum = (auris_spectrum_t){.sample_rate = 48000U, .channels = 2U,
                                .size = 1024U};
  for (size_t c = 0U; c < 2U; ++c) {
    for (size_t k = 0U; k < 513U; ++k) {
      spectrum.bins[c][k][0] = 1.0f;
      spectrum.bins[c][k][1] = -1.0f;
    }
  }
  CHECK(auris_spectrum_band(&spectrum, 500.0f, 3000.0f) == AURIS_OK);
  /* 46.875 Hz bins: 500 Hz -> bin 11 (515.6 Hz), 3000 Hz -> bin 64. */
  for (size_t c = 0U; c < 2U; ++c) {
    for (size_t k = 0U; k < 513U; ++k) {
      const bool kept = k >= 11U && k <= 64U;
      CHECK((spectrum.bins[c][k][0] == 1.0f) == kept);
      CHECK((spectrum.bins[c][k][1] == -1.0f) == kept);
    }
  }
  CHECK(auris_spectrum_band(&spectrum, 0.0f, 48000.0f) == AURIS_OK);
  CHECK(spectrum.bins[0][0][0] == 0.0f && spectrum.bins[0][512][0] == 0.0f);
  CHECK(auris_spectrum_band(&spectrum, 3000.0f, 500.0f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_spectrum_band(NULL, 500.0f, 3000.0f) ==
        AURIS_ERROR_INVALID_ARGUMENT);

  /* Multitone 156 Hz .. 7 kHz, MIC B exactly 3 samples late, at 48 kHz. */
  auris_fft_t *fft = auris_fft_create_size(1024U, 4U);
  static auris_preprocess_t prep;
  CHECK(fft != NULL && auris_preprocess_init_size(&prep, 1024U) == AURIS_OK);
  frame = (auris_analysis_frame_t){.sample_rate = 48000U, .channels = 2U,
                                   .size = 1024U};
  for (size_t n = 0U; n < 1024U; ++n) {
    double a = 0.2, b = 0.2;
    for (size_t k = 5U; k < 225U; k += 7U) {
      const double w = 6.283185307179586 * (double)k * 31.25 / 48000.0;
      a += 0.006 * sin(w * (double)n + (double)(k * k));
      b += 0.006 * sin(w * ((double)n - 3.0) + (double)(k * k));
    }
    frame.samples[0][n] = (float)a;
    frame.samples[1][n] = (float)b;
  }
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  CHECK(auris_spectrum_band(&spectrum, 500.0f, 3000.0f) == AURIS_OK);
  bool valid = false;
  CHECK(auris_gcc_phat_curve(fft, &spectrum, 0U, 1U, fine, &valid) ==
        AURIS_OK && valid);
  CHECK(argmax_fine(fine, 4096U, 60) == 12); /* 3.00 samples */
  auris_fft_destroy(fft);
  return EXIT_SUCCESS;
}

static int test_guards(void) {
  auris_fft_t *small = auris_fft_create();
  auris_fft_t *large = auris_fft_create_size(1024U, 4U);
  CHECK(small != NULL && large != NULL);
  frame = (auris_analysis_frame_t){.sample_rate = 48000U, .channels = 8U,
                                   .size = 1024U};
  CHECK(auris_spectrum_apply(small, &frame, &spectrum) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_spectrum_apply(large, &frame, &spectrum) == AURIS_OK);
  /* A 1024-sample spectrum cannot be correlated by a 512-sample transform. */
  CHECK(auris_gcc_phat(small, &spectrum, 0U, 1U, 8, &gcc) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat(large, &spectrum, 0U, 1U, 512, &gcc) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat(large, &spectrum, 0U, 0U, 8, &gcc) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  bool valid;
  CHECK(auris_gcc_phat_curve(small, &spectrum, 0U, 1U, fine, &valid) ==
        AURIS_ERROR_INVALID_ARGUMENT);

  /* Stages not generalized yet refuse run-time sizes instead of misreading. */
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_vec3f_t ring[8];
  for (size_t q = 0U; q < 8U; ++q) {
    const float a = (float)q * 0.785398f;
    ring[q] = (auris_vec3f_t){cosf(a), sinf(a), 0.0f};
  }
  auris_srp_t *srp = auris_srp_create(cmb, 48000U, ring, 8U);
  CHECK(srp != NULL);
  static auris_srp_result_t result;
  spectrum.sample_rate = 48000U;
  CHECK(auris_srp_apply(srp, large, &spectrum, &result) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  auris_srp_destroy(srp);
  auris_activity_config_t config = auris_activity_defaults();
  auris_activity_t activity = {0};
  CHECK(auris_activity_init(&activity, &config, 48000U, 8U, 383.625f) ==
        AURIS_OK);
  auris_activity_result_t gate;
  CHECK(auris_activity_apply(&activity, &spectrum, &gate) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  auris_fft_destroy(small);
  auris_fft_destroy(large);
  return EXIT_SUCCESS;
}

int main(void) {
  if (test_create() != EXIT_SUCCESS || test_impulses() != EXIT_SUCCESS ||
      test_fractional() != EXIT_SUCCESS || test_cmb_geometry() != EXIT_SUCCESS ||
      test_band() != EXIT_SUCCESS ||
      test_guards() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("runtime GCC tests: ok");
  return EXIT_SUCCESS;
}
