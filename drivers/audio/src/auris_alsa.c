#include "auris_alsa.h"
#include "auris_audio.h"
#include "auris_status.h"

#include <alsa/pcm.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static const auris_capture_profile_t RESPEAKER_PROFILE = {
    .device = NULL,
    .sample_rate = AURIS_ALSA_RATE,
    .raw_channels = AURIS_ALSA_RAW_CHANNELS,
    .format = AURIS_SAMPLE_S16_LE,
    .frame_size = 512U,
    .frame_hop = 256U,
    .gcc_upsample = 8U};

static snd_pcm_format_t alsa_format(auris_sample_format_t format) {
  return format == AURIS_SAMPLE_S16_LE ? SND_PCM_FORMAT_S16_LE
                                       : SND_PCM_FORMAT_S32_LE;
}

int auris_alsa_open_profile(auris_alsa_t *state,
                            const auris_capture_profile_t *profile,
                            const char *device, const size_t *map,
                            size_t channels) {
  if (state == NULL || profile == NULL || map == NULL || channels == 0U ||
      channels > AURIS_AUDIO_CHANNELS || profile->sample_rate == 0U ||
      profile->raw_channels == 0U ||
      profile->raw_channels > AURIS_AUDIO_RAW_CHANNELS_MAX ||
      auris_sample_bytes(profile->format) == 0U) {
    return -EINVAL;
  }
  if (device == NULL) {
    device = profile->device;
  }
  if (device == NULL) {
    return -EINVAL;
  }
  if (state->pcm != NULL) {
    return -EBUSY;
  }
  for (size_t c = 0U; c < channels; ++c) {
    if (map[c] >= profile->raw_channels) {
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
  err = snd_pcm_set_params(state->pcm, alsa_format(profile->format),
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           (unsigned int)profile->raw_channels,
                           profile->sample_rate, 0, 100000U);
  if (err < 0) {
    auris_alsa_close(state);
    return err;
  }

  err = snd_pcm_start(state->pcm);
  if (err < 0) {
    auris_alsa_close(state);
    return err;
  }

  state->profile = *profile;
  state->profile.device = NULL; /* the caller's string is not retained */
  state->frame_bytes =
      profile->raw_channels * auris_sample_bytes(profile->format);
  state->channels = channels;
  state->position = 0U;
  for (size_t c = 0U; c < channels; ++c) {
    state->map[c] = map[c];
  }
  return 0;
}

int auris_alsa_open(auris_alsa_t *state, const char *device, const size_t *map,
                    size_t channels) {
  if (device == NULL) {
    return -EINVAL;
  }
  return auris_alsa_open_profile(state, &RESPEAKER_PROFILE, device, map,
                                 channels);
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
  state->last_frames = 0U;

  size_t filled = 0U;
  while (filled < AURIS_AUDIO_FRAMES) {
    const snd_pcm_sframes_t got =
        snd_pcm_readi(state->pcm, &state->raw[filled * state->frame_bytes],
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

  const auris_status_t status = auris_audio_deinterleave(
      state->raw, state->profile.format, state->profile.raw_channels, filled,
      state->map, state->channels, block);
  if (status != AURIS_OK) {
    block->frames = 0U;
    return status;
  }
  block->sample_rate = state->profile.sample_rate;
  block->first_frame = state->position;
  state->position += (uint64_t)filled;
  state->last_frames = filled;
  return AURIS_OK;
}

const unsigned char *auris_alsa_raw(const auris_alsa_t *state, size_t *bytes) {
  if (state == NULL || bytes == NULL) {
    return NULL;
  }
  *bytes = state->last_frames * state->frame_bytes;
  return state->raw;
}

void auris_alsa_close(auris_alsa_t *state) {
  if (state != NULL && state->pcm != NULL) {
    (void)snd_pcm_close(state->pcm);
    state->pcm = NULL;
  }
}
