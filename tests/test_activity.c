#include "auris_activity.h"

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

/* A known spectral power fixture isolates gate timing from waveform overlap. */
static auris_status_t step(auris_activity_t *state, float db, size_t bin,
                           auris_activity_result_t *out) {
  auris_spectrum_t spectrum = {
      .sample_rate = 16000U, .channels = 4U, .first_frame = state->next_frame};
  const float power = powf(10.0f, db / 10.0f);
  const float magnitude = sqrtf(power * 512.0f * 191.625f / 2.0f);
  for (size_t m = 0U; m < 4U; ++m) {
    spectrum.bins[m][bin][0] = magnitude;
  }
  return auris_activity_apply(state, &spectrum, out);
}

int main(void) {
  auris_activity_config_t config = auris_activity_defaults();
  config.calibration_s = 0.032f;
  config.hold_s = 0.048f;
  auris_activity_t state = {0};
  auris_activity_result_t out;
  CHECK(auris_activity_init(&state, &config, 16000U, 4U, 191.625f) == AURIS_OK);
  CHECK(state.warm_frames == 2U && state.attack_frames == 2U &&
        state.hold_frames == 3U);
  CHECK(step(&state, -60.0f, 32U, &out) == AURIS_OK && !out.ready);
  CHECK(step(&state, -60.0f, 32U, &out) == AURIS_OK && !out.ready);
  CHECK(step(&state, -60.0f, 32U, &out) == AURIS_OK && out.ready &&
        !out.active);
  CHECK(fabsf(out.level_dbfs + 60.0f) < 0.001f);
  CHECK(fabsf(out.open_dbfs + 50.0f) < 0.001f);
  CHECK(fabsf(out.close_dbfs + 54.0f) < 0.001f);
  CHECK(step(&state, -48.0f, 32U, &out) == AURIS_OK && !out.active);
  CHECK(step(&state, -48.0f, 32U, &out) == AURIS_OK && out.active);
  CHECK(step(&state, -52.0f, 32U, &out) == AURIS_OK && out.active &&
        out.evidence);
  for (size_t n = 0U; n < 200U; ++n) {
    CHECK(step(&state, -48.0f, 32U, &out) == AURIS_OK && out.active);
  }
  CHECK(fabsf(out.noise_dbfs + 60.0f) < 0.001f); /* Target not learned away. */
  CHECK(step(&state, -60.0f, 32U, &out) == AURIS_OK && out.active &&
        !out.evidence);
  CHECK(step(&state, -60.0f, 32U, &out) == AURIS_OK && out.active);
  CHECK(step(&state, -60.0f, 32U, &out) == AURIS_OK && !out.active);
  /* A brief loud impulse cannot satisfy the attack counter. */
  CHECK(step(&state, -40.0f, 32U, &out) == AURIS_OK && !out.active);
  CHECK(step(&state, -60.0f, 32U, &out) == AURIS_OK && !out.active);
  /* Slow sub-threshold background rise is tracked. */
  for (size_t n = 0U; n < 1000U; ++n) {
    CHECK(step(&state, -58.0f, 32U, &out) == AURIS_OK && !out.active);
  }
  CHECK(out.noise_dbfs > -59.0f && out.noise_dbfs < -58.0f);
  auris_activity_reset(&state);
  for (size_t n = 0U; n < 20U; ++n) {
    CHECK(step(&state, -10.0f, 200U, &out) == AURIS_OK && !out.active);
    CHECK(out.level_dbfs <= -119.9f); /* 6250 Hz excluded. */
  }
  for (size_t n = 0U; n < 20U; ++n) {
    CHECK(step(&state, -75.0f, 32U, &out) == AURIS_OK && !out.active);
  }
  CHECK(step(&state, -45.0f, 32U, &out) == AURIS_OK && !out.active);
  CHECK(step(&state, -45.0f, 32U, &out) == AURIS_OK && out.active);
  auris_spectrum_t gap = {.sample_rate = 16000U,
                          .channels = 4U,
                          .first_frame = state.next_frame + 256U};
  CHECK(auris_activity_apply(&state, &gap, &out) == AURIS_OK && !out.ready &&
        !out.active);

  config = auris_activity_defaults();
  config.enabled = false;
  CHECK(auris_activity_init(&state, &config, 16000U, 4U, 191.625f) == AURIS_OK);
  CHECK(step(&state, -120.0f, 32U, &out) == AURIS_OK);
  CHECK(out.ready && out.active && out.evidence);
  CHECK(state.warm_seen == 0U && out.open_dbfs == -120.0f &&
        out.close_dbfs == -120.0f);

  config = auris_activity_defaults();
  config.close_db = config.open_db;
  CHECK(auris_activity_init(&state, &config, 16000U, 4U, 191.625f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  puts("activity tests: ok");
  return EXIT_SUCCESS;
}
