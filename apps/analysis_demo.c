#include "auris_analysis.h"
#include "auris_replay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int16_t pcm[800U * 2U];
  for (size_t n = 0U; n < 800U; ++n) {
    for (size_t c = 0U; c < 2U; ++c) {
      const float delayed_n = (float)n - 3.0f * (float)c;
      const float phase =
          6.2831853071795864769f * 440.0f * delayed_n / 16000.0f;
      const float value = 0.2f + 0.3f * sinf(phase);
      pcm[2U * n + c] = (int16_t)lrintf(value * 32768.0f);
    }
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
  auris_analysis_frame_t raw;
  auris_analysis_frame_t prepared;
  size_t outputs = 0U;

  for (;;) {
    if (source.read(source.context, &block) != AURIS_OK) {
      return EXIT_FAILURE;
    }
    if (block.frames == 0U) {
      break;
    }
    bool ready = false;
    if (auris_framer_push(&framer, &block, &raw, &ready) != AURIS_OK) {
      return EXIT_FAILURE;
    }
    if (!ready) {
      continue;
    }
    if (outputs == 0U) {
      prepared = raw;
      if (auris_preprocess_apply(&prep, &prepared) != AURIS_OK) {
        return EXIT_FAILURE;
      }
      puts("sample raw0 raw1 prepared0 prepared1 window");
      for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
        printf("%zu %.9g %.9g %.9g %.9g %.9g\n", n, (double)raw.samples[0][n],
               (double)raw.samples[1][n], (double)prepared.samples[0][n],
               (double)prepared.samples[1][n], (double)prep.window[n]);
      }
    }
    ++outputs;
  }
  fprintf(stderr, "analysis frames: %zu (expected 2)\n", outputs);
  return outputs == 2U ? EXIT_SUCCESS : EXIT_FAILURE;
}
