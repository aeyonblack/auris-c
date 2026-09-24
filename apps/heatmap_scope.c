
#include "auris_activity.h"
#include "auris_alsa.h"
#include "auris_array.h"
#include "auris_array_configs.h"
#include "auris_srp.h"
#include "auris_view.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COLUMNS 65U
#define ROWS 49U
#define DIRECTIONS (COLUMNS * ROWS)
#define SEARCH_STEP 1U
_Static_assert(DIRECTIONS <= AURIS_SRP_MAX_DIRECTIONS,
               "grid exceeds SRP capacity");

static const auris_view_t view = {
    .width = 640U,
    .height = 480U,
    .fx = 480.0f,
    .fy = 480.0f,
    .cx = 319.5f,
    .cy = 239.5f,
    .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

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
                            const auris_array_t *array, auris_vec3f_t ray,
                            uint64_t position) {

  block->sample_rate = AURIS_ALSA_RATE;
  block->channels = array->microphone_count;
  block->frames = AURIS_AUDIO_FRAMES;
  block->first_frame = position;
  for (size_t m = 0U; m < block->channels; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance =
        ((double)p.x_m * (double)ray.x_m + (double)p.y_m * (double)ray.y_m +
         (double)p.z_m * (double)ray.z_m) /
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
      const double source_time =
          (double)(position + (uint64_t)n) / (double)AURIS_ALSA_RATE;
      const bool sounding = (source_time >= 2.0 && source_time < 4.0) ||
                            (source_time >= 6.0 && source_time < 8.0);
      block->samples[m][n] = sounding ? (float)value : 0.0f;
    }
  }
}

static double elapsed_ms(struct timespec start, struct timespec end) {
  return 1000.0 * ((double)end.tv_sec - (double)start.tv_sec) +
         0.000001 * ((double)end.tv_nsec - (double)start.tv_nsec);
}

