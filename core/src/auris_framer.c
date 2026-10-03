#include "auris_analysis.h"
#include "auris_audio.h"
#include "auris_status.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(AURIS_ANALYSIS_HOP >= AURIS_AUDIO_FRAMES &&
                   AURIS_ANALYSIS_HOP <= AURIS_ANALYSIS_SIZE &&
                   AURIS_ANALYSIS_SIZE <= AURIS_ANALYSIS_MAX_SIZE,
               "default frame must satisfy the runtime limits");

bool auris_analysis_size_valid(size_t size, size_t hop) {
  /* A power of two has exactly one bit set: size & (size - 1) == 0. */
  return size >= AURIS_ANALYSIS_MIN_SIZE && size <= AURIS_ANALYSIS_MAX_SIZE &&
         (size & (size - 1U)) == 0U &&
         /* One input block may complete at most one frame. */
         hop >= AURIS_AUDIO_FRAMES && hop <= size;
}

auris_status_t auris_framer_init(auris_framer_t *state, size_t size,
                                 size_t hop) {
  if (state == NULL || !auris_analysis_size_valid(size, hop)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  state->size = size;
  state->hop = hop;
  auris_framer_restart(state);
  return AURIS_OK;
}

void auris_framer_reset(auris_framer_t *state) {
  (void)auris_framer_init(state, AURIS_ANALYSIS_SIZE, AURIS_ANALYSIS_HOP);
}

void auris_framer_restart(auris_framer_t *state) {
  if (state != NULL) {
    state->filled = 0U;
    state->next_frame = 0U;
    state->active = false;
    state->history.sample_rate = 0U;
    state->history.channels = 0U;
    state->history.size = state->size;
    state->history.first_frame = 0U;
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
      block->first_frame > UINT64_MAX - (uint64_t)block->frames ||
      !auris_analysis_size_valid(state->size, state->hop)) {
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
    state->history.size = state->size;
    state->history.first_frame = block->first_frame;
    state->active = true;
  }

  const size_t size = state->size;
  size_t offset = 0U;
  while (offset < block->frames) {
    size_t count = size - state->filled;
    if (count > block->frames - offset) {
      count = block->frames - offset;
    }
    for (size_t c = 0U; c < block->channels; ++c) {
      memcpy(&state->history.samples[c][state->filled],
             &block->samples[c][offset], count * sizeof(float));
    }
    state->filled += count;
    offset += count;

    if (state->filled == size) {
      /* Copy only the valid part of each row, not the whole capacity. */
      out->sample_rate = state->history.sample_rate;
      out->channels = state->history.channels;
      out->size = size;
      out->first_frame = state->history.first_frame;
      for (size_t c = 0U; c < block->channels; ++c) {
        memcpy(out->samples[c], state->history.samples[c],
               size * sizeof(float));
      }
      *ready = true;
      const size_t retained = size - state->hop;
      for (size_t c = 0U; c < block->channels; ++c) {
        memmove(state->history.samples[c],
                &state->history.samples[c][state->hop],
                retained * sizeof(float));
      }
      state->filled = retained;
      state->history.first_frame += state->hop;
    }
  }
  state->next_frame = block->first_frame + (uint64_t)block->frames;
  return AURIS_OK;
}
