#include "auris_array.h"
#include "auris_array_configs.h"
#include "auris_status.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int print_error(const char *operation, auris_status_t status) {
  fprintf(stderr, "%s: %s\n", operation, auris_status_string(status));
  return EXIT_FAILURE;
}

static int usage(const char *program) {
  fprintf(stderr, "usage: %s [--array NAME] [--rate HZ]\n  NAME: %s\n", program,
          auris_config_names());
  return EXIT_FAILURE;
}

int main(int argc, char **argv) {
  const char *name = "respeaker";
  unsigned long rate = 16000UL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
      char *end = NULL;
      errno = 0;
      rate = strtoul(argv[++i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0' || argv[i][0] == '-' ||
          rate == 0UL || rate > 384000UL) {
        return usage(argv[0]);
      }
    } else {
      return usage(argv[0]);
    }
  }
  const auris_array_t *array = auris_config_find(name);
  if (array == NULL) {
    fprintf(stderr, "unknown array '%s'\n", name);
    return usage(argv[0]);
  }
  const float sample_rate_hz = (float)rate;
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
  printf("Maximum |delay| at %lu Hz: %.3f samples\n", rate,
         aperture_m / array->speed_of_sound_mps * sample_rate_hz);

  printf("\nMicrophones (array frame, viewed from behind)\n");
  for (index = 0U; index < array->microphone_count; ++index) {
    const auris_vec3f_t position = array->positions[index];
    float bearing = atan2f(position.y_m, position.x_m) * 57.29578F;
    if (bearing < 0.0F) {
      bearing += 360.0F;
    }
    printf("  mic %zu: channel=%zu position=(%+.4f, %+.4f, %+.4f) m"
           "  bearing=%6.2f deg\n",
           index, array->capture_channels[index], position.x_m, position.y_m,
           position.z_m, bearing);
  }

  printf("\nPairs at %lu Hz sample rate\n", rate);
  for (index = 0U; index < pair_count; ++index) {
    float maximum_delay_samples;

    status =
        auris_pair_max_tdoa_samples(&pairs[index], array->speed_of_sound_mps,
                                    sample_rate_hz, &maximum_delay_samples);
    if (status != AURIS_OK) {
      return print_error("TDOA calculation failed", status);
    }

    printf("  (%zu,%zu): distance=%7.3f mm  max |delay|=%6.3f samples\n",
           pairs[index].first, pairs[index].second,
           pairs[index].distance_m * 1000.0F, maximum_delay_samples);
  }

  return EXIT_SUCCESS;
}
