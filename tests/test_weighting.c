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

static const auris_view_t view = {
    .width = 640U, .height = 480U, .fx = 480.0f, .fy = 480.0f, .cx = 319.5f,
    .cy = 239.5f, .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

static auris_vec3f_t grid[DIRECTIONS];
static auris_spectrum_t spectrum;
static auris_srp_result_t result;
static auris_framer_t framer;
static auris_preprocess_t prep;
static auris_analysis_frame_t frame;
static auris_audio_block_t block;
static auris_activity_t gate, level_gate;
static imager_dsp_t dsp;
static float weights[AURIS_FFT_MAX_BINS];
static float curve[SIZE * UP];

static uint32_t seed = 99U;
static float uniform(void) { /* -1..1 */
  seed = seed * 1664525U + 1013904223U;
  return (float)(seed >> 8) / 8388608.0f - 1.0f;
}

/* Two channels, B delayed by `delay` samples, unit magnitude in bins 11-85. */
static void delayed_pair(double delay) {
  spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 2U,
                                .size = SIZE};
  for (size_t k = 11U; k <= 85U; ++k) {
    const double phase = -6.283185307179586 * (double)k * delay / (double)SIZE;
    spectrum.bins[0][k][0] = 1.0f;
    spectrum.bins[1][k][0] = (float)cos(phase);
    spectrum.bins[1][k][1] = (float)sin(phase);
  }
}

static int test_weighted_gcc(auris_fft_t *fft) {
  bool valid = false;
  float weight = -1.0f;
  delayed_pair(3.0);
  CHECK(auris_gcc_phat_curve_weighted(fft, &spectrum, 0U, 1U, curve, &valid,
                                      &weight) == AURIS_OK);
  /* No weights: every bin with signal counts one. */
  CHECK(valid && fabsf(weight - 75.0f) < 1.0e-3f);
  CHECK(fabsf(curve[3U * UP] - 2.0f * 75.0f / (float)SIZE) < 1.0e-4f);

  float total = 0.0f;
  for (size_t k = 0U; k <= SIZE / 2U; ++k) {
    weights[k] = k % 2U == 0U ? 0.0f : 0.5f;
    total += k >= 11U && k <= 85U ? weights[k] : 0.0f;
  }
  spectrum.weights = weights;
  CHECK(auris_gcc_phat_curve_weighted(fft, &spectrum, 0U, 1U, curve, &valid,
                                      &weight) == AURIS_OK);
  CHECK(valid && fabsf(weight - total) < 1.0e-3f);
  /* The peak is still at 3 samples and equals 2 * weight / size. */
  size_t best = 0U;
  for (size_t n = 0U; n < SIZE * UP; ++n) {
    best = curve[n] > curve[best] ? n : best;
  }
  CHECK(best == 3U * UP);
  CHECK(fabsf(curve[best] - 2.0f * total / (float)SIZE) < 1.0e-4f);
  /* The unweighted entry point gives the same curve. */
  static float other[SIZE * UP];
  CHECK(auris_gcc_phat_curve(fft, &spectrum, 0U, 1U, other, &valid) ==
        AURIS_OK);
  CHECK(other[best] == curve[best]);
  CHECK(auris_gcc_phat_curve_weighted(fft, &spectrum, 0U, 1U, curve, &valid,
                                      NULL) == AURIS_ERROR_INVALID_ARGUMENT);
  /* A fresh spectrum from the transform carries no weights. */
  frame = (auris_analysis_frame_t){.sample_rate = RATE, .channels = 2U,
                                   .size = SIZE};
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  CHECK(spectrum.weights == NULL);
  return EXIT_SUCCESS;
}

/* Unit-phase spectra of a far source along `ray` in bins 11-64. */
static void source_spectrum(const auris_array_t *array, auris_vec3f_t ray) {
  spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 8U,
                                .size = SIZE};
  for (size_t m = 0U; m < 8U; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance = (double)RATE / 343.0 *
                           ((double)p.x_m * ray.x_m + (double)p.y_m * ray.y_m +
                            (double)p.z_m * ray.z_m);
    for (size_t k = 11U; k <= 64U; ++k) {
      const double phase = 6.283185307179586 * (double)k * advance / SIZE;
      spectrum.bins[m][k][0] = (float)cos(phase);
      spectrum.bins[m][k][1] = (float)sin(phase);
    }
  }
}

