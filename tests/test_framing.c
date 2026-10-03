#include "auris_analysis.h"
#include "auris_array_configs.h"
#include "auris_spectrum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

/* Pushes `total` frames of an 8-channel 48 kHz ramp (value = channel * 1e5
   + absolute index) in 128-frame blocks starting at `origin`, checking every
   emitted frame. Returns the number of frames emitted, or -1. */
static long run(auris_framer_t *framer, size_t size, size_t hop,
                uint64_t origin, size_t total) {
  static auris_audio_block_t block;
  static auris_analysis_frame_t frame;
  long emitted = 0;
  for (size_t done = 0U; done < total; done += AURIS_AUDIO_FRAMES) {
    block = (auris_audio_block_t){.sample_rate = 48000U, .channels = 8U,
                                  .frames = AURIS_AUDIO_FRAMES,
                                  .first_frame = origin + done};
    for (size_t c = 0U; c < 8U; ++c) {
      for (size_t n = 0U; n < AURIS_AUDIO_FRAMES; ++n) {
        block.samples[c][n] = (float)(c * 100000U + done + n);
      }
    }
    bool ready = false;
    if (auris_framer_push(framer, &block, &frame, &ready) != AURIS_OK) {
      return -1;
    }
    if (!ready) continue;
    const uint64_t start = origin + (uint64_t)emitted * hop;
    if (frame.size != size || frame.first_frame != start ||
        frame.channels != 8U || frame.sample_rate != 48000U) {
      return -1;
    }
    /* The frame completes exactly at the end of this block. */
    if (start + size != origin + done + AURIS_AUDIO_FRAMES) {
      return -1;
    }
    for (size_t c = 0U; c < 8U; ++c) {
      for (size_t n = 0U; n < size; ++n) {
        if (frame.samples[c][n] !=
            (float)(c * 100000U + (start - origin) + n)) {
          return -1;
        }
      }
    }
    ++emitted;
  }
  return emitted;
}

static int test_limits(void) {
  CHECK(auris_analysis_size_valid(512U, 256U));
  CHECK(auris_analysis_size_valid(1024U, 512U));
  CHECK(auris_analysis_size_valid(2048U, 1024U));
  CHECK(auris_analysis_size_valid(64U, 64U) == false); /* hop below a block */
  CHECK(auris_analysis_size_valid(128U, 128U));
  CHECK(auris_analysis_size_valid(2048U, 2048U));
  CHECK(!auris_analysis_size_valid(1536U, 768U));  /* not a power of two */
  CHECK(!auris_analysis_size_valid(4096U, 2048U)); /* above the maximum */
  CHECK(!auris_analysis_size_valid(32U, 16U));
  CHECK(!auris_analysis_size_valid(1024U, 127U));
  CHECK(!auris_analysis_size_valid(1024U, 1025U));
  CHECK(!auris_analysis_size_valid(0U, 0U));

  auris_framer_t framer;
  CHECK(auris_framer_init(&framer, 1536U, 768U) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_framer_init(NULL, 1024U, 512U) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_framer_init(&framer, 1024U, 512U) == AURIS_OK);
  CHECK(framer.size == 1024U && framer.hop == 512U && framer.filled == 0U &&
        !framer.active);
  auris_framer_reset(&framer); /* the default frame, as before */
  CHECK(framer.size == AURIS_ANALYSIS_SIZE && framer.hop == AURIS_ANALYSIS_HOP);
  return EXIT_SUCCESS;
}

