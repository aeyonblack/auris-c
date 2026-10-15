/* SRP-PHAT map scores against a reference built the plain way: one full GCC
   curve per pair from auris_gcc_phat_curve_band, sampled at each direction's
   fractional lag by linear interpolation and averaged over the pairs. Covers
   the CMB set-up (8 mics, 1024-sample frames, 8x) with and without the height
   band, and with per-pair gains. */
#include "auris_array_configs.h"
#include "auris_srp.h"

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

#define RATE 48000U
#define SIZE 1024U
#define HOP 512U
#define UP 8U
#define COLS 21U
#define ROWS 17U
#define COUNT (COLS * ROWS)
#define SPLIT_HZ 4000.0f
#define KEEP 0.25f

static float uniform(unsigned int *seed) {
  *seed = *seed * 1664525U + 1013904223U;
  return (float)((*seed >> 8) & 0xFFFFU) / 65535.0f;
}

static auris_spectrum_t spectrum;
static float curve_low[AURIS_SRP_MAX_PAIRS][SIZE * UP];
static float curve_high[AURIS_SRP_MAX_PAIRS][SIZE * UP];
static float low[COUNT], high[COUNT], expected[COUNT];
static auris_srp_result_t out;

static float sample(const float *curve, const auris_array_t *array,
                    auris_mic_pair_t pair, auris_vec3f_t u) {
  const auris_vec3f_t a = array->positions[pair.first];
  const auris_vec3f_t b = array->positions[pair.second];
  const float scale = (float)RATE / array->speed_of_sound_mps;
  const float lag = -scale * ((b.x_m - a.x_m) * u.x_m +
                              (b.y_m - a.y_m) * u.y_m +
                              (b.z_m - a.z_m) * u.z_m);
  const float fine = lag * (float)UP;
  const int left = (int)floorf(fine);
  const size_t n = SIZE * UP;
  const size_t i = (size_t)(left < 0 ? (int)n + left : left);
  const float f = fine - (float)left;
  const float c0 = curve[i], c1 = curve[(i + 1U) % n];
  return c0 + f * (c1 - c0);
}