static int test_coherence(auris_fft_t *fft) {
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_srp_t *srp =
      auris_srp_create_sized(cmb, RATE, SIZE, HOP, UP, grid, DIRECTIONS);
  CHECK(srp != NULL);
  /* One clean source: every pair agrees, whatever the weights. */
  source_spectrum(cmb, grid[15U * COLUMNS + 47U]);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.valid && result.best == 15U * COLUMNS + 47U);
  CHECK(fabsf(result.weight - 54.0f) < 1.0e-2f);
  CHECK(result.coherence > 0.97f && result.coherence < 1.01f);
  for (size_t k = 0U; k <= SIZE / 2U; ++k) {
    weights[k] = k < 30U ? 0.25f : 0.0f; /* bins 11-29 at a quarter */
  }
  spectrum.weights = weights;
  auris_srp_reset(srp);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(fabsf(result.weight - 19.0f * 0.25f) < 1.0e-3f);
  CHECK(result.coherence > 0.97f && result.best == 15U * COLUMNS + 47U);
  /* Unrelated phases on every microphone: no direction explains them. */
  float worst = 0.0f;
  for (size_t trial = 0U; trial < 20U; ++trial) {
    spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 8U,
                                  .size = SIZE};
    for (size_t m = 0U; m < 8U; ++m) {
      for (size_t k = 5U; k <= 85U; ++k) {
        const float phase = 3.14159265f * uniform();
        spectrum.bins[m][k][0] = cosf(phase);
        spectrum.bins[m][k][1] = sinf(phase);
      }
    }
    auris_srp_reset(srp);
    CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
    worst = fmaxf(worst, result.coherence);
  }
  printf("unrelated phases: worst coherence %.3f of 20\n", (double)worst);
  CHECK(worst < 0.15f);
  /* No weight at all is no evidence. */
  for (size_t k = 0U; k <= SIZE / 2U; ++k) {
    weights[k] = 0.0f;
  }
  spectrum.weights = weights;
  auris_srp_reset(srp);
  CHECK(auris_srp_apply(srp, fft, &spectrum, &result) == AURIS_OK);
  CHECK(result.weight == 0.0f && result.coherence == 0.0f);
  auris_srp_destroy(srp);
  return EXIT_SUCCESS;
}

/* Independent noise on 8 channels, plus from `on` samples six tones heard
   identically by every microphone (bins 8, 16, ... 48). `tone` is their
   amplitude: 0.0002 puts each about 13 dB above the noise in its bin but
   lifts the whole band by only about 5 dB, well short of the level gate's
   10 dB. */
static void faint_block(uint64_t position, uint64_t on, float tone) {
  block = (auris_audio_block_t){.sample_rate = RATE, .channels = 8U,
                                .frames = AURIS_AUDIO_FRAMES,
                                .first_frame = position};
  for (size_t n = 0U; n < AURIS_AUDIO_FRAMES; ++n) {
    const uint64_t t = position + n;
    float value = 0.0f;
    for (size_t h = 1U; t >= on && h <= 6U; ++h) {
      value += tone * sinf(6.2831853f * (float)(8U * h) * (float)(t % SIZE) /
                           (float)SIZE + (float)h);
    }
    for (size_t m = 0U; m < 8U; ++m) {
      block.samples[m][n] = value + 0.001f * uniform();
    }
  }
}

