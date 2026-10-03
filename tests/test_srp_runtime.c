#include "auris_array_configs.h"
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

static auris_vec3f_t ring[360];
static auris_spectrum_t spectrum;
static auris_srp_result_t out, first, second;
static auris_analysis_frame_t frame;

static float circular_error(size_t index, double target) {
  double e = fabs((double)index - target);
  return (float)(e > 180.0 ? 360.0 - e : e);
}

/* Unit-phase spectra of a far in-plane source at `degrees`, keeping only
   the bins of 500-3000 Hz at 48 kHz / 1024 (bins 11..64). */
static void plane_wave(const auris_array_t *array, double degrees,
                       uint64_t first_frame) {
  const double a = degrees * 0.017453292519943295;
  spectrum = (auris_spectrum_t){.sample_rate = 48000U, .channels = 8U,
                                .size = 1024U, .first_frame = first_frame};
  for (size_t m = 0U; m < 8U; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance =
        48000.0 / 343.0 * ((double)p.x_m * cos(a) + (double)p.y_m * sin(a));
    for (size_t k = 11U; k <= 64U; ++k) {
      const double phase = 6.283185307179586 * (double)k * advance / 1024.0;
      spectrum.bins[m][k][0] = (float)cos(phase);
      spectrum.bins[m][k][1] = (float)sin(phase);
    }
  }
}

static int test_create(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  CHECK(auris_srp_create_sized(cmb, 48000U, 1536U, 768U, 4U, ring, 360U) == NULL);
  CHECK(auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 3U, ring, 360U) == NULL);
  CHECK(auris_srp_create_sized(cmb, 48000U, 1024U, 64U, 4U, ring, 360U) == NULL);
  /* 64-sample frames: the 14.55-sample diameter lag exceeds half a frame. */
  CHECK(auris_srp_create_sized(cmb, 48000U, 64U, 128U, 4U, ring, 360U) == NULL);
  auris_srp_t *srp = auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, ring,
                                            360U);
  CHECK(srp != NULL);
  auris_srp_destroy(srp);
  return EXIT_SUCCESS;
}

/* Every bearing on the ring, band-limited analytic spectra, 28 pairs. */
static int test_ring_sweep(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_fft_t *fft = auris_fft_create_size(1024U, 4U);
  auris_srp_t *srp =
      auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, ring, 360U);
  CHECK(fft != NULL && srp != NULL);
  for (size_t target = 0U; target < 360U; ++target) {
    plane_wave(cmb, (double)target, 0U);
    CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
    CHECK(out.valid && out.pairs == 28U && out.directions == 360U);
    CHECK(circular_error(out.best, (double)target) <= 1.0f);
    float sum = 0.0f;
    for (size_t p = 0U; p < out.pairs; ++p) sum += out.contributions[p];
    CHECK(fabsf(sum / 28.0f - out.peak) < 1e-5f);
  }
  /* Source at 0 deg (beside MIC1): pair (0,4) = MIC1-MIC5, MIC5 later. */
  plane_wave(cmb, 0.0, 0U);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
  CHECK(out.best == 0U);
  CHECK(fabsf(out.predicted_lags[3] - 14.554f) < 0.01f);
  /* A fractional bearing selects a neighbouring grid direction. */
  plane_wave(cmb, 17.5, 0U);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
  CHECK(out.best == 17U || out.best == 18U);
  /* Silence: no usable map. */
  spectrum = (auris_spectrum_t){.sample_rate = 48000U, .channels = 8U,
                                .size = 1024U};
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK && !out.valid);
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return EXIT_SUCCESS;
}

