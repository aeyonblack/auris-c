#ifndef AURIS_REPLAY
#define AURIS_REPLAY

#include "auris_audio.h"
#include "auris_status.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const int16_t *pcm;
  size_t total_frames;
  size_t position;
  size_t channels;
  unsigned int sample_rate;
} auris_replay_t;

auris_status_t auris_replay_read(void *context, auris_audio_block_t *block);

#endif
