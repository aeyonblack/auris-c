#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_audio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

static void put_s16(unsigned char *p, int32_t v) {
  const uint32_t u = (uint32_t)v;
  p[0] = (unsigned char)(u & 0xFFU);
  p[1] = (unsigned char)((u >> 8U) & 0xFFU);
}

static void put_s32(unsigned char *p, int64_t v) {
  const uint32_t u = (uint32_t)(v & 0xFFFFFFFFLL);
  for (unsigned int i = 0U; i < 4U; ++i) {
    p[i] = (unsigned char)((u >> (8U * i)) & 0xFFU);
  }
}

static int test_scaling(void) {
  CHECK(auris_sample_bytes(AURIS_SAMPLE_S16_LE) == 2U);
  CHECK(auris_sample_bytes(AURIS_SAMPLE_S32_LE) == 4U);
  CHECK(auris_sample_bytes((auris_sample_format_t)7) == 0U);

  /* S16: full scale, zero, +/-1 LSB, one channel. */
  const int32_t s16[5] = {-32768, 32767, 0, 1, -1};
  unsigned char raw16[5 * 2];
  for (size_t n = 0U; n < 5U; ++n) put_s16(&raw16[2U * n], s16[n]);
  const size_t map0[1] = {0U};
  auris_audio_block_t block;
  memset(&block, 0, sizeof(block));
  CHECK(auris_audio_deinterleave(raw16, AURIS_SAMPLE_S16_LE, 1U, 5U, map0, 1U,
                                 &block) == AURIS_OK);
  CHECK(block.channels == 1U && block.frames == 5U);
  CHECK(block.samples[0][0] == -1.0f);
  CHECK(block.samples[0][1] == 32767.0f / 32768.0f);
  CHECK(block.samples[0][2] == 0.0f);
  CHECK(block.samples[0][3] == 1.0f / 32768.0f);
  CHECK(block.samples[0][4] == -1.0f / 32768.0f);

  /* S32 with a 24-bit sample in the upper bits: low byte always zero. */
  const int64_t s32[6] = {-2147483648LL, 0x7FFFFF00LL, 0, 0x100LL, -0x100LL,
                          0x40000000LL};
  unsigned char raw32[6 * 4];
  for (size_t n = 0U; n < 6U; ++n) put_s32(&raw32[4U * n], s32[n]);
  CHECK(auris_audio_deinterleave(raw32, AURIS_SAMPLE_S32_LE, 1U, 6U, map0, 1U,
                                 &block) == AURIS_OK);
  CHECK(block.samples[0][0] == -1.0f);
  CHECK(block.samples[0][1] == 1.0f - 1.0f / 8388608.0f); /* 1 - 2^-23 */
  CHECK(block.samples[0][2] == 0.0f);
  CHECK(block.samples[0][3] == 1.0f / 8388608.0f);
  CHECK(block.samples[0][4] == -1.0f / 8388608.0f);
  CHECK(block.samples[0][5] == 0.5f);
  return EXIT_SUCCESS;
}

