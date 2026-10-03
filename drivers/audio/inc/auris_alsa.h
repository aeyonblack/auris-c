#ifndef AURIS_ALSA_H
#define AURIS_ALSA_H

#include "auris_audio.h"
#include "auris_status.h"
#include <alsa/asoundlib.h>

/* ReSpeaker values used by auris_alsa_open, kept for the applications
   that have not moved to capture profiles yet. */
#define AURIS_ALSA_RAW_CHANNELS 6U
#define AURIS_ALSA_RATE 16000U

#define AURIS_ALSA_MAX_FRAME_BYTES (AURIS_AUDIO_RAW_CHANNELS_MAX * 4U)

typedef struct {
  snd_pcm_t *pcm;
  auris_capture_profile_t profile;
  size_t frame_bytes;
  size_t channels;
  size_t map[AURIS_AUDIO_CHANNELS];
  uint64_t position;
  size_t last_frames;
  unsigned char raw[AURIS_AUDIO_FRAMES * AURIS_ALSA_MAX_FRAME_BYTES];
} auris_alsa_t;

/* Opens `device`, or profile->device when `device` is NULL, with the
   profile's channel count, format and rate. map[c] is the device channel
   for logical channel c. Returns 0 or a negative errno/ALSA code. */
int auris_alsa_open_profile(auris_alsa_t *state,
                            const auris_capture_profile_t *profile,
                            const char *device, const size_t *map,
                            size_t channels);

/* ReSpeaker profile (6 channels, S16_LE, 16 kHz) on `device`. */
int auris_alsa_open(auris_alsa_t *state, const char *device, const size_t *map,
                    size_t channels);

auris_status_t auris_alsa_read(void *context, auris_audio_block_t *block);

/* The raw device bytes behind the most recent successful read, before
   conversion and mapping. Valid until the next read or close. */
const unsigned char *auris_alsa_raw(const auris_alsa_t *state, size_t *bytes);

void auris_alsa_close(auris_alsa_t *state);

#endif