static int test_gate(auris_fft_t *fft) {
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  auris_activity_config_t c = auris_activity_defaults_for(cmb);
  CHECK(c.weighting && c.low_hz == 200.0f && c.noise_bias == 2.5f &&
        c.weight_open == 3.0f && c.weight_close == 1.5f);
  CHECK(!auris_activity_defaults().weighting);
  CHECK(auris_preprocess_init_size(&prep, SIZE) == AURIS_OK);
  float energy = 0.0f;
  for (size_t n = 0U; n < SIZE; ++n) {
    energy += prep.window[n] * prep.window[n];
  }
  CHECK(auris_activity_init_sized(&gate, &c, RATE, 8U, SIZE, HOP, energy) ==
        AURIS_OK);
  CHECK(gate.first_bin == 5U && gate.last_bin == 85U);
  auris_activity_config_t bad = c;
  bad.noise_bias = 0.5f;
  CHECK(auris_activity_init_sized(&level_gate, &bad, RATE, 8U, SIZE, HOP,
                                  energy) == AURIS_ERROR_INVALID_ARGUMENT);
  bad = c;
  bad.weight_close = 0.0f;
  CHECK(auris_activity_init_sized(&level_gate, &bad, RATE, 8U, SIZE, HOP,
                                  energy) == AURIS_ERROR_INVALID_ARGUMENT);
  bad = c;
  bad.weight_open = 1.0f; /* below close */
  CHECK(auris_activity_init_sized(&level_gate, &bad, RATE, 8U, SIZE, HOP,
                                  energy) == AURIS_ERROR_INVALID_ARGUMENT);
  bad.weighting = false; /* then the weighting numbers are not checked */
  CHECK(auris_activity_init_sized(&level_gate, &bad, RATE, 8U, SIZE, HOP,
                                  energy) == AURIS_OK);
  /* The same band with the level gate, for comparison. */
  auris_activity_config_t plain = c;
  plain.weighting = false;
  CHECK(auris_activity_init_sized(&level_gate, &plain, RATE, 8U, SIZE, HOP,
                                  energy) == AURIS_OK);

  CHECK(auris_framer_init(&framer, SIZE, HOP) == AURIS_OK);
  const uint64_t on = 3U * RATE, end = 5U * RATE;
  size_t quiet_frames = 0U, tone_frames = 0U, opened = 0U, level_opened = 0U;
  float quiet_worst = 0.0f, noise_before = 0.0f;
  double tone_weight = 0.0, far_weight = 0.0, sum_weight = 0.0, lift = 0.0;
  for (uint64_t position = 0U; position < end; position += AURIS_AUDIO_FRAMES) {
    faint_block(position, on, 0.0002f);
    bool ready = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (!ready) {
      continue;
    }
    CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
    CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
    static auris_spectrum_t copy;
    copy = spectrum;
    auris_activity_result_t out, level_out;
    CHECK(auris_activity_apply(&gate, &spectrum, &out) == AURIS_OK);
    CHECK(auris_activity_apply(&level_gate, &copy, &level_out) == AURIS_OK);
    CHECK(spectrum.weights == gate.weights && copy.weights == NULL);
    CHECK(level_out.weight_sum == 0.0f);
    for (size_t k = 0U; k <= SIZE / 2U; ++k) {
      CHECK(gate.weights[k] >= 0.0f && gate.weights[k] < 1.0f);
      CHECK((k >= 5U && k <= 85U) || gate.weights[k] == 0.0f);
    }
    const uint64_t f = frame.first_frame;
    if (!out.ready) {
      CHECK(out.weight_sum == 0.0f); /* no noise estimate yet */
    } else if (f + SIZE <= on) {
      ++quiet_frames;
      quiet_worst = fmaxf(quiet_worst, out.weight_sum);
      CHECK(!out.active && !level_out.active);
      noise_before = gate.noise_bins[16];
    } else if (f >= on + SIZE) {
      ++tone_frames;
      opened += out.active && out.evidence ? 1U : 0U;
      level_opened += level_out.active ? 1U : 0U;
      tone_weight += (double)gate.weights[16];
      far_weight += (double)gate.weights[60];
      sum_weight += (double)out.weight_sum;
      lift += (double)(out.level_dbfs - out.noise_dbfs);
    }
  }
  printf("quiet: worst %.2f bins; tones: %.1f bins, band +%.1f dB, tone bin "
         "weight %.2f, noise bin weight %.3f; open %zu / %zu frames (level "
         "gate %zu)\n",
         (double)quiet_worst, sum_weight / (double)tone_frames,
         lift / (double)tone_frames, tone_weight / (double)tone_frames,
         far_weight / (double)tone_frames, opened, tone_frames, level_opened);
  CHECK(quiet_frames > 150U && tone_frames > 150U);
  CHECK(quiet_worst < 1.5f);          /* noise alone never reaches close */
  CHECK(level_opened == 0U);          /* the band level rose too little */
  CHECK(lift / (double)tone_frames < 7.0);
  CHECK(opened + 3U >= tone_frames);  /* open after the 3-hop attack */
  CHECK(tone_weight / (double)tone_frames > 0.75);
  CHECK(far_weight / (double)tone_frames < 0.05);
  CHECK(sum_weight / (double)tone_frames > 6.0);
  /* The source was not learned as noise while the gate was open: two
     seconds of a tone 20 times the noise would otherwise multiply the
     estimate several times over. Only the first, partly covered onset
     frame can still count as quiet. */
  printf("tone-bin noise estimate: %.3g before, %.3g after\n",
         (double)noise_before, (double)gate.noise_bins[16]);
  CHECK(fabsf(gate.noise_bins[16] - noise_before) < 0.1f * noise_before);

  /* Gate off with weighting: every frame passes, and stays weighted. */
  c.enabled = false;
  CHECK(auris_activity_init_sized(&gate, &c, RATE, 8U, SIZE, HOP, energy) ==
        AURIS_OK);
  CHECK(auris_framer_init(&framer, SIZE, HOP) == AURIS_OK);
  size_t frames = 0U;
  for (uint64_t position = 0U; position < 2U * RATE;
       position += AURIS_AUDIO_FRAMES) {
    faint_block(position, on, 0.0f);
    bool ready = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (!ready) {
      continue;
    }
    CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
    CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
    auris_activity_result_t out;
    CHECK(auris_activity_apply(&gate, &spectrum, &out) == AURIS_OK);
    CHECK(out.ready && out.active && out.evidence);
    ++frames;
    /* Unweighted while the noise is still being learned, weighted after. */
    CHECK((spectrum.weights == NULL) == (frames <= 94U));
  }
  return EXIT_SUCCESS;
}