static int test_mapping(void) {
  /* 8-channel S32 frames where device channel k of frame n holds a marker
     (k + 1) * 1000 + n, shifted into the upper 24 bits. */
  enum { FRAMES = AURIS_AUDIO_FRAMES };
  static unsigned char raw[FRAMES * 8 * 4];
  for (size_t n = 0U; n < FRAMES; ++n) {
    for (size_t k = 0U; k < 8U; ++k) {
      put_s32(&raw[(n * 8U + k) * 4U], ((int64_t)(k + 1U) * 1000 + (int64_t)n)
                                           * 256);
    }
  }
  const auris_array_t *cmb = auris_config_cmb_8();
  auris_audio_block_t block;
  CHECK(auris_audio_deinterleave(raw, AURIS_SAMPLE_S32_LE, 8U, FRAMES,
                                 cmb->capture_channels, 8U, &block) == AURIS_OK);
  CHECK(block.channels == 8U && block.frames == FRAMES);
  for (size_t m = 0U; m < 8U; ++m) {
    const size_t k = cmb->capture_channels[m];
    for (size_t n = 0U; n < FRAMES; ++n) {
      const float expected =
          (float)(((double)(k + 1U) * 1000.0 + (double)n) * 256.0 /
                  2147483648.0);
      CHECK(block.samples[m][n] == expected);
    }
  }

  /* ReSpeaker-style: 6 S16 channels, mics on device channels 1..4. */
  static unsigned char raw16[FRAMES * 6 * 2];
  for (size_t n = 0U; n < FRAMES; ++n) {
    for (size_t k = 0U; k < 6U; ++k) {
      put_s16(&raw16[(n * 6U + k) * 2U], (int32_t)(k * 1000U + n));
    }
  }
  const auris_array_t *rs = auris_config_respeaker_usb_4();
  CHECK(auris_audio_deinterleave(raw16, AURIS_SAMPLE_S16_LE, 6U, FRAMES,
                                 rs->capture_channels, 4U, &block) == AURIS_OK);
  for (size_t m = 0U; m < 4U; ++m) {
    for (size_t n = 0U; n < FRAMES; ++n) {
      CHECK(block.samples[m][n] ==
            (float)(rs->capture_channels[m] * 1000U + n) / 32768.0f);
    }
  }

  /* Rejections. */
  const size_t bad[2] = {0U, 8U};
  CHECK(auris_audio_deinterleave(raw, AURIS_SAMPLE_S32_LE, 8U, FRAMES, bad, 2U,
                                 &block) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_audio_deinterleave(raw, AURIS_SAMPLE_S32_LE, 8U, FRAMES + 1U,
                                 cmb->capture_channels, 8U,
                                 &block) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_audio_deinterleave(raw, AURIS_SAMPLE_S32_LE, 9U, FRAMES,
                                 cmb->capture_channels, 8U,
                                 &block) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_audio_deinterleave(raw, AURIS_SAMPLE_S32_LE, 8U, FRAMES,
                                 cmb->capture_channels, 0U,
                                 &block) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_audio_deinterleave(raw, (auris_sample_format_t)9, 8U, FRAMES,
                                 cmb->capture_channels, 8U,
                                 &block) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_audio_deinterleave(NULL, AURIS_SAMPLE_S32_LE, 8U, FRAMES,
                                 cmb->capture_channels, 8U,
                                 &block) == AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

static int test_profiles(void) {
  auris_config_t cmb, rs;
  CHECK(auris_config_lookup("cmb", &cmb));
  CHECK(auris_config_lookup("respeaker", &rs));
  CHECK(!auris_config_lookup("nope", &cmb) && !auris_config_lookup(NULL, &cmb));
  CHECK(auris_config_lookup("cmb", &cmb));
  CHECK(cmb.array == auris_config_cmb_8() && cmb.capture == auris_capture_cmb());
  CHECK(strcmp(cmb.name, "cmb") == 0);
  CHECK(strcmp(cmb.capture->device, "hw:AurisCMB") == 0);
  CHECK(cmb.capture->sample_rate == 48000U && cmb.capture->raw_channels == 8U &&
        cmb.capture->format == AURIS_SAMPLE_S32_LE);
  CHECK(rs.array == auris_config_respeaker_usb_4());
  CHECK(strcmp(rs.capture->device, "hw:CARD=ArrayUAC10,DEV=0") == 0);
  CHECK(rs.capture->sample_rate == 16000U && rs.capture->raw_channels == 6U &&
        rs.capture->format == AURIS_SAMPLE_S16_LE);
  CHECK(auris_config_find("cmb") == cmb.array);
  /* Every mapped channel exists on its device. */
  const auris_config_t both[2] = {cmb, rs};
  for (size_t i = 0U; i < 2U; ++i) {
    for (size_t m = 0U; m < both[i].array->microphone_count; ++m) {
      CHECK(both[i].array->capture_channels[m] < both[i].capture->raw_channels);
    }
  }
  return EXIT_SUCCESS;
}

static int test_driver_validation(void) {
  /* Every case is rejected before any ALSA device is opened. */
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  const size_t map[8] = {0U, 4U, 5U, 1U, 2U, 6U, 7U, 3U};
  const size_t bad[2] = {0U, 8U};
  const size_t dup[2] = {3U, 3U};
  auris_alsa_t state = {0};
  CHECK(auris_alsa_open_profile(&state, NULL, NULL, map, 8U) == -EINVAL);
  CHECK(auris_alsa_open_profile(&state, cmb, NULL, bad, 2U) == -EINVAL);
  CHECK(auris_alsa_open_profile(&state, cmb, NULL, dup, 2U) == -EINVAL);
  CHECK(auris_alsa_open_profile(&state, cmb, NULL, map, 0U) == -EINVAL);
  CHECK(auris_alsa_open_profile(&state, cmb, NULL, map, 9U) == -EINVAL);
  auris_capture_profile_t odd = *cmb;
  odd.raw_channels = 9U;
  CHECK(auris_alsa_open_profile(&state, &odd, NULL, map, 8U) == -EINVAL);
  odd = *cmb;
  odd.device = NULL;
  CHECK(auris_alsa_open_profile(&state, &odd, NULL, map, 8U) == -EINVAL);
  /* The ReSpeaker entry point keeps its 6-channel limit. */
  const size_t seven[1] = {6U};
  CHECK(auris_alsa_open(&state, "hw:CARD=ArrayUAC10,DEV=0", seven, 1U) ==
        -EINVAL);
  CHECK(state.pcm == NULL);
  CHECK(sizeof(state.raw) >= AURIS_AUDIO_FRAMES * 8U * 4U);
  return EXIT_SUCCESS;
}

int main(void) {
  if (test_scaling() != EXIT_SUCCESS || test_mapping() != EXIT_SUCCESS ||
      test_profiles() != EXIT_SUCCESS ||
      test_driver_validation() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("capture format tests: ok");
  return EXIT_SUCCESS;
}
