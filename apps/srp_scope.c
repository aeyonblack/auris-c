#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_srp.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIRECTIONS 360U

_Static_assert(sizeof(float) == 4U, "scope requires float32");
static volatile sig_atomic_t keep_running = 1;

static void stop_capture(int signum) {
  (void)signum;
  keep_running = 0;
}

static bool write_floats(const float *data, size_t count) {
  return fwrite(data, sizeof(float), count, stdout) == count;
}

static void synthetic_block(auris_audio_block_t *block,
                            const auris_array_t *array, float degrees,
                            uint64_t position) {
  const double angle = (double)degrees * 0.01745329251994329577;
  block->sample_rate = AURIS_ALSA_RATE;
  block->channels = array->microphone_count;
  block->frames = AURIS_AUDIO_FRAMES;
  block->first_frame = position;
  for (size_t m = 0U; m < block->channels; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance =
        ((double)p.x_m * cos(angle) + (double)p.y_m * sin(angle)) /
        (double)array->speed_of_sound_mps;
    for (size_t n = 0U; n < block->frames; ++n) {
      const double time =
          (double)(position + (uint64_t)n) / (double)AURIS_ALSA_RATE + advance;
      double value = 0.2;
      for (size_t k = 5U; k < 225U; k += 7U) {
        const double frequency = (double)k * (double)AURIS_ALSA_RATE / 512.0;
        value +=
            0.006 * sin(6.283185307179586 * frequency * time + (double)(k * k));
      }
      block->samples[m][n] = (float)value;
    }
  }
}

int main(int argc, char **argv) {
  const bool synthetic = argc == 3 && strcmp(argv[1], "--synthetic") == 0;
  if ((!synthetic && argc != 2) ||
      (argc == 2 && strcmp(argv[1], "--synthetic") == 0)) {
    fprintf(stderr, "usage: %s ALSA_DEVICE | --synthetic ANGLE_DEG\n", argv[0]);
    return EXIT_FAILURE;
  }
  float target = -1.0f;
  if (synthetic) {
    char *end = NULL;
    errno = 0;
    target = strtof(argv[2], &end);
    if (errno != 0 || end == argv[2] || *end != '\0' || !isfinite(target) ||
        target < 0.0f || target >= 360.0f) {
      fprintf(stderr, "angle must be in [0, 360)\n");
      return EXIT_FAILURE;
    }
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

  const auris_array_t *array = auris_config_respeaker_usb_4();
  auris_vec3f_t grid[DIRECTIONS];
  for (size_t q = 0U; q < DIRECTIONS; ++q) {
    const float angle = (float)q * 0.01745329251994329577f;
    grid[q] = (auris_vec3f_t){cosf(angle), sinf(angle), 0.0f};
  }
  auris_fft_t *fft = auris_fft_create();
  auris_srp_t *srp = auris_srp_create(array, AURIS_ALSA_RATE, grid, DIRECTIONS);
  auris_alsa_t state = {0};
  int result = EXIT_FAILURE;
  if (fft == NULL || srp == NULL) {
    fprintf(stderr, "DSP setup failed\n");
    goto done;
  }
  auris_framer_t framer;
  auris_framer_reset(&framer);
  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  auris_analysis_frame_t analysis;
  auris_spectrum_t spectrum;
  auris_srp_result_t output;
  auris_audio_block_t block;
  auris_audio_source_t source = {&state, auris_alsa_read};
  const size_t pair_count = auris_array_pair_count(array->microphone_count);

  /* Stream metadata before acquisition begins. */
  if (printf("AURIS4 %u %u %zu %zu %.9g\n", AURIS_ALSA_RATE, DIRECTIONS,
             array->microphone_count, pair_count, (double)target) < 0) {
    goto done;
  }
  for (size_t m = 0U; m < array->microphone_count; ++m) {
    const float xyz[3] = {array->positions[m].x_m, array->positions[m].y_m,
                          array->positions[m].z_m};
    if (!write_floats(xyz, 3U)) {
      goto done;
    }
  }
  if (fflush(stdout) == EOF) {
    goto done;
  }
  if (!synthetic) {
    const int err = auris_alsa_open(&state, argv[1], array->capture_channels,
                                    array->microphone_count);
    if (err < 0) {
      fprintf(stderr, "open: %s\n", snd_strerror(err));
      goto done;
    }
  }
  uint64_t position = 0U;
  while (keep_running) {
    if (synthetic) {
      if (position >= (uint64_t)AURIS_ALSA_RATE * 10U) {
        break;
      }
      synthetic_block(&block, array, target, position);
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
        auris_srp_apply(srp, fft, &spectrum, &output) != AURIS_OK) {
      fprintf(stderr, "DSP failed\n");
      goto done;
    }
    /* Only the diagnostic time display loses float precision over long runs. */
    const float summary[5] = {
        (float)((double)output.first_frame / (double)AURIS_ALSA_RATE),
        (float)output.best, output.peak, output.contrast,
        output.valid ? 1.0f : 0.0f};
    if (!write_floats(summary, 5U) ||
        !write_floats(output.scores, DIRECTIONS) ||
        !write_floats(output.contributions, pair_count) ||
        !write_floats(output.predicted_lags, pair_count) ||
        fflush(stdout) == EOF) {
      goto done;
    }
  }
  result = EXIT_SUCCESS;
done:
  auris_alsa_close(&state);
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return result;
}
