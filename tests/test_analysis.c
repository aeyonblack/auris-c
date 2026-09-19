#include "auris_analysis.h"
#include "auris_replay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                  \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

static int test_replay_and_overlap(void) {
  int16_t pcm[800U * 2U];
  for (size_t n = 0U; n < 800U; ++n) {
    pcm[2U * n] = (int16_t)n;
    pcm[2U * n + 1U] = (int16_t)(1000U + n);
  }
  auris_replay_t replay = {.pcm = pcm,
                           .total_frames = 800U,
                           .position = 0U,
                           .channels = 2U,
                           .sample_rate = 16000U};
  auris_audio_source_t source = {&replay, auris_replay_read};
  auris_framer_t framer;
  auris_framer_reset(&framer);
  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  auris_audio_block_t block;
  auris_analysis_frame_t frame;
  size_t emitted = 0U;
  for (;;) {
    CHECK(source.read(source.context, &block) == AURIS_OK);
    bool ready = true;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (block.frames == 0U) {
      CHECK(!ready);
      break;
    }
    if (ready) {
      CHECK(frame.first_frame == (uint64_t)(emitted * 256U));
      CHECK(frame.channels == 2U && frame.sample_rate == 16000U);
      for (size_t c = 0U; c < 2U; ++c) {
        for (size_t n = 0U; n < 512U; ++n) {
          const float expected =
              (float)(c * 1000U + emitted * 256U + n) / 32768.0f;
          CHECK(frame.samples[c][n] == expected);
        }
      }
      /* Processing this output must not alter the next overlap. */
      CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
      ++emitted;
    }
  }
  CHECK(emitted == 2U);
  CHECK(framer.filled == 288U); /* 256 retained + 32 final samples */
  return EXIT_SUCCESS;
}

static int test_boundaries(void) {
  auris_framer_t framer;
  auris_framer_reset(&framer);
  auris_analysis_frame_t frame;
  auris_audio_block_t block = {.sample_rate = 16000U,
                               .channels = AURIS_AUDIO_CHANNELS};
  uint64_t position = 100U;
  size_t emitted = 0U;
  bool ready = false;

  /* 37-sample chunks force frame boundaries inside input blocks. */
  for (size_t b = 0U; b < 28U; ++b) {
    block.first_frame = position;
    block.frames = 37U;
    for (size_t c = 0U; c < block.channels; ++c) {
      for (size_t n = 0U; n < block.frames; ++n) {
        block.samples[c][n] =
            (float)(position + (uint64_t)n + (uint64_t)c * 2000U);
      }
    }
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    position += block.frames;
    if (ready) {
      CHECK(frame.first_frame == 100U + (uint64_t)emitted * 256U);
      for (size_t c = 0U; c < frame.channels; ++c) {
        for (size_t n = 0U; n < 512U; ++n) {
          CHECK(frame.samples[c][n] ==
                (float)(frame.first_frame + (uint64_t)n + (uint64_t)c * 2000U));
        }
      }
      ++emitted;
    }
  }
  CHECK(emitted == 3U);
  CHECK(framer.filled == 268U);

  const size_t before = framer.filled;
  block.first_frame = position + 1U; /* missing one audio frame */
  CHECK(auris_framer_push(&framer, &block, &frame, &ready) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(!ready && framer.filled == before && framer.next_frame == position);

  block.first_frame = position;
  block.sample_rate = 48000U;
  CHECK(auris_framer_push(&framer, &block, &frame, &ready) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  block.sample_rate = 16000U;
  block.channels = 1U;
  CHECK(auris_framer_push(&framer, &block, &frame, &ready) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  block.channels = AURIS_AUDIO_CHANNELS;
  block.frames = AURIS_AUDIO_FRAMES + 1U;
  CHECK(auris_framer_push(&framer, &block, &frame, &ready) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  block.frames = 37U;
  CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);

  auris_framer_reset(&framer);
  block.first_frame = 9000U;
  CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
  CHECK(!ready && framer.history.first_frame == 9000U);
  return EXIT_SUCCESS;
}

static int test_preprocessing(void) {
  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  CHECK(fabsf(prep.window[0]) < 0.000001f);
  CHECK(fabsf(prep.window[511]) < 0.000001f);
  CHECK(prep.window[255] > 0.9999f);
  for (size_t n = 0U; n < 512U; ++n) {
    CHECK(prep.window[n] >= 0.0f && prep.window[n] <= 1.0f);
    CHECK(fabsf(prep.window[n] - prep.window[511U - n]) < 0.000001f);
  }

  auris_analysis_frame_t frame = {
      .sample_rate = 16000U, .channels = 2U, .first_frame = 123U};
  for (size_t n = 0U; n < 512U; ++n) {
    frame.samples[0][n] = 0.25f;
    frame.samples[1][n] = -0.5f;
  }
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
  for (size_t n = 0U; n < 512U; ++n) {
    CHECK(frame.samples[0][n] == 0.0f);
    CHECK(frame.samples[1][n] == 0.0f);
  }

  /* Alternating signal has exactly known mean, amplitude, and sign. */
  for (size_t n = 0U; n < 512U; ++n) {
    const float ac = n % 2U == 0U ? 0.125f : -0.125f;
    frame.samples[0][n] = 0.25f + ac;
    frame.samples[1][n] = -0.5f - ac;
  }
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
  CHECK(frame.first_frame == 123U && frame.sample_rate == 16000U);
  for (size_t n = 0U; n < 512U; ++n) {
    const float ac = n % 2U == 0U ? 0.125f : -0.125f;
    CHECK(fabsf(frame.samples[0][n] - ac * prep.window[n]) < 0.000001f);
    CHECK(fabsf(frame.samples[1][n] + ac * prep.window[n]) < 0.000001f);
  }
  return EXIT_SUCCESS;
}

int main(void) {
  if (test_replay_and_overlap() != EXIT_SUCCESS ||
      test_boundaries() != EXIT_SUCCESS ||
      test_preprocessing() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("analysis tests: ok");
  return EXIT_SUCCESS;
}
