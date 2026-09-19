#include "auris_analysis.h"
#include "auris_status.h"

#include <math.h>
#include <stddef.h>

void auris_preprocess_init(auris_preprocess_t *state) {
  if (state == NULL) {
    return;
  }
  const float two_pi = 6.2831853071795864769f;
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    const float phase = two_pi * (float)n / (float)(AURIS_ANALYSIS_SIZE - 1U);
    state->window[n] = 0.5f * (1.0f - cosf(phase));
  }
}

auris_status_t auris_preprocess_apply(const auris_preprocess_t *state,
                                      auris_analysis_frame_t *frame) {
  if (state == NULL || frame == NULL || frame->channels == 0U ||
      frame->channels > AURIS_AUDIO_CHANNELS || frame->sample_rate == 0U) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  for (size_t c = 0U; c < frame->channels; ++c) {
    float sum = 0.0f;
    for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
      sum += frame->samples[c][n];
    }
    const float mean = sum / (float)AURIS_ANALYSIS_SIZE;
    for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
      frame->samples[c][n] = (frame->samples[c][n] - mean) * state->window[n];
    }
  }
  return AURIS_OK;
}