/* The imager's DSP on 8 channels of independent noise plus, from 1.5 s:
   kind 0, a faint broadband far source at grid cell (47, 15);
   kind 1, a burst of noise that is different on every microphone. */
static int imager_run(int weighting, int kind, size_t *shown, size_t *frames,
                      size_t *best) {
  imager_audio_plan_t plan;
  const imager_audio_options_t options = {.array = "cmb", .step = 1U,
                                          .weighting = weighting};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, true, true) == 0);
  CHECK(auris_framer_init(&framer, plan.frame_size, plan.frame_hop) ==
        AURIS_OK);
  const auris_array_t *array = plan.config.array;
  const auris_vec3f_t ray = grid[15U * COLUMNS + 47U];
  result = (auris_srp_result_t){0};
  *shown = 0U;
  *frames = 0U;
  for (uint64_t position = 0U; position < 4U * RATE;
       position += AURIS_AUDIO_FRAMES) {
    block = (auris_audio_block_t){.sample_rate = RATE, .channels = 8U,
                                  .frames = AURIS_AUDIO_FRAMES,
                                  .first_frame = position};
    for (size_t m = 0U; m < 8U; ++m) {
      const auris_vec3f_t p = array->positions[m];
      const double advance = ((double)p.x_m * ray.x_m + (double)p.y_m * ray.y_m +
                              (double)p.z_m * ray.z_m) / 343.0;
      for (size_t n = 0U; n < AURIS_AUDIO_FRAMES; ++n) {
        const double t = (double)(position + n) / RATE + advance;
        double value = 0.0;
        /* 28 tones from 219 Hz to 3.9 kHz, about 4 dB above the noise
           across the band. */
        for (size_t k = 7U; kind == 0 && t >= 1.5 && k < 125U; k += 4U) {
          value += 0.00011 * sin(6.283185307179586 * (double)k * 31.25 * t +
                                 (double)(k * k));
        }
        const float burst = kind == 1 && position >= 3U * RATE / 2U ? 0.004f
                                                                    : 0.0f;
        block.samples[m][n] = (float)value + (0.001f + burst) * uniform();
      }
    }
    bool ready = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (!ready) {
      continue;
    }
    auris_activity_result_t out;
    imager_view_t drawn;
    CHECK(imager_dsp_process(&dsp, &frame, &out, &result, &drawn) ==
          AURIS_OK);
    if (frame.first_frame < 3U * RATE / 2U - SIZE) {
      CHECK(!drawn.valid); /* nothing before the source */
    } else if (frame.first_frame >= 2U * RATE) {
      ++*frames;
      if (drawn.valid) {
        ++*shown;
        *best = drawn.best;
      }
    }
  }
  imager_dsp_close(&dsp);
  return EXIT_SUCCESS;
}

static int test_imager(void) {
  size_t shown = 0U, frames = 0U, best = 0U;
  /* The level gate never sees the faint source. */
  CHECK(imager_run(-1, 0, &shown, &frames, &best) == EXIT_SUCCESS);
  printf("faint source, level gate: shown %zu of %zu frames\n", shown, frames);
  CHECK(frames > 150U && shown == 0U);
  /* Weighted, it is found, in the right place. */
  CHECK(imager_run(0, 0, &shown, &frames, &best) == EXIT_SUCCESS);
  printf("faint source, weighted: shown %zu of %zu frames, winner (%zu, %zu)\n",
         shown, frames, best % COLUMNS, best / COLUMNS);
  CHECK(shown * 10U >= frames * 9U);
  const int dc = (int)(best % COLUMNS) - 47, dr = (int)(best / COLUMNS) - 15;
  CHECK(dc >= -2 && dc <= 2 && dr >= -2 && dr <= 2);
  /* A loud burst with nothing in common between microphones opens the
     gate, but its map is not coherent enough to be shown. */
  CHECK(imager_run(0, 1, &shown, &frames, &best) == EXIT_SUCCESS);
  printf("incoherent burst, weighted: shown %zu of %zu frames\n", shown,
         frames);
  CHECK(shown == 0U);
  return EXIT_SUCCESS;
}

int main(void) {
  auris_fft_t *fft = auris_fft_create_size(SIZE, UP);
  int status = EXIT_FAILURE;
  if (fft != NULL &&
      auris_view_grid(&view, COLUMNS, ROWS, grid, DIRECTIONS) == AURIS_OK &&
      test_weighted_gcc(fft) == EXIT_SUCCESS &&
      test_coherence(fft) == EXIT_SUCCESS && test_gate(fft) == EXIT_SUCCESS &&
      test_imager() == EXIT_SUCCESS) {
    puts("weighting tests: ok");
    status = EXIT_SUCCESS;
  }
  auris_fft_destroy(fft);
  return status;
}
