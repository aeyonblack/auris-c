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

#define RATE 48000U
#define SIZE 1024U
#define HOP 512U
#define UP 4U
#define COLUMNS 65U
#define ROWS 49U
#define DIRECTIONS (COLUMNS * ROWS)
#define LAST_BIN 128U /* 6 kHz */

static const auris_view_t view = {
    .width = 640U, .height = 480U, .fx = 480.0f, .fy = 480.0f, .cx = 319.5f,
    .cy = 239.5f, .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

static auris_vec3f_t grid[DIRECTIONS];
static auris_spectrum_t spectrum;
static auris_srp_result_t result;
static float gains[AURIS_FFT_MAX_BINS], weights[AURIS_FFT_MAX_BINS];
static float curve[SIZE * UP], other[SIZE * UP];

#include <stdint.h>

/* What a pair d metres apart keeps of bin k: 1 - (sin x / x)^2, with
   x = 2 pi f d / c. */
static double keep(double d, size_t k) {
  const double x = 6.283185307179586 * (double)k * RATE / SIZE * d / 343.0;
  const double s = sin(x) / x;
  return 1.0 - s * s;
}

static int test_gained_gcc(auris_fft_t *fft) {
  spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 2U,
                                .size = SIZE};
  for (size_t k = 11U; k <= 85U; ++k) { /* B is 3 samples late */
    const double phase = -6.283185307179586 * (double)k * 3.0 / (double)SIZE;
    spectrum.bins[0][k][0] = 1.0f;
    spectrum.bins[1][k][0] = (float)cos(phase);
    spectrum.bins[1][k][1] = (float)sin(phase);
  }
  float total = 0.0f, both = 0.0f;
  for (size_t k = 0U; k <= SIZE / 2U; ++k) {
    gains[k] = (float)(k % 3U) / 2.0f;
    weights[k] = k % 2U == 0U ? 1.0f : 0.5f;
    if (k >= 11U && k <= 85U) {
      total += gains[k];
      both += gains[k] * weights[k];
    }
  }
  bool valid = false;
  float weight = -1.0f;
  CHECK(auris_gcc_phat_curve_gained(fft, &spectrum, 0U, 1U, gains, curve,
                                    &valid, &weight) == AURIS_OK);
  CHECK(valid && fabsf(weight - total) < 1.0e-3f);
  size_t best = 0U;
  for (size_t n = 0U; n < SIZE * UP; ++n) {
    best = curve[n] > curve[best] ? n : best;
  }
  CHECK(best == 3U * UP);
  CHECK(fabsf(curve[best] - 2.0f * total / (float)SIZE) < 1.0e-4f);
  /* Gains multiply the spectrum's own weights. */
  spectrum.weights = weights;
  CHECK(auris_gcc_phat_curve_gained(fft, &spectrum, 0U, 1U, gains, curve,
                                    &valid, &weight) == AURIS_OK);
  CHECK(valid && fabsf(weight - both) < 1.0e-3f);
  CHECK(fabsf(curve[3U * UP] - 2.0f * both / (float)SIZE) < 1.0e-4f);
  /* No gains is the weighted curve exactly. */
  CHECK(auris_gcc_phat_curve_gained(fft, &spectrum, 0U, 1U, NULL, curve,
                                    &valid, &weight) == AURIS_OK);
  float plain = 0.0f;
  CHECK(auris_gcc_phat_curve_weighted(fft, &spectrum, 0U, 1U, other, &valid,
                                      &plain) == AURIS_OK);
  CHECK(weight == plain);
  for (size_t n = 0U; n < SIZE * UP; ++n) {
    CHECK(curve[n] == other[n]);
  }
  CHECK(auris_gcc_phat_curve_gained(fft, &spectrum, 0U, 1U, gains, NULL,
                                    &valid, &weight) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

static uint32_t seed = 7U;
static float uniform(void) { /* -1..1 */
  seed = seed * 1664525U + 1013904223U;
  return (float)(seed >> 8) / 8388608.0f - 1.0f;
}

static double advance(const auris_array_t *array, size_t m, auris_vec3f_t ray) {
  const auris_vec3f_t p = array->positions[m];
  return (double)RATE / 343.0 *
         ((double)p.x_m * ray.x_m + (double)p.y_m * ray.y_m +
          (double)p.z_m * ray.z_m);
}

/* One frame of a far source along `ray` (bins 5..LAST_BIN) with nothing
   else. */
static void clean_spectrum(const auris_array_t *array, auris_vec3f_t ray) {
  spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 8U,
                                .size = SIZE};
  for (size_t m = 0U; m < 8U; ++m) {
    for (size_t k = 5U; k <= LAST_BIN; ++k) {
      const double phase =
          6.283185307179586 * (double)k * advance(array, m, ray) / SIZE;
      spectrum.bins[m][k][0] = (float)cos(phase);
      spectrum.bins[m][k][1] = (float)sin(phase);
    }
  }
}

#define WAVES 16U
/* One frame of a source in a reverberant room: the source along `ray`,
   plus WAVES plane waves of equal strength from random directions all
   round the array, each with its own random phase in every bin. Below
   `weak_below` the source is half as strong as the room; above, three
   times stronger. */
static void room_spectrum(const auris_array_t *array, auris_vec3f_t ray,
                          size_t weak_below) {
  static double lead[WAVES][8];
  for (size_t i = 0U; i < WAVES; ++i) {
    auris_vec3f_t d;
    float length2;
    do {
      d = (auris_vec3f_t){uniform(), uniform(), uniform()};
      length2 = d.x_m * d.x_m + d.y_m * d.y_m + d.z_m * d.z_m;
    } while (length2 > 1.0f || length2 < 0.01f);
    const float length = sqrtf(length2);
    d = (auris_vec3f_t){d.x_m / length, d.y_m / length, d.z_m / length};
    for (size_t m = 0U; m < 8U; ++m) {
      lead[i][m] = advance(array, m, d);
    }
  }
  spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 8U,
                                .size = SIZE};
  for (size_t k = 5U; k <= LAST_BIN; ++k) {
    const double step = 6.283185307179586 * (double)k / SIZE;
    const double level = k < weak_below ? 0.5 : 3.0;
    const double turn = 3.141592653589793 * (double)uniform();
    for (size_t m = 0U; m < 8U; ++m) {
      const double phase = step * advance(array, m, ray) + turn;
      spectrum.bins[m][k][0] = (float)(level * cos(phase));
      spectrum.bins[m][k][1] = (float)(level * sin(phase));
    }
    for (size_t i = 0U; i < WAVES; ++i) {
      const double start = 3.141592653589793 * (double)uniform();
      for (size_t m = 0U; m < 8U; ++m) {
        const double phase = step * lead[i][m] + start;
        spectrum.bins[m][k][0] += (float)(cos(phase) / 4.0);
        spectrum.bins[m][k][1] += (float)(sin(phase) / 4.0);
      }
    }
  }
}