static int run(bool height, bool gains, unsigned int seed) {
  int result = 1;
  const auris_array_t *array = auris_config_cmb_8();
  auris_fft_t *fft = auris_fft_create_size(SIZE, UP);
  auris_srp_t *srp = NULL;
  auris_vec3f_t grid[COUNT];
  auris_mic_pair_t pairs[AURIS_SRP_MAX_PAIRS];
  size_t pair_count = 0U;
  for (size_t y = 0U; y < ROWS; ++y) {
    for (size_t x = 0U; x < COLS; ++x) {
      const float yaw = (-35.0f + 70.0f * (float)x / (float)(COLS - 1U)) *
                        0.01745329252f;
      const float pitch = (-30.0f + 60.0f * (float)y / (float)(ROWS - 1U)) *
                          0.01745329252f;
      grid[y * COLS + x] = (auris_vec3f_t){
          cosf(pitch) * sinf(yaw), sinf(pitch), -cosf(pitch) * cosf(yaw)};
    }
  }
  CHECK(fft != NULL);
  CHECK(auris_array_generate_pairs(array, pairs, AURIS_SRP_MAX_PAIRS,
                                   &pair_count) == AURIS_OK);
  srp = auris_srp_create_sized(array, RATE, SIZE, HOP, UP, grid, COUNT);
  CHECK(srp != NULL);
  CHECK(auris_srp_set_grid(srp, COLS, ROWS, 1U) == AURIS_OK);
  if (gains) {
    CHECK(auris_srp_set_diffuse_rejection(srp, 0.75f) == AURIS_OK);
  }
  if (height) {
    /* share 0 keeps the low bins' say on height at KEEP from the start. */
    CHECK(auris_srp_set_height_band(srp, SPLIT_HZ, KEEP, 0.0f) == AURIS_OK);
  }
  memset(&spectrum, 0, sizeof(spectrum));
  spectrum.sample_rate = RATE;
  spectrum.channels = array->microphone_count;
  spectrum.size = SIZE;
  const size_t bins = SIZE / 2U + 1U;
  /* A band-limited 200-6000 Hz spectrum: random phases plus a source. */
  for (size_t c = 0U; c < spectrum.channels; ++c) {
    for (size_t k = 5U; k <= 128U; ++k) {
      const float phase = 6.2831853f * uniform(&seed);
      const float amp = 0.1f + uniform(&seed);
      spectrum.bins[c][k][0] = amp * cosf(phase);
      spectrum.bins[c][k][1] = amp * sinf(phase);
    }
  }
  CHECK(auris_srp_apply(srp, fft, &spectrum, &out) == AURIS_OK);

  /* The reference, from full curves. */
  const size_t split = (size_t)ceil((double)SPLIT_HZ * SIZE / RATE - 1.0e-6);
  static float pair_gains[SIZE / 2U + 1U];
  for (size_t p = 0U; p < pair_count; ++p) {
    const float *g = NULL;
    if (gains) {
      const double bin_hz = (double)RATE / (double)SIZE;
      const double per_hz = 6.283185307179586 * (double)pairs[p].distance_m /
                            (double)array->speed_of_sound_mps;
      for (size_t k = 0U; k < bins; ++k) {
        const double x = per_hz * (double)k * bin_hz;
        const double coherence = x < 1.0e-6 ? 1.0 : sin(x) / x;
        pair_gains[k] = (float)(1.0 - 0.75 * coherence * coherence);
      }
      g = pair_gains;
    }
    bool valid = false;
    float weight = 0.0f;
    CHECK(auris_gcc_phat_curve_band(fft, &spectrum, pairs[p].first,
                                    pairs[p].second, g, 1U,
                                    height ? split - 1U : SIZE / 2U,
                                    curve_low[p], &valid, &weight) == AURIS_OK);
    if (height) {
      CHECK(auris_gcc_phat_curve_band(fft, &spectrum, pairs[p].first,
                                      pairs[p].second, g, split, SIZE / 2U,
                                      curve_high[p], &valid, &weight) ==
            AURIS_OK);
    }
  }
  float column_best[COLS];
  for (size_t x = 0U; x < COLS; ++x) {
    column_best[x] = -INFINITY;
  }
  for (size_t q = 0U; q < COUNT; ++q) {
    float l = 0.0f, h = 0.0f;
    for (size_t p = 0U; p < pair_count; ++p) {
      l += sample(curve_low[p], array, pairs[p], grid[q]);
      if (height) {
        h += sample(curve_high[p], array, pairs[p], grid[q]);
      }
    }
    low[q] = l / (float)pair_count;
    high[q] = h / (float)pair_count;
    column_best[q % COLS] = fmaxf(column_best[q % COLS], low[q]);
  }
  size_t best = 0U;
  for (size_t q = 0U; q < COUNT; ++q) {
    const float b = column_best[q % COLS];
    expected[q] = height ? high[q] + b - KEEP * (b - low[q]) : low[q];
    if (expected[q] > expected[best]) {
      best = q;
    }
  }
  float worst = 0.0f;
  for (size_t q = 0U; q < COUNT; ++q) {
    worst = fmaxf(worst, fabsf(out.scores[q] - expected[q]));
  }
  if (worst > 2.0e-6f) {
    fprintf(stderr, "height %d gains %d: worst score error %g\n", height,
            gains, (double)worst);
  }
  CHECK(worst <= 2.0e-6f);
  CHECK(out.evaluated == COUNT && out.pairs == pair_count);
  CHECK(out.best == best);
  float sum = 0.0f;
  for (size_t p = 0U; p < out.pairs; ++p) {
    sum += out.contributions[p];
  }
  if (!height) {
    CHECK(fabsf(sum / (float)out.pairs - out.peak) < 1.0e-6f);
  }
  result = 0;
done:
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return result;
}

int main(void) {
  int result = EXIT_FAILURE;
  CHECK(run(false, false, 11U) == 0);
  CHECK(run(false, true, 12U) == 0);
  CHECK(run(true, false, 13U) == 0);
  CHECK(run(true, true, 14U) == 0);
  puts("SRP map matches the full-curve reference");
  result = EXIT_SUCCESS;
done:
  return result;
}
