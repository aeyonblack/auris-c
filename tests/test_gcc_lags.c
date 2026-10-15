/* auris_gcc_phat_lags must give the same values as the full inverse transform
   of auris_gcc_phat_curve_band at every requested lag, with the same validity
   and weight, for full and band-limited spectra, with and without gains and
   per-bin weights. */
#include "auris_spectrum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      goto done;                                                               \
    }                                                                          \
  } while (0)

static float uniform(unsigned int *seed) {
  *seed = *seed * 1664525U + 1013904223U;
  return (float)((*seed >> 8) & 0xFFFFU) / 65535.0f;
}

static auris_spectrum_t spectrum;

/* zero_low..zero_high: bins kept; the rest are zeroed like a band-limited
   spectrum (0, 0 keeps every bin). */
static int compare(size_t size, size_t upsample, size_t first, size_t last,
                   bool use_gains, bool use_weights, size_t keep_low,
                   size_t keep_high, unsigned int seed) {
  int result = 1;
  auris_fft_t *fft = auris_fft_create_size(size, upsample);
  const size_t bins = size / 2U + 1U, fine = size * upsample;
  float *curve = malloc(fine * sizeof(float));
  float *gains = malloc(bins * sizeof(float));
  float *weights = malloc(bins * sizeof(float));
  int *lags = malloc(fine * sizeof(int));
  float *values = malloc(fine * sizeof(float));
  CHECK(fft != NULL && curve != NULL && gains != NULL && weights != NULL &&
        lags != NULL && values != NULL);
  memset(&spectrum, 0, sizeof(spectrum));
  spectrum.sample_rate = 48000U;
  spectrum.channels = 3U;
  spectrum.size = size;
  for (size_t c = 0U; c < 3U; ++c) {
    for (size_t k = 0U; k < bins; ++k) {
      const bool kept = keep_high == 0U || (k >= keep_low && k <= keep_high);
      spectrum.bins[c][k][0] = kept ? 2.0f * uniform(&seed) - 1.0f : 0.0f;
      spectrum.bins[c][k][1] = kept ? 2.0f * uniform(&seed) - 1.0f : 0.0f;
    }
  }
  for (size_t k = 0U; k < bins; ++k) {
    gains[k] = 0.2f + 0.8f * uniform(&seed);
    weights[k] = uniform(&seed);
  }
  spectrum.weights = use_weights ? weights : NULL;
  const float *g = use_gains ? gains : NULL;
  bool full_valid = false, lags_valid = true;
  float full_weight = -1.0f, lags_weight = -2.0f;
  CHECK(auris_gcc_phat_curve_band(fft, &spectrum, 0U, 2U, g, first, last,
                                  curve, &full_valid, &full_weight) ==
        AURIS_OK);
  /* Every lag in the reserved range: up to +-300, or the whole curve
     (-fine/2..fine/2, both ends) when that is shorter. */
  const size_t reach = fine / 2U < 300U ? fine / 2U : 300U;
  CHECK(auris_fft_reserve_lags(fft, reach) == AURIS_OK);
  size_t count = 0U;
  for (int lag = -(int)reach; lag <= (int)reach; ++lag) {
    if (count < fine) {
      lags[count++] = lag;
    }
  }
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 2U, g, first, last, lags,
                            count, values, &lags_valid, &lags_weight) ==
        AURIS_OK);
  CHECK(lags_valid == full_valid);
  CHECK(lags_weight == full_weight);
  float largest = 0.0f, worst = 0.0f;
  for (size_t i = 0U; i < count; ++i) {
    const size_t index =
        (size_t)(lags[i] < 0 ? (int)fine + lags[i] : lags[i]) % fine;
    largest = fmaxf(largest, fabsf(curve[index]));
    worst = fmaxf(worst, fabsf(values[i] - curve[index]));
  }
  if (worst > 2.0e-6f) {
    fprintf(stderr, "size %zu x%zu bins %zu..%zu: worst %g (largest %g)\n",
            size, upsample, first, last, (double)worst, (double)largest);
  }
  CHECK(worst <= 2.0e-6f);
  result = 0;
done:
  free(values);
  free(lags);
  free(weights);
  free(gains);
  free(curve);
  auris_fft_destroy(fft);
  return result;
}

int main(void) {
  int result = EXIT_FAILURE;
  auris_fft_t *fft = auris_fft_create_size(256U, 4U);
  CHECK(fft != NULL);
  /* (Frames of at least 256 samples: smaller ones are not a valid analysis
     size for this hop rule.) The CMB set-up: 1024-sample frames, 8x, both halves of the height band,
     on a spectrum limited to 200-6000 Hz, with weights and gains. */
  CHECK(compare(1024U, 8U, 1U, 85U, true, true, 5U, 128U, 1U) == 0);
  CHECK(compare(1024U, 8U, 86U, 512U, true, true, 5U, 128U, 2U) == 0);
  /* Full spectra, no gains or weights; the ReSpeaker set-up; a short frame. */
  CHECK(compare(1024U, 8U, 1U, 512U, false, false, 0U, 0U, 3U) == 0);
  CHECK(compare(512U, 4U, 1U, 256U, true, false, 0U, 0U, 4U) == 0);
  CHECK(compare(256U, 1U, 3U, 20U, false, true, 0U, 0U, 5U) == 0);
  CHECK(compare(256U, 4U, 1U, 128U, true, true, 2U, 30U, 6U) == 0);

  /* Arguments. */
  memset(&spectrum, 0, sizeof(spectrum));
  spectrum.sample_rate = 48000U;
  spectrum.channels = 2U;
  spectrum.size = 256U;
  spectrum.bins[0][3][0] = 1.0f;
  spectrum.bins[1][3][0] = 1.0f;
  int lags[3] = {-1, 0, 1};
  float values[3];
  bool valid = false;
  float weight = 0.0f;
  /* Nothing reserved yet; then more than half the 1024-point fine curve. */
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 1U, NULL, 1U, 32U, lags, 3U,
                            values, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_fft_reserve_lags(fft, 513U) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_fft_reserve_lags(NULL, 1U) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_fft_reserve_lags(fft, 32U) == AURIS_OK);
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 1U, NULL, 1U, 32U, lags, 3U,
                            values, &valid, &weight) == AURIS_OK);
  CHECK(valid && weight == 1.0f);
  CHECK(fabsf(values[1] - 2.0f / 256.0f) < 1.0e-7f);
  CHECK(auris_gcc_phat_lags(NULL, &spectrum, 0U, 1U, NULL, 1U, 32U, lags, 3U,
                            values, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 0U, NULL, 1U, 32U, lags, 3U,
                            values, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 1U, NULL, 5U, 4U, lags, 3U,
                            values, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 1U, NULL, 1U, 32U, NULL, 3U,
                            values, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 1U, NULL, 1U, 32U, lags, 0U,
                            values, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 1U, NULL, 1U, 32U, lags, 3U,
                            NULL, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  lags[2] = 33; /* beyond the reserved range */
  CHECK(auris_gcc_phat_lags(fft, &spectrum, 0U, 1U, NULL, 1U, 32U, lags, 3U,
                            values, &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  puts("GCC-PHAT at chosen lags matches the full inverse transform");
  result = EXIT_SUCCESS;
done:
  auris_fft_destroy(fft);
  return result;
}
