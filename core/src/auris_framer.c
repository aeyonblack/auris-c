#include "auris_analysis.h"
#include "auris_audio.h"
#include "auris_status.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(AURIS_ANALYSIS_HOP > 0U &&
                   AURIS_ANALYSIS_HOP <= AURIS_ANALYSIS_SIZE,
               "hop must fit inside the analysis frame");
_Static_assert(AURIS_AUDIO_FRAMES <= AURIS_ANALYSIS_HOP,
               "one input block must produce at most one output frame");

void auris_framer_reset(auris_framer_t *state) {
  if (state != NULL) {
    *state = (auris_framer_t){0};
  }
}

auris_status_t auris_framer_push(auris_framer_t *state,
                                 const auris_audio_block_t *block,
                                 auris_analysis_frame_t *out, bool *ready) {
  if (ready == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *ready = false;
  if (state == NULL || block == NULL || out == NULL || block->channels == 0U ||
      block->channels > AURIS_AUDIO_CHANNELS || block->sample_rate == 0U ||
      block->frames > AURIS_AUDIO_FRAMES ||
      block->first_frame > UINT64_MAX - (uint64_t)block->frames) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  if (block->frames == 0U) {
    return AURIS_OK;
  }

  if (state->active) {
    if (block->sample_rate != state->history.sample_rate ||
        block->channels != state->history.channels ||
        block->first_frame != state->next_frame) {
      return AURIS_ERROR_INVALID_ARGUMENT;
    }
  } else {
    state->history.sample_rate = block->sample_rate;
    state->history.channels = block->channels;
    state->history.first_frame = block->first_frame;
    state->active = true;
  }

  size_t offset = 0U;
  while (offset < block->frames) {
    size_t count = AURIS_ANALYSIS_SIZE - state->filled;
    if (count > block->frames - offset) {
      count = block->frames - offset;
    }
    for (size_t c = 0U; c < block->channels; ++c) {
      memcpy(&state->history.samples[c][state->filled],
             &block->samples[c][offset], count * sizeof(float));
    }
    state->filled += count;
    offset += count;

    if (state->filled == AURIS_ANALYSIS_SIZE) {
      *out = state->history;
      *ready = true;
      const size_t retained = AURIS_ANALYSIS_SIZE - AURIS_ANALYSIS_HOP;
      for (size_t c = 0U; c < block->channels; ++c) {
        memmove(state->history.samples[c],
                &state->history.samples[c][AURIS_ANALYSIS_HOP],
                retained * sizeof(float));
      }
      state->filled = retained;
      state->history.first_frame += AURIS_ANALYSIS_HOP;
    }
  }
  state->next_frame = block->first_frame + (uint64_t)block->frames;
  return AURIS_OK;
}
