#ifndef AURIS_AUDIO_H
#define AURIS_AUDIO_H

#include "auris_status.h"
#include <stddef.h>
#include <stdint.h>

#define AURIS_AUDIO_CHANNELS 8U
#define AURIS_AUDIO_FRAMES 128U

typedef struct {
  unsigned int sample_rate;
  size_t channels;
  size_t frames;
  uint64_t first_frame;
  float samples[AURIS_AUDIO_CHANNELS][AURIS_AUDIO_FRAMES];
} auris_audio_block_t;

typedef struct {
  void *context;
  auris_status_t (*read)(void *context, auris_audio_block_t *block);
} auris_audio_source_t;

#endif
