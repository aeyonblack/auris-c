#include "auris_view.h"
#include "imager_audio.h"

#include <math.h>
#include <stdint.h>
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
#define FIRST_BIN 5U
#define SPLIT_BIN 86U /* the first bin at or above 4 kHz (4031 Hz) */
#define LAST_BIN 128U /* 6 kHz */

static const auris_view_t view = {
    .width = 640U, .height = 480U, .fx = 480.0f, .fy = 480.0f, .cx = 319.5f,
    .cy = 239.5f, .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

static auris_vec3f_t grid[DIRECTIONS];
static auris_spectrum_t spectrum;
static auris_srp_result_t result, plain;
static float weights[AURIS_FFT_MAX_BINS];
static float whole[SIZE * UP], low[SIZE * UP], high[SIZE * UP];

/* Bins first..last of every microphone as heard from `ray`. Other bins are
   left as they are. */
static void add_source(const auris_array_t *array, auris_vec3f_t ray,
                       size_t first, size_t last) {
  for (size_t m = 0U; m < 8U; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance = (double)RATE / 343.0 *
                           ((double)p.x_m * ray.x_m + (double)p.y_m * ray.y_m +
                            (double)p.z_m * ray.z_m);
    for (size_t k = first; k <= last; ++k) {
      const double phase = 6.283185307179586 * (double)k * advance / SIZE;
      spectrum.bins[m][k][0] = (float)cos(phase);
      spectrum.bins[m][k][1] = (float)sin(phase);
    }
  }
}

static void empty_spectrum(void) {
  spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 8U,
                                .size = SIZE};
}