/* The cell that wins over `frames` frames of the room. */
static int room_winner(auris_srp_t *srp, auris_fft_t *fft,
                       const auris_array_t *array, auris_vec3f_t ray,
                       size_t frames, size_t *winner) {
  static double total[DIRECTIONS];
  for (size_t q = 0U; q < DIRECTIONS; ++q) {
    total[q] = 0.0;
  }
  /* Speech: strong where the room is, weaker above 2 kHz. */
  for (size_t k = 0U; k <= SIZE / 2U; ++k) {
    weights[k] = k < 43U ? 1.0f : 0.1f;
  }
  seed = 7U; /* the same room for every setting */
  for (size_t f = 0U; f < frames; ++f) {
    room_spectrum(array, ray, 43U);
    spectrum.weights = weights;
    spectrum.first_frame = (uint64_t)f * HOP;
    CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
    for (size_t q = 0U; q < DIRECTIONS; ++q) {
      total[q] += (double)result.scores[q];
    }
  }
  *winner = 0U;
  for (size_t q = 1U; q < DIRECTIONS; ++q) {
    if (total[q] > total[*winner]) {
      *winner = q;
    }
  }
  return EXIT_SUCCESS;
}

static int test_srp(auris_fft_t *fft) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_srp_t *srp =
      auris_srp_create_sized(cmb, RATE, SIZE, HOP, UP, grid, DIRECTIONS);
  CHECK(srp != NULL);
  auris_mic_pair_t pairs[AURIS_SRP_MAX_PAIRS];
  size_t pair_count = 0U;
  CHECK(auris_array_generate_pairs(cmb, pairs, AURIS_SRP_MAX_PAIRS,
                                   &pair_count) == AURIS_OK);
  double full = 0.0;
  for (size_t p = 0U; p < pair_count; ++p) {
    for (size_t k = 5U; k <= LAST_BIN; ++k) {
      full += keep((double)pairs[p].distance_m, k) / (double)pair_count;
    }
  }
  const float bins = (float)(LAST_BIN - 4U);
  const size_t source = 24U * COLUMNS + 57U; /* about 26 degrees right */
  /* A clean source: off by default, every bin counts one. */
  clean_spectrum(cmb, grid[source]);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.valid && result.best == source);
  CHECK(fabsf(result.weight - bins) < 1.0e-2f);
  /* Full strength: each pair keeps 1 - sinc^2 of each bin. A clean source
     is still found exactly and is still fully coherent. */
  CHECK(auris_srp_set_diffuse_rejection(srp, 1.0f) == AURIS_OK);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.valid && result.best == source);
  CHECK(fabs((double)result.weight - full) < 0.05);
  CHECK(full > 0.5 * (double)bins && full < 0.9 * (double)bins);
  CHECK(result.coherence > 0.97f && result.coherence < 1.01f);
  /* Part strength keeps 1 - strength * sinc^2. */
  CHECK(auris_srp_set_diffuse_rejection(srp, 0.75f) == AURIS_OK);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(fabs((double)result.weight -
             ((double)bins - 0.75 * ((double)bins - full))) < 0.05);
  CHECK(result.best == source && result.coherence > 0.97f);
  /* Zero is off. */
  CHECK(auris_srp_set_diffuse_rejection(srp, 0.0f) == AURIS_OK);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(fabsf(result.weight - bins) < 1.0e-2f);

  /* A reverberant room pulls the estimate toward straight ahead (column
     32); the rejection brings it back toward the source. */
  size_t winner = 0U;
  CHECK(room_winner(srp, fft, cmb, grid[source], 30U, &winner) ==
        EXIT_SUCCESS);
  const size_t off_column = winner % COLUMNS, off_row = winner / COLUMNS;
  CHECK(auris_srp_set_diffuse_rejection(srp, 0.75f) == AURIS_OK);
  CHECK(room_winner(srp, fft, cmb, grid[source], 30U, &winner) ==
        EXIT_SUCCESS);
  const size_t part_column = winner % COLUMNS;
  CHECK(auris_srp_set_diffuse_rejection(srp, 1.0f) == AURIS_OK);
  CHECK(room_winner(srp, fft, cmb, grid[source], 30U, &winner) ==
        EXIT_SUCCESS);
  const size_t full_column = winner % COLUMNS, full_row = winner / COLUMNS;
  fprintf(stderr, "room, source at column 57 row 24: off %zu,%zu; 0.75 %zu; "
          "1.0 %zu,%zu\n", off_column, off_row, part_column, full_column,
          full_row);
  CHECK(off_column < 57U);
  CHECK(part_column > off_column && part_column <= 58U);
  CHECK(full_column >= part_column && full_column <= 58U);

  CHECK(auris_srp_set_diffuse_rejection(NULL, 1.0f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_diffuse_rejection(srp, -0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_diffuse_rejection(srp, 1.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_diffuse_rejection(srp, NAN) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  auris_srp_destroy(srp);
  return EXIT_SUCCESS;
}

static int test_plan(void) {
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  const auris_capture_profile_t *rs = auris_capture_respeaker_usb();
  CHECK(cmb->diffuse_rejection == 0.75f && rs->diffuse_rejection == 0.0f);
  CHECK(cmb->weighted_high_hz == 6000.0f);
  /* The level gate's band is unchanged. */
  CHECK(cmb->band_low_hz == 500.0f && cmb->band_high_hz == 4000.0f);
  imager_audio_options_t options = {.array = "cmb", .step = 1U};
  imager_audio_plan_t plan;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.diffuse == 0.75f && plan.band_low_hz == 200.0f &&
        plan.band_high_hz == 6000.0f);
  options.diffuse_given = true; /* 0: off */
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.diffuse == 0.0f && plan.band_high_hz == 6000.0f);
  options.diffuse = 1.0f;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.diffuse == 1.0f);
  options.diffuse = 1.5f;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  options.diffuse = -0.5f;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  /* Without SNR weighting the band is the level gate's. */
  options = (imager_audio_options_t){.array = "cmb", .step = 1U,
                                     .weighting = -1};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.band_low_hz == 500.0f && plan.band_high_hz == 4000.0f);
  /* The ReSpeaker is as it was. */
  options = (imager_audio_options_t){.array = "respeaker", .step = 1U};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.diffuse == 0.0f && plan.band_low_hz == 500.0f &&
        plan.band_high_hz == 3000.0f);
  return EXIT_SUCCESS;
}

int main(void) {
  CHECK(auris_view_grid(&view, COLUMNS, ROWS, grid, DIRECTIONS) == AURIS_OK);
  auris_fft_t *fft = auris_fft_create_size(SIZE, UP);
  CHECK(fft != NULL);
  int status = test_gained_gcc(fft);
  if (status == EXIT_SUCCESS) status = test_srp(fft);
  if (status == EXIT_SUCCESS) status = test_plan();
  auris_fft_destroy(fft);
  if (status == EXIT_SUCCESS) puts("diffuse rejection: ok");
  return status;
}