int main(int argc, char **argv) {
  const bool display = argc > 1 && strcmp(argv[1], "--display") == 0;
  if (display) {
    for (int i = 1; i + 1 < argc; ++i) {
      argv[i] = argv[i + 1];
    }
    --argc;
  }
  const bool synthetic = argc == 4 && strcmp(argv[1], "--synthetic") == 0;
  if ((!synthetic && argc != 2) ||
      (argc == 2 && strcmp(argv[1], "--synthetic") == 0)) {
    fprintf(stderr, "usage: %s [--display] ALSA_DEVICE | [--display] --synthetic COLUMN ROW\n",
            argv[0]);
    return EXIT_FAILURE;
  }
  float target[2] = {-1.0f, -1.0f};
  auris_vec3f_t truth = {0};
  if (synthetic) {
    const float limits[2] = {(float)(COLUMNS - 1U), (float)(ROWS - 1U)};
    for (size_t i = 0U; i < 2U; ++i) {
      char *end = NULL;
      errno = 0;
      target[i] = strtof(argv[i + 2U], &end);
      if (errno != 0 || end == argv[i + 2U] || *end != '\0' ||
          !isfinite(target[i]) || target[i] < 0.0f || target[i] > limits[i]) {
        fprintf(stderr, "column must be 0..%u and row 0..%u\n",
                COLUMNS - 1U, ROWS - 1U);
        return EXIT_FAILURE;
      }
    }
    const float u =
        target[0] / (float)(COLUMNS - 1U) * (float)(view.width - 1U);
    const float v = target[1] / (float)(ROWS - 1U) * (float)(view.height - 1U);
    if (auris_view_ray(&view, u, v, &truth) != AURIS_OK) {
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
  if (auris_view_grid(&view, COLUMNS, ROWS, grid, DIRECTIONS) != AURIS_OK) {
    fprintf(stderr, "view setup failed\n");
    return EXIT_FAILURE;
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
  if (auris_srp_set_grid(srp, COLUMNS, ROWS, SEARCH_STEP) != AURIS_OK) {
    fprintf(stderr, "search grid setup failed\n");
    goto done;
  }
  auris_framer_reset(&framer);
  auris_preprocess_t prep;
  auris_preprocess_init(&prep);
  auris_activity_config_t activity_config = auris_activity_defaults();
  activity_config.enabled = true;
  /* Edit configuration here, then rebuild; examples:
     activity_config.open_db = 8.0f;
     activity_config.high_hz = 5000.0f;
  */
  float window_energy = 0.0f;
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    window_energy += prep.window[n] * prep.window[n];
  }
  auris_activity_t activity = {0};
  if (auris_activity_init(&activity, &activity_config, AURIS_ALSA_RATE,
                          array->microphone_count, window_energy) != AURIS_OK ||
      auris_srp_set_smoothing(srp, 0.100f) != AURIS_OK) {
    fprintf(stderr, "activity setup failed\n");
    goto done;
  }
  uint64_t next_report = 0U;
  auris_analysis_frame_t analysis;
  auris_spectrum_t spectrum;
  auris_srp_result_t output = {0};
  auris_audio_block_t block;
  auris_audio_source_t source = {&state, auris_alsa_read};
  const size_t pair_count = auris_array_pair_count(array->microphone_count);

  /* Stream metadata before acquisition begins. */
  if (printf("%s %u %u %u %zu %zu %.9g %.9g", display ? "AURIS6" : "AURIS5", AURIS_ALSA_RATE, COLUMNS,
             ROWS, array->microphone_count, pair_count, (double)target[0],
             (double)target[1]) < 0) {
    goto done;
  }
  if ((display && printf(" %zu %zu", view.width, view.height) < 0) ||
      putchar('\n') == EOF) {
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
  double srp_total_ms = 0.0, srp_max_ms = 0.0;
  size_t srp_calls = 0U;
  while (keep_running) {
    if (synthetic) {
      if (position >= (uint64_t)AURIS_ALSA_RATE * 10U) {
        break;
      }
      synthetic_block(&block, array, truth, position);
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
    auris_activity_result_t gate;
    if (auris_preprocess_apply(&prep, &analysis) != AURIS_OK ||
        auris_spectrum_apply(fft, &analysis, &spectrum) != AURIS_OK ||
        auris_activity_apply(&activity, &spectrum, &gate) != AURIS_OK) {
      fprintf(stderr, "preprocessing/activity failed\n");
      goto done;
    }
    if (gate.active && gate.evidence) {
      struct timespec start, end;
      if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        goto done;
      }
      if (auris_srp_apply(srp, fft, &spectrum, &output) != AURIS_OK) {
        fprintf(stderr, "SRP failed\n");
        goto done;
      }
      if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        goto done;
      }
      const double ms = elapsed_ms(start, end);
      srp_total_ms += ms;
      if (ms > srp_max_ms) {
        srp_max_ms = ms;
      }
      ++srp_calls;
    }
    if (!gate.active || !output.valid) {
      output = (auris_srp_result_t){0};
      auris_srp_reset(srp);
    }
    /* During the short hold, output is the last valid observation. */
    output.first_frame = analysis.first_frame;
    output.directions = DIRECTIONS;
    output.pairs = pair_count;
    if (analysis.first_frame >= next_report) {
      fprintf(stderr, "level %.1f noise %.1f open %.1f close %.1f dBFS | %s\n",
              (double)gate.level_dbfs, (double)gate.noise_dbfs,
              (double)gate.open_dbfs, (double)gate.close_dbfs,
              !gate.ready     ? "calibrating"
              : !gate.active  ? "closed"
              : gate.evidence ? "active"
                              : "holding");
      fprintf(stderr, "grid %ux%u step %u | evaluated %zu/%zu | %s\n",
              COLUMNS, ROWS, SEARCH_STEP, output.evaluated, output.directions,
              output.approximate ? "approximate map" : "exhaustive map");
      if (srp_calls > 0U) {
        fprintf(stderr, "SRP mean %.3f ms, max %.3f ms over %zu calls\n",
                srp_total_ms / (double)srp_calls, srp_max_ms, srp_calls);
      }
      srp_total_ms = 0.0;
      srp_max_ms = 0.0;
      srp_calls = 0U;
      next_report = analysis.first_frame + (uint64_t)AURIS_ALSA_RATE;
    }

    /* Only the diagnostic time display loses float precision over long runs. */
    const float summary[5] = {
        (float)((double)output.first_frame / (double)AURIS_ALSA_RATE),
        (float)output.best, output.peak, output.contrast,
        output.valid ? 1.0f : 0.0f};
    const float telemetry[5] = {
        gate.level_dbfs, gate.ready ? 1.0f : 0.0f,
        gate.active ? 1.0f : 0.0f, gate.evidence ? 1.0f : 0.0f,
        activity_config.enabled ? 1.0f : 0.0f};
    if (!write_floats(summary, 5U) ||
        (display && !write_floats(telemetry, 5U)) ||
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