static int test_band_curve(auris_fft_t *fft) {
  const auris_array_t *cmb = auris_config_cmb_8();
  empty_spectrum();
  add_source(cmb, grid[20U * COLUMNS + 50U], FIRST_BIN, LAST_BIN);
  for (size_t k = 0U; k <= SIZE / 2U; ++k) {
    weights[k] = 0.25f + 0.5f * (float)(k % 3U) / 2.0f;
  }
  spectrum.weights = weights;
  bool valid = false;
  float all = 0.0f, below = 0.0f, above = 0.0f;
  CHECK(auris_gcc_phat_curve_gained(fft, &spectrum, 0U, 4U, NULL, whole,
                                    &valid, &all) == AURIS_OK);
  CHECK(valid);
  /* Two halves of the band add up to the whole: curve and weight. */
  CHECK(auris_gcc_phat_curve_band(fft, &spectrum, 0U, 4U, NULL, 1U,
                                  SPLIT_BIN - 1U, low, &valid, &below) ==
        AURIS_OK);
  CHECK(valid);
  CHECK(auris_gcc_phat_curve_band(fft, &spectrum, 0U, 4U, NULL, SPLIT_BIN,
                                  SIZE / 2U - 1U, high, &valid, &above) ==
        AURIS_OK);
  CHECK(valid && fabsf(below + above - all) < 1.0e-3f);
  float expected = 0.0f;
  for (size_t k = FIRST_BIN; k < SPLIT_BIN; ++k) {
    expected += weights[k];
  }
  CHECK(fabsf(below - expected) < 1.0e-3f);
  for (size_t n = 0U; n < SIZE * UP; ++n) {
    CHECK(fabsf(low[n] + high[n] - whole[n]) < 1.0e-5f);
  }
  /* A range with no signal is not valid and weighs nothing. */
  CHECK(auris_gcc_phat_curve_band(fft, &spectrum, 0U, 4U, NULL, 200U, 300U,
                                  high, &valid, &above) == AURIS_OK);
  CHECK(!valid && above == 0.0f);
  /* DC and Nyquist are never used; a reversed range is refused. */
  CHECK(auris_gcc_phat_curve_band(fft, &spectrum, 0U, 4U, NULL, 0U,
                                  SIZE / 2U, low, &valid, &below) == AURIS_OK);
  CHECK(fabsf(below - all) < 1.0e-3f);
  CHECK(auris_gcc_phat_curve_band(fft, &spectrum, 0U, 4U, NULL, 90U, 80U, low,
                                  &valid, &below) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_gcc_phat_curve_band(fft, &spectrum, 0U, 4U, NULL, 1U,
                                  SIZE / 2U + 1U, low, &valid, &below) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

static int test_srp(auris_fft_t *fft) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_srp_t *srp =
      auris_srp_create_sized(cmb, RATE, SIZE, HOP, UP, grid, DIRECTIONS);
  CHECK(srp != NULL);
  const size_t source = 20U * COLUMNS + 50U;
  const size_t echoed = 31U * COLUMNS + 50U; /* same column, 11 rows lower */

  /* The grid must be known first, and searched exhaustively. */
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.1f, 0.0f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_grid(srp, COLUMNS, ROWS, 2U) == AURIS_OK);
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.1f, 0.0f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_grid(srp, COLUMNS, ROWS, 1U) == AURIS_OK);

  /* share 0: the low bins always keep just `keep` of their say on height.
     One clean source: nothing changes. Same winner, weight, coherence. */
  empty_spectrum();
  add_source(cmb, grid[source], FIRST_BIN, LAST_BIN);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &plain) == AURIS_OK);
  CHECK(plain.valid && plain.best == source);
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.1f, 0.0f) == AURIS_OK);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.valid && result.best == source);
  CHECK(fabsf(result.weight - plain.weight) < 1.0e-3f);
  CHECK(fabsf(result.peak - plain.peak) < 1.0e-5f);
  CHECK(result.coherence > 0.97f && result.coherence < 1.01f);
  CHECK(result.evaluated == DIRECTIONS && !result.approximate);
  /* Along the source's column the low bins now say little... */
  const float drop = plain.scores[source] - plain.scores[echoed];
  CHECK(drop > 0.0f);
  CHECK(result.scores[source] - result.scores[echoed] < 0.9f * drop);
  /* ...but between columns nothing is lost: each column's best is kept. */
  const size_t aside = 20U * COLUMNS + 20U;
  CHECK(result.scores[source] - result.scores[aside] >
        0.8f * (plain.scores[source] - plain.scores[aside]));

  /* A floor reflection: below 4 kHz the sound seems to come from lower
     down, and those bins are the strongest. Only the high bins have the
     true height. */
  empty_spectrum();
  add_source(cmb, grid[echoed], FIRST_BIN, SPLIT_BIN - 1U);
  add_source(cmb, grid[source], SPLIT_BIN, LAST_BIN);
  for (size_t k = 0U; k <= SIZE / 2U; ++k) {
    weights[k] = k < SPLIT_BIN ? 1.0f : 0.3f;
  }
  spectrum.weights = weights;
  CHECK(auris_srp_set_height_band(srp, 0.0f, 1.0f, 0.0f) == AURIS_OK);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &plain) == AURIS_OK);
  CHECK(plain.best % COLUMNS == 50U);
  fprintf(stderr, "echo: without, row %zu (source 20, echo 31)\n",
          plain.best / COLUMNS);
  CHECK(plain.best / COLUMNS >= 26U); /* pulled most of the way */
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.1f, 0.0f) == AURIS_OK);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  fprintf(stderr, "echo: with, column %zu row %zu\n", result.best % COLUMNS,
          result.best / COLUMNS);
  CHECK(result.best % COLUMNS == 50U);
  CHECK(result.best / COLUMNS >= 19U && result.best / COLUMNS <= 21U);
  CHECK(fabsf(result.weight - plain.weight) < 1.0e-3f);

  /* With nothing above 4 kHz the low bins still give their best guess. */
  empty_spectrum();
  add_source(cmb, grid[echoed], FIRST_BIN, SPLIT_BIN - 1U);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.valid && result.best == echoed);
  /* And with nothing below it, the high bins alone decide. */
  empty_spectrum();
  add_source(cmb, grid[source], SPLIT_BIN, LAST_BIN);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.valid && result.best == source);

  /* With a share, the low bins keep their whole say until the high bins
     carry that share of their weight. The echo frame has 16 %: at once the
     high bins decide. */
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.1f, 0.1f) == AURIS_OK);
  uint64_t position = 0U;
  for (size_t frames = 0U; frames < 100U; ++frames, position += HOP) {
    empty_spectrum();
    add_source(cmb, grid[echoed], FIRST_BIN, SPLIT_BIN - 1U);
    add_source(cmb, grid[source], SPLIT_BIN, LAST_BIN);
    spectrum.weights = weights;
    spectrum.first_frame = position;
    CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
    CHECK(result.best % COLUMNS == 50U);
    CHECK(result.best / COLUMNS >= 19U && result.best / COLUMNS <= 21U);
  }
  /* The same sound without its high bins: what the low bins say about
     height comes back over about a second, not at once. */
  const size_t above = echoed - 5U * COLUMNS;
  empty_spectrum();
  add_source(cmb, grid[echoed], FIRST_BIN, SPLIT_BIN - 1U);
  spectrum.weights = weights;
  static auris_spectrum_t low_only;
  low_only = spectrum;
  CHECK(auris_srp_set_height_band(srp, 0.0f, 1.0f, 0.0f) == AURIS_OK);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &plain) == AURIS_OK);
  const float full = plain.scores[echoed] - plain.scores[above];
  CHECK(full > 0.0f);
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.1f, 0.1f) == AURIS_OK);
  /* Fresh, it has heard no high bins: exactly as without the split. */
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.best == echoed);
  CHECK(fabsf(result.scores[echoed] - result.scores[above] - full) <
        1.0e-3f * full);
  position = 0U;
  for (size_t frames = 0U; frames < 100U; ++frames, position += HOP) {
    empty_spectrum();
    add_source(cmb, grid[echoed], FIRST_BIN, SPLIT_BIN - 1U);
    add_source(cmb, grid[source], SPLIT_BIN, LAST_BIN);
    spectrum.weights = weights;
    spectrum.first_frame = position;
    CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  }
  float say = 0.0f;
  for (size_t frames = 0U; frames < 400U; ++frames, position += HOP) {
    spectrum = low_only;
    spectrum.first_frame = position;
    CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
    CHECK(result.best == echoed);
    say = (result.scores[echoed] - result.scores[above]) / full;
    if (frames == 0U) {
      CHECK(say > 0.05f && say < 0.2f);
    }
    if (frames == 40U) { /* 0.43 s on */
      CHECK(say < 0.6f);
    }
  }
  fprintf(stderr, "low bins' say on height 4.3 s after the high bins stop: "
          "%.2f\n", (double)say);
  CHECK(say > 0.9f && say <= 1.001f);

  /* Changing the search step later is refused while the split is on. */
  CHECK(auris_srp_set_grid(srp, COLUMNS, ROWS, 2U) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_height_band(NULL, 4000.0f, 0.1f, 0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.0f, 0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 1.5f, 0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_height_band(srp, 30000.0f, 0.1f, 0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_height_band(srp, -1.0f, 0.1f, 0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_height_band(srp, NAN, 0.1f, 0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_height_band(srp, 4000.0f, 0.1f, -0.1f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  auris_srp_destroy(srp);
  return EXIT_SUCCESS;
}

static int test_plan(void) {
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  const auris_capture_profile_t *rs = auris_capture_respeaker_usb();
  CHECK(cmb->height_split_hz == 4000.0f && cmb->height_keep == 0.1f &&
        cmb->height_share == 0.1f);
  CHECK(rs->height_split_hz == 0.0f && rs->height_keep == 1.0f);
  imager_audio_options_t options = {.array = "cmb", .step = 1U};
  imager_audio_plan_t plan;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.height_split_hz == 4000.0f && plan.height_keep == 0.1f &&
        plan.height_share == 0.1f);
  /* A coarse search cannot use it. */
  options.step = 2U;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.height_split_hz == 0.0f && plan.height_keep == 1.0f);
  options.step = 1U;
  options.height_given = true;
  options.height_split_hz = 3500.0f;
  options.height_keep = 0.25f;
  options.height_share = 0.05f;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.height_split_hz == 3500.0f && plan.height_keep == 0.25f &&
        plan.height_share == 0.05f);
  options.height_split_hz = 0.0f; /* off */
  options.height_keep = 1.0f;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.height_split_hz == 0.0f && plan.height_share == 0.0f);
  options.height_keep = 0.0f;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  options.height_keep = 0.5f;
  options.height_split_hz = 25000.0f;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  options.height_split_hz = 4000.0f;
  options.height_share = -1.0f;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  options = (imager_audio_options_t){.array = "respeaker", .step = 1U};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.height_split_hz == 0.0f && plan.height_keep == 1.0f);
  /* The whole chain opens with the CMB's defaults. */
  static imager_dsp_t dsp;
  options = (imager_audio_options_t){.array = "cmb", .step = 1U};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, true, true) == 0);
  imager_dsp_close(&dsp);
  return EXIT_SUCCESS;
}

int main(void) {
  CHECK(auris_view_grid(&view, COLUMNS, ROWS, grid, DIRECTIONS) == AURIS_OK);
  auris_fft_t *fft = auris_fft_create_size(SIZE, UP);
  CHECK(fft != NULL);
  int status = test_band_curve(fft);
  if (status == EXIT_SUCCESS) status = test_srp(fft);
  if (status == EXIT_SUCCESS) status = test_plan();
  auris_fft_destroy(fft);
  if (status == EXIT_SUCCESS) puts("height band: ok");
  return status;
}
