#ifndef AURIS_AUDIO_H
#define AURIS_AUDIO_H

#include "auris_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AURIS_AUDIO_CHANNELS 8U
#define AURIS_AUDIO_FRAMES 128U
#define AURIS_AUDIO_RAW_CHANNELS_MAX 8U /* channels in one device frame */

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

typedef enum {
  AURIS_SAMPLE_S16_LE = 0,
  AURIS_SAMPLE_S32_LE = 1,
} auris_sample_format_t;

typedef struct {
  const char *device;
  unsigned int sample_rate;
  size_t raw_channels;
  auris_sample_format_t format;
  size_t frame_size; /* analysis frame length in samples */
  size_t frame_hop;  /* samples between consecutive analysis frames */
  size_t gcc_upsample; /* fine GCC points per sample */
  float band_low_hz;   /* localization and activity band */
  float band_high_hz;
  float gate_minimum_dbfs; /* activity gate's absolute floor for this gain */
  bool snr_weighting;      /* weight PHAT bins by their SNR; gate on weight */
  float weighted_low_hz;   /* the band's lower edge when weighting is on */
  float coherence_min;     /* least SRP coherence for a map to be shown */
} auris_capture_profile_t;

size_t auris_sample_bytes(auris_sample_format_t format);

auris_status_t auris_audio_deinterleave(const void *raw,
                                        auris_sample_format_t format,
                                        size_t raw_channels, size_t frames,
                                        const size_t *map, size_t channels,
                                        auris_audio_block_t *block);

#endif
