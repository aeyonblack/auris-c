#include "auris_array.h"
#include "auris_array_configs.h"
#include "auris_status.h"

#include <stdio.h>
#include <stdlib.h>

#define REPORT_SAMPLE_RATE_HZ 16000.0F

static int print_error(const char *operation, auris_status_t status) {
  fprintf(stderr, "%s: %s\n", operation, auris_status_string(status));
  return EXIT_FAILURE;
}

int main(void) {
  const auris_array_t *array = auris_config_respeaker_usb_4();
  auris_mic_pair_t pairs[AURIS_MAX_PAIRS];
  size_t pair_count;
  size_t index;
  float minimum_spacing_m;
  float aperture_m;
  float aliasing_hz;
  auris_status_t status;

  status = auris_array_validate(array);
  if (status != AURIS_OK) {
    return print_error("array validation failed", status);
  }

  status =
      auris_array_generate_pairs(array, pairs, AURIS_MAX_PAIRS, &pair_count);
  if (status != AURIS_OK) {
    return print_error("pair generation failed", status);
  }

  status = auris_array_spacing(array, &minimum_spacing_m, &aperture_m);
  if (status != AURIS_OK) {
    return print_error("spacing calculation failed", status);
  }

  status = auris_array_aliasing_estimate_hz(array, &aliasing_hz);
  if (status != AURIS_OK) {
    return print_error("aliasing estimate failed", status);
  }

  printf("Array: %s\n", array->name);
  printf("Microphones: %zu\n", array->microphone_count);
  printf("Unique pairs: %zu\n", pair_count);
  printf("Sound speed: %.1f m/s\n", array->speed_of_sound_mps);
  printf("Minimum spacing: %.3f mm\n", minimum_spacing_m * 1000.0F);
  printf("Aperture: %.3f mm\n", aperture_m * 1000.0F);
  printf("Adjacent-spacing alias estimate: %.1f Hz\n", aliasing_hz);

  printf("\nMicrophones\n");
  for (index = 0U; index < array->microphone_count; ++index) {
    const auris_vec3f_t position = array->positions[index];

    printf("  mic %zu: channel=%zu position=(%+.3f, %+.3f, %+.3f) m\n", index,
           array->capture_channels[index], position.x_m, position.y_m,
           position.z_m);
  }

  printf("\nPairs at %.0f Hz sample rate\n", REPORT_SAMPLE_RATE_HZ);
  for (index = 0U; index < pair_count; ++index) {
    float maximum_delay_samples;

    status = auris_pair_max_tdoa_samples(
        &pairs[index], array->speed_of_sound_mps, REPORT_SAMPLE_RATE_HZ,
        &maximum_delay_samples);
    if (status != AURIS_OK) {
      return print_error("TDOA calculation failed", status);
    }

    printf("  (%zu,%zu): distance=%7.3f mm  max |delay|=%5.3f samples\n",
           pairs[index].first, pairs[index].second,
           pairs[index].distance_m * 1000.0F, maximum_delay_samples);
  }

  return EXIT_SUCCESS;
}
