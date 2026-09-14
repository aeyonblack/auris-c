#include "auris_array.h"
#include "auris_array_configs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int expect_true(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAILED: %s\n", message);
    return 0;
  }

  return 1;
}

static int expect_near(float actual, float expected, float tolerance,
                       const char *message) {
  if (fabsf(actual - expected) > tolerance) {
    fprintf(stderr, "FAILED: %s: expected %.6f, received %.6f\n", message,
            expected, actual);
    return 0;
  }

  return 1;
}

int main(void) {
  const auris_array_t *array = auris_config_respeaker_usb_4();
  auris_mic_pair_t pairs[AURIS_MAX_PAIRS];
  size_t pair_count = 0U;
  float minimum_spacing_m = 0.0F;
  float aperture_m = 0.0F;
  float aliasing_hz = 0.0F;
  float diameter_delay_samples = 0.0F;
  auris_status_t status;

  status = auris_array_validate(array);
  if (!expect_true(status == AURIS_OK, "configuration must be valid")) {
    return EXIT_FAILURE;
  }

  if (!expect_true(auris_array_pair_count(4U) == 6U,
                   "four microphones must produce six pairs")) {
    return EXIT_FAILURE;
  }

  if (!expect_true(auris_array_pair_count(8U) == 28U,
                   "eight microphones must produce twenty-eight pairs")) {
    return EXIT_FAILURE;
  }

  status =
      auris_array_generate_pairs(array, pairs, AURIS_MAX_PAIRS, &pair_count);
  if (!expect_true(status == AURIS_OK, "pair generation must succeed")) {
    return EXIT_FAILURE;
  }

  if (!expect_true(pair_count == 6U, "generated pair count must be six")) {
    return EXIT_FAILURE;
  }

  if (!expect_true(pairs[0].first == 0U && pairs[0].second == 1U,
                   "first pair must be (0,1)")) {
    return EXIT_FAILURE;
  }

  if (!expect_true(pairs[5].first == 2U && pairs[5].second == 3U,
                   "last pair must be (2,3)")) {
    return EXIT_FAILURE;
  }

  status = auris_array_spacing(array, &minimum_spacing_m, &aperture_m);
  if (!expect_true(status == AURIS_OK, "spacing must succeed")) {
    return EXIT_FAILURE;
  }

  if (!expect_near(minimum_spacing_m, 0.0452548F, 0.000001F,
                   "minimum spacing")) {
    return EXIT_FAILURE;
  }

  if (!expect_near(aperture_m, 0.064F, 0.000001F, "aperture")) {
    return EXIT_FAILURE;
  }

  status = auris_array_aliasing_estimate_hz(array, &aliasing_hz);
  if (!expect_true(status == AURIS_OK, "alias estimate must succeed")) {
    return EXIT_FAILURE;
  }

  if (!expect_near(aliasing_hz, 3789.0F, 2.0F, "alias estimate")) {
    return EXIT_FAILURE;
  }

  status = auris_pair_max_tdoa_samples(&pairs[1], array->speed_of_sound_mps,
                                       16000.0F, &diameter_delay_samples);
  if (!expect_true(status == AURIS_OK, "TDOA must succeed")) {
    return EXIT_FAILURE;
  }

  if (!expect_near(diameter_delay_samples, 2.98542F, 0.0001F,
                   "diameter delay at 16 kHz")) {
    return EXIT_FAILURE;
  }

  status = auris_array_generate_pairs(array, pairs, 5U, &pair_count);
  if (!expect_true(status == AURIS_ERROR_CAPACITY,
                   "small pair buffer must report capacity error")) {
    return EXIT_FAILURE;
  }

  if (!expect_true(pair_count == 6U,
                   "capacity error must report required pair count")) {
    return EXIT_FAILURE;
  }

  printf("array tests passed\n");
  return EXIT_SUCCESS;
}
