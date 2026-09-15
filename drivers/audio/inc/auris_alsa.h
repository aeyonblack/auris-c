#ifndef AURIS_ALSA_H
#define AURIS_ALSA_H

#include "auris_audio.h"
#include "auris_status.h"
#include <alsa/asoundlib.h>

#define AURIS_ALSA_RAW_CHANNELS 6U
#define AURIS_ALSA_RATE 16000U

typedef struct {
  snd_pcm_t *pcm;
  size_t channels;
  size_t map[AURIS_AUDIO_CHANNELS];
  uint64_t position;
  int16_t raw[AURIS_AUDIO_FRAMES * AURIS_ALSA_RAW_CHANNELS];
} auris_alsa_t;

int auris_alsa_open(auris_alsa_t *state, const char *device, const size_t *map,
                    size_t channels);

auris_status_t auris_alsa_read(void *context, auris_audio_block_t *block);

void auris_alsa_close(auris_alsa_t *state);

#endif
