#include "auris_replay.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition);        \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

int main(void) {
  int16_t pcm[(AURIS_AUDIO_FRAMES + 1U) * 2U];
  for (size_t n = 0U; n < AURIS_AUDIO_FRAMES + 1U; ++n) {
    pcm[2U * n] = (int16_t)n;
    pcm[2U * n + 1U] = (int16_t)(-(int)n);
  }
  pcm[0] = INT16_MIN;
  pcm[1] = INT16_MAX;

  auris_replay_t replay = {.pcm = pcm,
                           .total_frames = AURIS_AUDIO_FRAMES + 1U,
                           .position = 0U,
                           .channels = 2U,
                           .sample_rate = 16000U};
  auris_audio_source_t source = {.context = &replay, .read = auris_replay_read};
  auris_audio_block_t block;

  CHECK(source.read(source.context, &block) == AURIS_OK);
  CHECK(block.channels == 2U && block.sample_rate == 16000U);
  CHECK(block.frames == AURIS_AUDIO_FRAMES);
  CHECK(block.first_frame == 0U);
  CHECK(block.samples[0][0] == -1.0f);
  CHECK(block.samples[1][0] == 32767.0f / 32768.0f);
  for (size_t n = 1U; n < block.frames; ++n) {
    CHECK(block.samples[0][n] == (float)n / 32768.0f);
    CHECK(block.samples[1][n] == -(float)n / 32768.0f);
  }

  CHECK(source.read(source.context, &block) == AURIS_OK);
  CHECK(block.frames == 1U);
  CHECK(block.first_frame == AURIS_AUDIO_FRAMES);
  CHECK(block.samples[0][0] == (float)AURIS_AUDIO_FRAMES / 32768.0f);
  CHECK(block.samples[1][0] == -(float)AURIS_AUDIO_FRAMES / 32768.0f);

  for (size_t i = 0U; i < 2U; ++i) {
    CHECK(source.read(source.context, &block) == AURIS_OK);
    CHECK(block.frames == 0U);
    CHECK(block.first_frame == AURIS_AUDIO_FRAMES + 1U);
  }

  replay.channels = 0U;
  CHECK(source.read(source.context, &block) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(block.frames == 0U);

  puts("audio contract: ok");
  return EXIT_SUCCESS;
}