/* The full time-domain path at 48 kHz with fractional delays. */
static int test_time_domain(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_fft_t *fft = auris_fft_create_size(1024U, 4U);
  auris_srp_t *srp =
      auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, ring, 360U);
  static auris_preprocess_t prep;
  CHECK(fft != NULL && srp != NULL &&
        auris_preprocess_init_size(&prep, 1024U) == AURIS_OK);
  const double targets[7] = {0.0, 17.5, 45.0, 90.0, 133.0, 225.0, 300.0};
  for (size_t t = 0U; t < 7U; ++t) {
    const double a = targets[t] * 0.017453292519943295;
    frame = (auris_analysis_frame_t){.sample_rate = 48000U, .channels = 8U,
                                     .size = 1024U};
    for (size_t m = 0U; m < 8U; ++m) {
      const auris_vec3f_t p = cmb->positions[m];
      const double advance =
          ((double)p.x_m * cos(a) + (double)p.y_m * sin(a)) / 343.0;
      for (size_t n = 0U; n < 1024U; ++n) {
        double v = 0.2;
        for (size_t k = 5U; k < 225U; k += 7U) {
          v += 0.006 * sin(6.283185307179586 * (double)k * 31.25 *
                               ((double)n / 48000.0 + advance) +
                           (double)(k * k));
        }
        frame.samples[m][n] = (float)v;
      }
    }
    CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
    CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
    CHECK(auris_spectrum_band(&spectrum, 500.0f, 3000.0f) == AURIS_OK);
    CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);
    CHECK(out.valid && circular_error(out.best, targets[t]) <= 2.0f);
  }
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return EXIT_SUCCESS;
}

/* Smoothing counts in 512-sample hops at 48 kHz. */
static int test_smoothing_hop(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_fft_t *fft = auris_fft_create_size(1024U, 4U);
  auris_srp_t *raw =
      auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, ring, 360U);
  auris_srp_t *smooth =
      auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, ring, 360U);
  CHECK(fft != NULL && raw != NULL && smooth != NULL);
  CHECK(auris_srp_set_smoothing(smooth, 0.100f) == AURIS_OK);
  plane_wave(cmb, 40.0, 0U);
  CHECK(auris_srp_apply(raw, fft, &spectrum, &first) == AURIS_OK);
  CHECK(auris_srp_apply(smooth, fft, &spectrum, &out) == AURIS_OK);
  plane_wave(cmb, 200.0, 512U); /* the next hop */
  CHECK(auris_srp_apply(raw, fft, &spectrum, &second) == AURIS_OK);
  CHECK(auris_srp_apply(smooth, fft, &spectrum, &out) == AURIS_OK);
  const float alpha = 1.0f - expf(-(512.0f / 48000.0f) / 0.100f);
  for (size_t q = 0U; q < 360U; ++q) {
    const float expected =
        first.scores[q] + alpha * (second.scores[q] - first.scores[q]);
    CHECK(fabsf(out.scores[q] - expected) < 1e-5f);
  }
  /* A gap of one hop (first_frame jumps by 1024) seeds fresh history. */
  plane_wave(cmb, 200.0, 2048U);
  CHECK(auris_srp_apply(smooth, fft, &spectrum, &out) == AURIS_OK);
  CHECK(circular_error(out.best, 200.0) <= 1.0f);
  auris_srp_destroy(raw);
  auris_srp_destroy(smooth);
  auris_fft_destroy(fft);
  return EXIT_SUCCESS;
}

static int test_guards(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_srp_t *srp =
      auris_srp_create_sized(cmb, 48000U, 1024U, 512U, 4U, ring, 360U);
  auris_fft_t *wrong_up = auris_fft_create_size(1024U, 8U);
  auris_fft_t *wrong_size = auris_fft_create_size(512U, 4U);
  CHECK(srp != NULL && wrong_up != NULL && wrong_size != NULL);
  plane_wave(cmb, 0.0, 0U);
  CHECK(auris_srp_apply(srp, wrong_up, &spectrum, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_apply(srp, wrong_size, &spectrum, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  spectrum.size = 512U;
  auris_fft_t *right = auris_fft_create_size(1024U, 4U);
  CHECK(right != NULL);
  CHECK(auris_srp_apply(srp, right, &spectrum, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  auris_fft_destroy(right);
  auris_fft_destroy(wrong_up);
  auris_fft_destroy(wrong_size);
  auris_srp_destroy(srp);
  return EXIT_SUCCESS;
}

int main(void) {
  for (size_t q = 0U; q < 360U; ++q) {
    const float a = (float)q * 0.017453292f;
    ring[q] = (auris_vec3f_t){cosf(a), sinf(a), 0.0f};
  }
  if (test_create() != EXIT_SUCCESS || test_ring_sweep() != EXIT_SUCCESS ||
      test_time_domain() != EXIT_SUCCESS ||
      test_smoothing_hop() != EXIT_SUCCESS || test_guards() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("runtime SRP tests: ok");
  return EXIT_SUCCESS;
}
