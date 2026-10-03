#include "auris_audio.h"
#include <stdint.h>

size_t auris_sample_bytes(auris_sample_format_t format) {
  switch (format) {
  case AURIS_SAMPLE_S16_LE:
    return 2U;
  case AURIS_SAMPLE_S32_LE:
    return 4U;
  default:
    return 0U;
  }
}

static float s16_le(const unsigned char *p) {
  int32_t value = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8U));
  if (value >= 0x8000) {
    value -= 0x10000;
  }
  return (float)value / 32768.0f;
}

static float s32_le(const unsigned char *p) {
  const uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
                        ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
  const int64_t value =
      bits >= 0x80000000U ? (int64_t)bits - 4294967296LL : (int64_t)bits;
  return (float)value / 2147483648.0f;
}

auris_status_t auris_audio_deinterleave(const void *raw,
                                        auris_sample_format_t format,
                                        size_t raw_channels, size_t frames,
                                        const size_t *map, size_t channels,
                                        auris_audio_block_t *block) {
  const size_t bytes = auris_sample_bytes(format);
  if (raw == NULL || map == NULL || block == NULL || bytes == 0U ||
      raw_channels == 0U || raw_channels > AURIS_AUDIO_RAW_CHANNELS_MAX ||
      channels == 0U || channels > AURIS_AUDIO_CHANNELS ||
      frames > AURIS_AUDIO_FRAMES) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  for (size_t c = 0U; c < channels; ++c) {
    if (map[c] >= raw_channels) {
      return AURIS_ERROR_INVALID_ARGUMENT;
    }
  }
  const unsigned char *base = raw;
  const size_t frame_bytes = raw_channels * bytes;
  for (size_t c = 0U; c < channels; ++c) {
    const unsigned char *p = base + map[c] * bytes;
    for (size_t n = 0U; n < frames; ++n, p += frame_bytes) {
      block->samples[c][n] =
          format == AURIS_SAMPLE_S16_LE ? s16_le(p) : s32_le(p);
    }
  }
  block->channels = channels;
  block->frames = frames;
  return AURIS_OK;
}
