#include "auris_spectrum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition);  \
      goto done;                                                               \
    }                                                                          \
  } while (0)

int main(void) {
  int result = EXIT_FAILURE;
  auris_fft_t *fft = auris_fft_create();
  auris_analysis_frame_t frame = {.sample_rate = 16000U, .channels = 2U};
  auris_spectrum_t spectrum;
  auris_gcc_t gcc;
  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  CHECK(fft != NULL);

  /* A constant has only DC. This checks transform scaling. */
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    frame.samples[0][n] = 0.25f;
  }
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  CHECK(fabsf(spectrum.bins[0][0][0] - 128.0f) < 1.0e-4f);
  CHECK(fabsf(spectrum.bins[0][1][0]) < 1.0e-4f);

  /* A bin-centred sine checks frequency indexing and imaginary sign. */
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    frame.samples[0][n] = 0.5f * sinf(6.2831853071795864769f * 32.0f *
                                      (float)n / (float)AURIS_ANALYSIS_SIZE);
  }
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  CHECK(fabsf(spectrum.bins[0][32][0]) < 0.01f);
  CHECK(fabsf(spectrum.bins[0][32][1] + 128.0f) < 0.01f);

  /* Test both delay signs and zero with impulses away from edges. */
  for (int delay = -3; delay <= 3; ++delay) {
    frame = (auris_analysis_frame_t){.sample_rate = 16000U, .channels = 2U};
    frame.samples[0][200] = 0.5f;
    frame.samples[1][(size_t)(200 + delay)] = 0.5f;
    CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
    CHECK(auris_gcc_phat(fft, &spectrum, 0U, 1U, 8, &gcc) == AURIS_OK);
    CHECK(gcc.valid && gcc.lag_samples == delay);
    CHECK(fabsf(gcc.peak - 510.0f / 512.0f) < 1.0e-4f);
    CHECK(auris_gcc_phat(fft, &spectrum, 1U, 0U, 8, &gcc) == AURIS_OK);
    CHECK(gcc.lag_samples == -delay);
  }

  /* Broadband, delayed, DC-biased input through real preprocessing. */
  frame = (auris_analysis_frame_t){
      .sample_rate = 16000U, .channels = 2U, .first_frame = 256U};
  uint32_t seed = 1U;
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    frame.samples[0][n] =
        0.2f + 0.2f * ((float)(seed >> 8) / 16777216.0f - 0.5f);
    frame.samples[1][n] = n >= 3U ? frame.samples[0][n - 3U] : 0.2f;
  }
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  CHECK(spectrum.first_frame == 256U && spectrum.sample_rate == 16000U);
  CHECK(auris_gcc_phat(fft, &spectrum, 0U, 1U, 8, &gcc) == AURIS_OK);
  CHECK(gcc.valid && gcc.lag_samples == 3);

  frame = (auris_analysis_frame_t){.sample_rate = 16000U, .channels = 2U};
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  CHECK(auris_gcc_phat(fft, &spectrum, 0U, 1U, 8, &gcc) == AURIS_OK);
  CHECK(!gcc.valid);
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    CHECK(gcc.correlation[n] == 0.0f);
  }
  CHECK(auris_gcc_phat(fft, &spectrum, 0U, 0U, 8, &gcc) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat(fft, &spectrum, 0U, 1U, 256, &gcc) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  puts("spectrum and GCC-PHAT tests: ok");
  result = EXIT_SUCCESS;
done:
  auris_fft_destroy(fft);
  return result;
}
