#include "auris_analysis.h"
#include "auris_status.h"

#include <math.h>
#include <stddef.h>

auris_status_t auris_preprocess_init_size(auris_preprocess_t *state,
                                          size_t size) {
  if (state == NULL || size < 2U || size > AURIS_ANALYSIS_MAX_SIZE) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float two_pi = 6.2831853071795864769f;
  state->size = size;
  for (size_t n = 0U; n < size; ++n) {
    const float phase = two_pi * (float)n / (float)(size - 1U);
    state->window[n] = 0.5f * (1.0f - cosf(phase));
  }
  return AURIS_OK;
}

void auris_preprocess_init(auris_preprocess_t *state) {
  (void)auris_preprocess_init_size(state, AURIS_ANALYSIS_SIZE);
}

auris_status_t auris_preprocess_apply(const auris_preprocess_t *state,
                                      auris_analysis_frame_t *frame) {
  if (state == NULL || frame == NULL || frame->channels == 0U ||
      frame->channels > AURIS_AUDIO_CHANNELS || frame->sample_rate == 0U ||
      frame->size != state->size) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const size_t size = state->size;
  for (size_t c = 0U; c < frame->channels; ++c) {
    float sum = 0.0f;
    for (size_t n = 0U; n < size; ++n) {
      sum += frame->samples[c][n];
    }
    const float mean = sum / (float)size;
    for (size_t n = 0U; n < size; ++n) {
      frame->samples[c][n] = (frame->samples[c][n] - mean) * state->window[n];
    }
  }
  return AURIS_OK;
}
