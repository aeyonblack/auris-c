#include "auris_array.h"
#include "auris_status.h"

#include <math.h>
#include <stddef.h>

static float vector_distance(auris_vec3f_t first, auris_vec3f_t second) {
  const float dx = second.x_m - first.x_m;
  const float dy = second.y_m - first.y_m;
  const float dz = second.z_m - first.z_m;

  return sqrtf((dx * dx) + (dy * dy) + (dz * dz));
}

size_t auris_array_pair_count(size_t microphone_count) {
  if (microphone_count < 2U) {
    return 0U;
  }
  return (microphone_count * (microphone_count - 1U)) / 2U;
}

auris_status_t auris_array_validate(const auris_array_t *array) {
  size_t first;
  size_t second;

  if (array == NULL || array->name == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  if (array->microphone_count < 2U ||
      array->microphone_count > AURIS_MAX_MICROPHONES) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  if (!isfinite(array->speed_of_sound_mps) ||
      array->speed_of_sound_mps <= 0.0F) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  for (first = 0U; first < array->microphone_count; ++first) {
    const auris_vec3f_t position = array->positions[first];

    if (!isfinite(position.x_m) || !isfinite(position.y_m) ||
        !isfinite(position.z_m)) {
      return AURIS_ERROR_INVALID_ARGUMENT;
    }

    for (second = first + 1U; second < array->microphone_count; ++second) {
      if (array->capture_channels[first] == array->capture_channels[second]) {
        return AURIS_ERROR_INVALID_ARGUMENT;
      }

      if (vector_distance(position, array->positions[second]) <= 0.0F) {
        return AURIS_ERROR_INVALID_ARGUMENT;
      }
    }
  }

  return AURIS_OK;
}

auris_status_t auris_array_generate_pairs(const auris_array_t *array,
                                          auris_mic_pair_t *pairs,
                                          size_t pair_capacity,
                                          size_t *pair_count) {
  size_t first;
  size_t second;
  size_t output_index = 0U;
  size_t required_count;
  auris_status_t status;

  if (pairs == NULL || pair_count == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  status = auris_array_validate(array);
  if (status != AURIS_OK) {
    return status;
  }

  required_count = auris_array_pair_count(array->microphone_count);
  if (pair_capacity < required_count) {
    *pair_count = required_count;
    return AURIS_ERROR_CAPACITY;
  }

  for (first = 0U; first < array->microphone_count; ++first) {
    for (second = first + 1U; second < array->microphone_count; ++second) {
      pairs[output_index].first = first;
      pairs[output_index].second = second;
      pairs[output_index].distance_m =
          vector_distance(array->positions[first], array->positions[second]);
      ++output_index;
    }
  }

  *pair_count = output_index;
  return AURIS_OK;
}

auris_status_t auris_array_spacing(const auris_array_t *array,
                                   float *minimum_spacing_m,
                                   float *aperture_m) {
  auris_mic_pair_t pairs[AURIS_MAX_PAIRS];
  size_t pair_count;
  size_t index;
  float minimum;
  float maximum;
  auris_status_t status;

  if (minimum_spacing_m == NULL || aperture_m == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  status =
      auris_array_generate_pairs(array, pairs, AURIS_MAX_PAIRS, &pair_count);
  if (status != AURIS_OK) {
    return status;
  }

  minimum = pairs[0].distance_m;
  maximum = pairs[0].distance_m;

  for (index = 1U; index < pair_count; ++index) {
    if (pairs[index].distance_m < minimum) {
      minimum = pairs[index].distance_m;
    }

    if (pairs[index].distance_m > maximum) {
      maximum = pairs[index].distance_m;
    }
  }

  *minimum_spacing_m = minimum;
  *aperture_m = maximum;
  return AURIS_OK;
}

auris_status_t auris_array_aliasing_estimate_hz(const auris_array_t *array,
                                                float *frequency_hz) {
  float minimum_spacing_m;
  float aperture_m;
  auris_status_t status;

  if (frequency_hz == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  status = auris_array_spacing(array, &minimum_spacing_m, &aperture_m);
  if (status != AURIS_OK) {
    return status;
  }

  (void)aperture_m;
  *frequency_hz = array->speed_of_sound_mps / (2.0F * minimum_spacing_m);
  return AURIS_OK;
}

auris_status_t auris_pair_max_tdoa_samples(const auris_mic_pair_t *pair,
                                           float speed_of_sound_mps,
                                           float sample_rate_hz,
                                           float *maximum_delay_samples) {
  if (pair == NULL || maximum_delay_samples == NULL ||
      !isfinite(speed_of_sound_mps) || speed_of_sound_mps <= 0.0F ||
      !isfinite(sample_rate_hz) || sample_rate_hz <= 0.0F ||
      !isfinite(pair->distance_m) || pair->distance_m <= 0.0F) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  *maximum_delay_samples =
      (pair->distance_m / speed_of_sound_mps) * sample_rate_hz;

  return AURIS_OK;
}