static int test_schedules(void) {
  static auris_framer_t framer;
  /* CMB default: 1024/512. 4096 samples -> frames at 0,512,...,3072. */
  CHECK(auris_framer_init(&framer, 1024U, 512U) == AURIS_OK);
  CHECK(run(&framer, 1024U, 512U, 0U, 4096U) == 7);
  CHECK(framer.filled == 512U);

  /* Restart keeps the configuration and accepts a new timeline. */
  auris_framer_restart(&framer);
  CHECK(framer.size == 1024U && framer.hop == 512U && !framer.active);
  CHECK(run(&framer, 1024U, 512U, 96000U, 2048U) == 3);

  /* Longest frame, 75% overlap, and a non-zero origin. */
  CHECK(auris_framer_init(&framer, 2048U, 512U) == AURIS_OK);
  CHECK(run(&framer, 2048U, 512U, 4800U, 4096U) == 5);

  /* Hop equal to the frame: no overlap. */
  CHECK(auris_framer_init(&framer, 256U, 256U) == AURIS_OK);
  CHECK(run(&framer, 256U, 256U, 0U, 1024U) == 4);

  /* A gap is still rejected at the new size. */
  CHECK(auris_framer_init(&framer, 1024U, 512U) == AURIS_OK);
  CHECK(run(&framer, 1024U, 512U, 0U, 1024U) == 1);
  static auris_audio_block_t block;
  static auris_analysis_frame_t frame;
  block = (auris_audio_block_t){.sample_rate = 48000U, .channels = 8U,
                                .frames = 128U, .first_frame = 1024U + 1U};
  bool ready = true;
  CHECK(auris_framer_push(&framer, &block, &frame, &ready) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(!ready);
  return EXIT_SUCCESS;
}

static int test_window(void) {
  static auris_preprocess_t prep;
  CHECK(auris_preprocess_init_size(&prep, 1024U) == AURIS_OK);
  CHECK(prep.size == 1024U);
  CHECK(fabsf(prep.window[0]) < 1e-6f && fabsf(prep.window[1023]) < 1e-6f);
  double energy = 0.0;
  for (size_t n = 0U; n < 1024U; ++n) {
    CHECK(fabsf(prep.window[n] - prep.window[1023U - n]) < 1e-6f);
    energy += (double)prep.window[n] * (double)prep.window[n];
  }
  /* Sum of a symmetric Hann's squares is 3(N - 1)/8. */
  CHECK(fabs(energy - 3.0 * 1023.0 / 8.0) < 1e-3);
  CHECK(auris_preprocess_init_size(&prep, 4096U) == AURIS_ERROR_INVALID_ARGUMENT);
  auris_preprocess_init(&prep);
  CHECK(prep.size == AURIS_ANALYSIS_SIZE);

  /* Mean removal and windowing over the frame's own length only. */
  static auris_analysis_frame_t frame;
  CHECK(auris_preprocess_init_size(&prep, 1024U) == AURIS_OK);
  frame = (auris_analysis_frame_t){.sample_rate = 48000U, .channels = 1U,
                                   .size = 1024U};
  for (size_t n = 0U; n < AURIS_ANALYSIS_MAX_SIZE; ++n) {
    frame.samples[0][n] = n < 1024U ? 0.25f + (n % 2U == 0U ? 0.125f : -0.125f)
                                    : 99.0f;
  }
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_OK);
  for (size_t n = 0U; n < 1024U; ++n) {
    const float ac = n % 2U == 0U ? 0.125f : -0.125f;
    CHECK(fabsf(frame.samples[0][n] - ac * prep.window[n]) < 1e-6f);
  }
  CHECK(frame.samples[0][1024] == 99.0f); /* beyond the frame: untouched */

  /* A frame of another size is refused. */
  frame.size = 512U;
  CHECK(auris_preprocess_apply(&prep, &frame) == AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

static int test_profiles_and_guard(void) {
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  const auris_capture_profile_t *rs = auris_capture_respeaker_usb();
  CHECK(cmb->frame_size == 1024U && cmb->frame_hop == 512U);
  CHECK(rs->frame_size == 512U && rs->frame_hop == 256U);
  CHECK(auris_analysis_size_valid(cmb->frame_size, cmb->frame_hop));
  CHECK(auris_analysis_size_valid(rs->frame_size, rs->frame_hop));

  /* The spectrum stage still works at the default size only (Module 21
     generalizes it), and must refuse anything else rather than misread. */
  auris_fft_t *fft = auris_fft_create();
  CHECK(fft != NULL);
  static auris_analysis_frame_t frame;
  static auris_spectrum_t spectrum;
  frame = (auris_analysis_frame_t){.sample_rate = 48000U, .channels = 8U,
                                   .size = 1024U};
  const auris_status_t refused = auris_spectrum_apply(fft, &frame, &spectrum);
  frame.size = AURIS_ANALYSIS_SIZE;
  const auris_status_t accepted = auris_spectrum_apply(fft, &frame, &spectrum);
  auris_fft_destroy(fft);
  CHECK(refused == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(accepted == AURIS_OK);
  return EXIT_SUCCESS;
}

int main(void) {
  if (test_limits() != EXIT_SUCCESS || test_schedules() != EXIT_SUCCESS ||
      test_window() != EXIT_SUCCESS ||
      test_profiles_and_guard() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("runtime framing tests: ok");
  return EXIT_SUCCESS;
}
