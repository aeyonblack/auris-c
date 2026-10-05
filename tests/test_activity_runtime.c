#include "auris_activity.h"
#include "auris_array_configs.h"

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

static auris_framer_t framer;
static auris_preprocess_t prep;
static auris_analysis_frame_t frame;
static auris_spectrum_t spectrum;
static auris_audio_block_t block;

static double db(double power) { return 10.0 * log10(power); }

static int test_profiles(void) {
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  const auris_capture_profile_t *rs = auris_capture_respeaker_usb();
  CHECK(cmb->band_low_hz == 500.0f && cmb->band_high_hz == 4000.0f);
  CHECK(rs->band_low_hz == 500.0f && rs->band_high_hz == 3000.0f);
  CHECK(cmb->gate_minimum_dbfs == -70.0f && rs->gate_minimum_dbfs == -65.0f);
  CHECK(cmb->band_high_hz <= (float)cmb->sample_rate / 2.0f);
  CHECK(rs->band_high_hz <= (float)rs->sample_rate / 2.0f);
  const auris_activity_config_t base = auris_activity_defaults();
  const auris_activity_config_t c = auris_activity_defaults_for(cmb);
  /* Module 27: the CMB's gate weights bins, so its band reaches 200 Hz. */
  CHECK(cmb->snr_weighting && !rs->snr_weighting);
  CHECK(cmb->weighted_low_hz == 200.0f);
  CHECK(c.weighting && c.low_hz == 200.0f && c.high_hz == 6000.0f &&
        c.minimum_dbfs == -70.0f);
  CHECK(!auris_activity_defaults_for(rs).weighting &&
        auris_activity_defaults_for(rs).low_hz == 500.0f);
  CHECK(c.enabled == base.enabled && c.open_db == base.open_db &&
        c.close_db == base.close_db &&
        c.attack_s == base.attack_s && c.hold_s == base.hold_s &&
        c.calibration_s == base.calibration_s);
  const auris_activity_config_t none = auris_activity_defaults_for(NULL);
  CHECK(none.low_hz == base.low_hz && none.high_hz == base.high_hz &&
        none.minimum_dbfs == base.minimum_dbfs);
  /* The ReSpeaker keeps the original floor. */
  CHECK(auris_activity_defaults_for(rs).minimum_dbfs == base.minimum_dbfs);
  return EXIT_SUCCESS;
}

/* The level gate of Module 24: the CMB's 500-4000 Hz band, no weighting. */
static auris_activity_config_t unweighted_cmb(void) {
  auris_activity_config_t c = auris_activity_defaults_for(auris_capture_cmb());
  c.weighting = false;
  c.low_hz = auris_capture_cmb()->band_low_hz;
  c.high_hz = auris_capture_cmb()->band_high_hz;
  return c;
}

