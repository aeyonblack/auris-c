#include "auris_alsa.h"
#include "auris_audio.h"
#include "auris_status.h"

#include <alsa/pcm.h>
#include <asm-generic/errno-base.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

int auris_alsa_open(auris_alsa_t *state, const char *device, const size_t *map,
                    size_t channels) {
  if (state == NULL || device == NULL || map == NULL || channels == 0U ||
      channels > AURIS_AUDIO_CHANNELS) {
    return -EINVAL;
  }
  if (state->pcm != NULL) {
    return -EBUSY;
  }
  for (size_t c = 0U; c < channels; ++c) {
    if (map[c] >= AURIS_ALSA_RAW_CHANNELS) {
      return -EINVAL;
    }
    for (size_t j = 0U; j < c; ++j) {
      if (map[c] == map[j]) {
        return -EINVAL;
      }
    }
  }

  int err = snd_pcm_open(&state->pcm, device, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) {
    state->pcm = NULL;
    return err;
  }
  err = snd_pcm_set_params(
      state->pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
      AURIS_ALSA_RAW_CHANNELS, AURIS_ALSA_RATE, 0, 100000U);
  if (err < 0) {
    auris_alsa_close(state);
    return err;
  }

  err = snd_pcm_start(state->pcm);
  if (err < 0) {
    auris_alsa_close(state);
    return err;
  }

  state->channels = channels;
  state->position = 0U;
  for (size_t c = 0U; c < channels; ++c) {
    state->map[c] = map[c];
  }
  return 0;
}

auris_status_t auris_alsa_read(void *context, auris_audio_block_t *block) {
  auris_alsa_t *state = context;
  if (block == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  block->frames = 0U;
  if (state == NULL || state->pcm == NULL) {
    return AURIS_ERROR_NOT_READY;
  }

  size_t filled = 0U;
  while (filled < AURIS_AUDIO_FRAMES) {
    const snd_pcm_sframes_t got =
        snd_pcm_readi(state->pcm, &state->raw[filled * AURIS_ALSA_RAW_CHANNELS],
                      (snd_pcm_uframes_t)(AURIS_AUDIO_FRAMES - filled));
    if (got == -EINTR) {
      continue;
    }
    if (got <= 0) {
      fprintf(stderr, "ALSA capture stopped: %s\n",
              got < 0 ? snd_strerror((int)got) : "zero-frame read");
      return AURIS_ERROR_INTERNAL;
    }
    filled += (size_t)got;
  }

  for (size_t c = 0U; c < state->channels; ++c) {
    for (size_t n = 0U; n < filled; ++n) {
      block->samples[c][n] =
          (float)state->raw[n * AURIS_ALSA_RAW_CHANNELS + state->map[c]] /
          32768.0f;
    }
  }
  block->sample_rate = AURIS_ALSA_RATE;
  block->channels = state->channels;
  block->first_frame = state->position;
  block->frames = filled;
  state->position += (uint64_t)filled;
  return AURIS_OK;
}

void auris_alsa_close(auris_alsa_t *state) {
  if (state != NULL && state->pcm != NULL) {
    (void)snd_pcm_close(state->pcm);
    state->pcm = NULL;
  }
}
