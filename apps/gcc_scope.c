#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_spectrum.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float) == 4U, "scope requires 32-bit float");

static volatile sig_atomic_t keep_running = 1;

static void stop_capture(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static bool write_floats(const float *data, size_t count) {
  return fwrite(data, sizeof(float), count, stdout) == count;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s ALSA_DEVICE | --synthetic\n", argv[0]);
    return EXIT_FAILURE;
  }
  struct sigaction action = {0};
  action.sa_handler = stop_capture;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) != 0 ||
      sigaction(SIGTERM, &action, NULL) != 0) {
    return EXIT_FAILURE;
  }
  action.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &action, NULL) != 0) {
    return EXIT_FAILURE;
  }

  const bool synthetic = strcmp(argv[1], "--synthetic") == 0;
  const size_t mic_a = 0U;
  const size_t mic_b = synthetic ? 1U : 2U;
  const auris_array_t *array = auris_config_respeaker_usb_4();
  auris_mic_pair_t pairs[AURIS_MAX_PAIRS];
  size_t pair_count = 0U;
  float physical_limit = synthetic ? 8.0f : 0.0f;
  if (!synthetic) {
    if (auris_array_generate_pairs(array, pairs, AURIS_MAX_PAIRS,
                                   &pair_count) != AURIS_OK) {
      return EXIT_FAILURE;
    }
    for (size_t p = 0U; p < pair_count; ++p) {
      if (pairs[p].first == mic_a && pairs[p].second == mic_b) {
        if (auris_pair_max_tdoa_samples(&pairs[p], array->speed_of_sound_mps,
                                        (float)AURIS_ALSA_RATE,
                                        &physical_limit) != AURIS_OK) {
          return EXIT_FAILURE;
        }
      }
    }
    if (physical_limit <= 0.0f) {
      return EXIT_FAILURE;
    }
  }
  const int max_lag = (int)ceilf(physical_limit);
  if (max_lag >= (int)AURIS_ANALYSIS_SIZE / 2) {
    return EXIT_FAILURE;
  }

  /* Allocate/plan DSP before starting ALSA capture. */
  auris_fft_t *fft = auris_fft_create();
  if (fft == NULL) {
    fprintf(stderr, "FFT initialization failed\n");
    return EXIT_FAILURE;
  }
  auris_framer_t framer;
  auris_framer_reset(&framer);
  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  auris_analysis_frame_t analysis;
  auris_spectrum_t spectrum;
  auris_gcc_t gcc;
  auris_audio_block_t block;
  auris_alsa_t state = {0};
  int result = EXIT_FAILURE;
  if (!synthetic) {
    const int err = auris_alsa_open(&state, argv[1], array->capture_channels,
                                    array->microphone_count);
    if (err < 0) {
      fprintf(stderr, "open: %s\n", snd_strerror(err));
      goto done;
    }
  }
  auris_audio_source_t source = {&state, auris_alsa_read};
  uint64_t position = 0U;
  uint32_t seed = 1U;
  float delay_line[3] = {0};
  size_t cursor = 0U;
  if (printf("AURIS3 %u %u %zu %zu %.9g\n", AURIS_ALSA_RATE,
             AURIS_ANALYSIS_SIZE, mic_a, mic_b, (double)physical_limit) < 0) {
    goto done;
  }
  while (keep_running) {
    if (synthetic) {
      if (position >= (uint64_t)AURIS_ALSA_RATE * 10U) {
        break;
      }
      block.sample_rate = AURIS_ALSA_RATE;
      block.channels = 2U;
      block.frames = AURIS_AUDIO_FRAMES;
      block.first_frame = position;
      for (size_t n = 0U; n < block.frames; ++n) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        const float value = 0.2f * ((float)(seed >> 8) / 16777216.0f - 0.5f);
        block.samples[0][n] = 0.2f + value;
        block.samples[1][n] = 0.2f + delay_line[cursor];
        delay_line[cursor] = value;
        cursor = (cursor + 1U) % 3U;
      }
      position += (uint64_t)block.frames;
    } else if (source.read(source.context, &block) != AURIS_OK) {
      if (!keep_running) {
        break;
      }
      fprintf(stderr, "capture failed\n");
      goto done;
    }
    bool ready = false;
    if (auris_framer_push(&framer, &block, &analysis, &ready) != AURIS_OK) {
      fprintf(stderr, "invalid or discontinuous audio\n");
      goto done;
    }
    if (!ready) {
      continue;
    }
    if (auris_preprocess_apply(&prep, &analysis) != AURIS_OK ||
        auris_spectrum_apply(fft, &analysis, &spectrum) != AURIS_OK ||
        auris_gcc_phat(fft, &spectrum, mic_a, mic_b, max_lag, &gcc) !=
            AURIS_OK) {
      fprintf(stderr, "DSP failed\n");
      goto done;
    }
    const float summary[3] = {(float)gcc.lag_samples, gcc.peak,
                              gcc.valid ? 1.0f : 0.0f};
    if (!write_floats(summary, 3U) ||
        !write_floats(analysis.samples[mic_a], AURIS_ANALYSIS_SIZE) ||
        !write_floats(analysis.samples[mic_b], AURIS_ANALYSIS_SIZE)) {
      goto done;
    }
    /* Each bin is one array of two floats; transmit those arrays. */
    for (size_t side = 0U; side < 2U; ++side) {
      const size_t mic = side == 0U ? mic_a : mic_b;
      for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
        if (!write_floats(spectrum.bins[mic][k], 2U)) {
          goto done;
        }
      }
    }
    if (!write_floats(gcc.correlation, AURIS_ANALYSIS_SIZE) ||
        fflush(stdout) == EOF) {
      goto done;
    }
  }
  result = EXIT_SUCCESS;
done:
  auris_alsa_close(&state);
  auris_fft_destroy(fft);
  return result;
}
