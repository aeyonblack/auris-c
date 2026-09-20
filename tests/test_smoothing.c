#include "auris_activity.h"
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

static void source(auris_spectrum_t *s, const auris_array_t *array,
                   auris_vec3f_t u, uint64_t first) {
  *s = (auris_spectrum_t){
      .sample_rate = 16000U, .channels = 4U, .first_frame = first};
  for (size_t m = 0U; m < 4U; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const float advance = 16000.0f / array->speed_of_sound_mps *
                          (p.x_m * u.x_m + p.y_m * u.y_m + p.z_m * u.z_m);
    for (size_t k = 1U; k + 1U < AURIS_FFT_BINS; ++k) {
      const float phase = 6.283185307179586f * (float)k * advance / 512.0f;
      s->bins[m][k][0] = cosf(phase);
      s->bins[m][k][1] = sinf(phase);
    }
  }
}

int main(void) {
  int result = EXIT_FAILURE;
  const auris_array_t *array = auris_config_respeaker_usb_4();
  const auris_vec3f_t grid[3] = {
      {-0.4f, 0, sqrtf(0.84f)}, {0, 0, 1}, {0.4f, 0, sqrtf(0.84f)}};
  auris_fft_t *fft = auris_fft_create();
  auris_srp_t *raw = auris_srp_create(array, 16000U, grid, 3U);
  auris_srp_t *smooth = auris_srp_create(array, 16000U, grid, 3U);
  CHECK(fft != NULL && raw != NULL && smooth != NULL);
  CHECK(auris_srp_set_smoothing(smooth, 0.100f) == AURIS_OK);
  auris_spectrum_t spectrum;
  auris_srp_result_t a, b, out;
  source(&spectrum, array, grid[0], 0U);
  CHECK(auris_srp_apply(raw, fft, &spectrum, &a) == AURIS_OK);
  CHECK(auris_srp_apply(smooth, fft, &spectrum, &out) == AURIS_OK);
  CHECK(out.best == 0U);
  source(&spectrum, array, grid[2], 256U);
  CHECK(auris_srp_apply(raw, fft, &spectrum, &b) == AURIS_OK);
  CHECK(auris_srp_apply(smooth, fft, &spectrum, &out) == AURIS_OK);
  const float alpha = 1.0f - expf(-0.016f / 0.100f);
  for (size_t q = 0U; q < 3U; ++q) {
    CHECK(fabsf(out.scores[q] -
                (a.scores[q] + alpha * (b.scores[q] - a.scores[q]))) < 1.0e-5f);
  }
  float sum = 0.0f;
  for (size_t p = 0U; p < out.pairs; ++p) {
    sum += out.contributions[p];
  }
  CHECK(fabsf(sum / (float)out.pairs - out.peak) < 1.0e-6f);
  auris_srp_reset(smooth);
  CHECK(auris_srp_apply(smooth, fft, &spectrum, &out) == AURIS_OK &&
        out.best == 2U);
  source(&spectrum, array, grid[0],
         1024U); /* Timeline gap: seed, do not blend. */
  CHECK(auris_srp_apply(smooth, fft, &spectrum, &out) == AURIS_OK &&
        out.best == 0U);

  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  float window_energy = 0.0f;
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    window_energy += prep.window[n] * prep.window[n];
  }
  auris_activity_config_t config = auris_activity_defaults();
  auris_activity_t activity = {0};
  CHECK(auris_activity_init(&activity, &config, 16000U, 4U, window_energy) ==
        AURIS_OK);
  auris_analysis_frame_t frame = {.sample_rate = 16000U, .channels = 4U};
  for (size_t m = 0U; m < 4U; ++m) {
    for (size_t n = 0U; n < 512U; ++n) {
      frame.samples[m][n] =
          0.2f + 0.1f * cosf(6.283185307179586f * 32.0f * (float)n / 512.0f);
    }
  }
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  auris_activity_result_t gate;
  CHECK(auris_activity_apply(&activity, &spectrum, &gate) == AURIS_OK);
  CHECK(fabsf(gate.level_dbfs - 10.0f * log10f(0.1f * 0.1f / 2.0f)) < 0.01f);
  for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
    if (k < activity.first_bin || k > activity.last_bin) {
      CHECK(spectrum.bins[0][k][0] == 0.0f && spectrum.bins[0][k][1] == 0.0f);
    }
  }
  puts("smoothing and band-power tests: ok");
  result = EXIT_SUCCESS;
done:
  auris_srp_destroy(smooth);
  auris_srp_destroy(raw);
  auris_fft_destroy(fft);
  return result;
}