static int test_sizing(void) {
  auris_activity_config_t c = unweighted_cmb();
  static auris_activity_t s;
  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, SIZE, HOP, 383.625f) ==
        AURIS_OK);
  CHECK(s.size == SIZE && s.hop == HOP);
  /* Durations stay in seconds: hops are 10.67 ms at 48 kHz. */
  CHECK(s.warm_frames == 94U && s.attack_frames == 3U && s.hold_frames == 18U);
  /* 46.875 Hz bins: 500 Hz -> bin 11 (515.6 Hz), 4000 Hz -> bin 85. */
  CHECK(s.first_bin == 11U && s.last_bin == 85U);
  CHECK(fabsf(s.denominator - 8.0f * 1024.0f * 383.625f) < 1.0f);

  static auris_activity_t legacy;
  CHECK(auris_activity_init(&legacy, &c, 16000U, 4U, 191.625f) == AURIS_OK);
  CHECK(legacy.size == AURIS_ANALYSIS_SIZE && legacy.hop == AURIS_ANALYSIS_HOP);
  CHECK(legacy.attack_frames == 2U && legacy.hold_frames == 12U);

  auris_activity_config_t full = c;
  full.low_hz = 0.0f;
  full.high_hz = 24000.0f;
  CHECK(auris_activity_init_sized(&s, &full, RATE, 8U, SIZE, HOP, 383.625f) ==
        AURIS_OK);
  CHECK(s.first_bin == 1U && s.last_bin == 511U); /* DC, Nyquist excluded */

  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, 1000U, HOP, 383.625f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, SIZE, 2048U, 383.625f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, SIZE, 64U, 383.625f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, SIZE, HOP, 1025.0f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_activity_init_sized(&s, &c, RATE, 9U, SIZE, HOP, 383.625f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  full.high_hz = 24001.0f;
  CHECK(auris_activity_init_sized(&s, &full, RATE, 8U, SIZE, HOP, 383.625f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  /* A 128-sample frame has 375 Hz bins: none lies within 500-700 Hz. */
  c.low_hz = 500.0f;
  c.high_hz = 700.0f;
  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, 128U, 128U, 47.625f) ==
        AURIS_ERROR_INVALID_ARGUMENT);

  /* A spectrum of another size is rejected, not misread. */
  c = unweighted_cmb();
  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, SIZE, HOP, 383.625f) ==
        AURIS_OK);
  auris_activity_result_t out;
  spectrum = (auris_spectrum_t){.sample_rate = RATE, .channels = 8U,
                                .size = 512U};
  CHECK(auris_activity_apply(&s, &spectrum, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

/* Uniform noise in [-a, a] from a fixed linear congruential generator. */
static uint32_t seed = 12345U;
static float noise(float a) {
  seed = seed * 1664525U + 1013904223U;
  return a * ((float)(seed >> 8) / 8388608.0f - 1.0f);
}

/* The gate on the real pipeline: 1024-sample frames every 512 at 48 kHz,
   independent noise on 8 channels and a 1500 Hz tone from 1.5 to 2.5 s. */
static int test_pipeline(auris_fft_t *fft) {
  CHECK(auris_framer_init(&framer, SIZE, HOP) == AURIS_OK);
  CHECK(auris_preprocess_init_size(&prep, SIZE) == AURIS_OK);
  float window_energy = 0.0f;
  for (size_t n = 0U; n < SIZE; ++n) {
    window_energy += prep.window[n] * prep.window[n];
  }
  CHECK(fabsf(window_energy - 383.625f) < 0.01f);
  const auris_activity_config_t c = unweighted_cmb();
  static auris_activity_t s;
  CHECK(auris_activity_init_sized(&s, &c, RATE, 8U, SIZE, HOP,
                                  window_energy) == AURIS_OK);
  const uint64_t on = 72000U, off = 120000U, end = 192000U;
  int64_t opened = -1, closed = -1;
  size_t frames = 0U;
  float noise_db = 0.0f;
  for (uint64_t position = 0U; position < end; position += AURIS_AUDIO_FRAMES) {
    block = (auris_audio_block_t){.sample_rate = RATE, .channels = 8U,
                                  .frames = AURIS_AUDIO_FRAMES,
                                  .first_frame = position};
    for (size_t n = 0U; n < AURIS_AUDIO_FRAMES; ++n) {
      const uint64_t t = position + n;
      const float tone =
          t >= on && t < off
              ? 0.01f * sinf(6.2831853f * 1500.0f * (float)t / (float)RATE)
              : 0.0f;
      for (size_t m = 0U; m < 8U; ++m) {
        block.samples[m][n] = tone + noise(0.001f);
      }
    }
    bool ready = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (!ready) {
      continue;
    }
    CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
    CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
    /* Per-microphone band powers, measured before the gate masks bins. */
    double sum = 0.0;
    for (size_t m = 0U; m < 8U; ++m) {
      float p = 0.0f;
      CHECK(auris_spectrum_band_power(&spectrum, m, 500.0f, 4000.0f,
                                      window_energy, &p) == AURIS_OK);
      sum += (double)p;
    }
    auris_activity_result_t out;
    CHECK(auris_activity_apply(&s, &spectrum, &out) == AURIS_OK);
    CHECK(fabs(db(sum / 8.0) - (double)out.level_dbfs) < 0.01);
    CHECK(s.next_frame == frame.first_frame + HOP);
    ++frames;
    if (frames <= 94U) {
      CHECK(!out.ready && !out.active); /* one second of calibration */
    }
    const uint64_t f = frame.first_frame;
    if (f + SIZE <= on - SIZE) {
      CHECK(!out.active); /* noise alone never opens it */
      noise_db = out.noise_dbfs;
    }
    if (out.active && opened < 0) {
      opened = (int64_t)f;
    }
    if (opened >= 0 && !out.active && closed < 0) {
      closed = (int64_t)f;
    }
    if (f >= on && f + SIZE <= off) {
      CHECK(out.active && out.evidence);
      CHECK(fabsf(out.level_dbfs + 43.01f) < 0.2f); /* -40 dB - 3.01 dB */
    }
  }
  /* Noise: variance a^2/3 spread over 24 kHz, 3515.6 Hz of it in band. */
  const double expected_noise = db(1.0e-6 / 3.0 * (85.0 - 11.0 + 1.0) / 512.0);
  CHECK(fabs((double)noise_db - expected_noise) < 0.5);
  /* Opens once 3 frames hear the tone: first frame overlapping the onset
     plus two hops, give or take the window's quiet tails. */
  CHECK(opened >= (int64_t)(on - SIZE) && opened <= (int64_t)(on + HOP));
  /* Closes 18 hops (192 ms) after the last frame with tone energy. */
  CHECK(closed >= (int64_t)(off + 16U * HOP) &&
        closed <= (int64_t)(off + 19U * HOP));
  printf("opened at %.4f s, closed at %.4f s, noise %.2f dBFS (expect %.2f)\n",
         (double)opened / RATE, (double)closed / RATE, (double)noise_db,
         expected_noise);
  return EXIT_SUCCESS;
}

static int test_band_power(auris_fft_t *fft) {
  CHECK(auris_preprocess_init_size(&prep, SIZE) == AURIS_OK);
  float window_energy = 0.0f;
  for (size_t n = 0U; n < SIZE; ++n) {
    window_energy += prep.window[n] * prep.window[n];
  }
  frame = (auris_analysis_frame_t){.sample_rate = RATE, .channels = 2U,
                                   .size = SIZE};
  for (size_t n = 0U; n < SIZE; ++n) {
    frame.samples[0][n] = 0.01f * sinf(6.2831853f * 3000.0f * (float)n /
                                       (float)RATE); /* bin 64 exactly */
    frame.samples[1][n] = 0.001f * sinf(6.2831853f * 140.625f * (float)n /
                                        (float)RATE); /* bin 3 */
  }
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
  CHECK(auris_spectrum_apply(fft, &frame, &spectrum) == AURIS_OK);
  float p = 0.0f;
  /* A sine of amplitude A has power A^2 / 2: -43.01 dBFS for 0.01. */
  CHECK(auris_spectrum_band_power(&spectrum, 0U, 2828.4f, 5656.9f,
                                  window_energy, &p) == AURIS_OK);
  CHECK(fabs(db(p) + 43.01) < 0.05);
  CHECK(auris_spectrum_band_power(&spectrum, 0U, 500.0f, 4000.0f,
                                  window_energy, &p) == AURIS_OK);
  CHECK(fabs(db(p) + 43.01) < 0.05);
  CHECK(auris_spectrum_band_power(&spectrum, 0U, 353.6f, 707.1f,
                                  window_energy, &p) == AURIS_OK);
  CHECK(db(p) < -110.0); /* Hann leakage 2.3 kHz away */
  /* Hann spreads a bin-centred sine over three bins (2, 3, 4 here). */
  CHECK(auris_spectrum_band_power(&spectrum, 1U, 50.0f, 250.0f, window_energy,
                                  &p) == AURIS_OK);
  CHECK(fabs(db(p) + 63.01) < 0.05);
  CHECK(auris_spectrum_band_power(&spectrum, 1U, 88.4f, 176.8f, window_energy,
                                  &p) == AURIS_OK); /* bins 2-3 only */
  CHECK(fabs(db(p) + 63.80) < 0.05);
  CHECK(auris_spectrum_band_power(&spectrum, 1U, 500.0f, 4000.0f,
                                  window_energy, &p) == AURIS_OK);
  CHECK(db(p) < -100.0); /* the 140 Hz rumble is outside the band */
  /* Invalid requests. */
  CHECK(auris_spectrum_band_power(&spectrum, 2U, 500.0f, 4000.0f,
                                  window_energy, &p) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_spectrum_band_power(&spectrum, 0U, 4000.0f, 500.0f,
                                  window_energy, &p) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_spectrum_band_power(&spectrum, 0U, 520.0f, 550.0f,
                                  window_energy, &p) ==
        AURIS_ERROR_INVALID_ARGUMENT); /* no bin inside */
  CHECK(auris_spectrum_band_power(&spectrum, 0U, 500.0f, 4000.0f, 0.0f, &p) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_spectrum_band_power(NULL, 0U, 500.0f, 4000.0f, window_energy,
                                  &p) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_spectrum_band_power(&spectrum, 0U, 500.0f, 4000.0f,
                                  window_energy, NULL) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

int main(void) {
  auris_fft_t *fft = auris_fft_create_size(SIZE, 4U);
  int result = EXIT_FAILURE;
  if (fft != NULL && test_profiles() == EXIT_SUCCESS &&
      test_sizing() == EXIT_SUCCESS && test_band_power(fft) == EXIT_SUCCESS &&
      test_pipeline(fft) == EXIT_SUCCESS) {
    puts("activity runtime tests: ok");
    result = EXIT_SUCCESS;
  }
  auris_fft_destroy(fft);
  return result;
}
