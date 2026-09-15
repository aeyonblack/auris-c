#include "auris_replay.h"
#include "auris_audio.h"
#include "auris_status.h"
#include <stddef.h>
#include <stdint.h>

auris_status_t auris_replay_read(void *context, auris_audio_block_t *block) {
  auris_replay_t *replay = context;

  if (block == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  block->frames = 0U;

  if (replay == NULL || replay->channels == 0U ||
      replay->channels > AURIS_AUDIO_CHANNELS || replay->sample_rate == 0U ||
      replay->position > replay->total_frames ||
      replay->total_frames > SIZE_MAX / replay->channels ||
      (replay->pcm == NULL && replay->total_frames != 0U)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }

  size_t count = replay->total_frames - replay->position;
  if (count > AURIS_AUDIO_FRAMES) {
    count = AURIS_AUDIO_FRAMES;
  }

  block->sample_rate = replay->sample_rate;
  block->channels = replay->channels;
  block->first_frame = (uint64_t)replay->position;

  for (size_t c = 0U; c < replay->channels; ++c) {
    for (size_t n = 0U; n < count; ++n) {
      const size_t index = (replay->position + n) * replay->channels + c;
      block->samples[c][n] = (float)replay->pcm[index] / 32768.0f;
    }
  }

  replay->position += count;
  block->frames = count;
  return AURIS_OK;
}
