#include "auris_activity.h"
#include "auris_alsa.h"
#include "auris_array.h"
#include "auris_array_configs.h"
#include "auris_srp.h"
#include "auris_view.h"
#include "auris_wav.h"

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

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--display] [--array NAME]\n"
          "          [--device D | --file REC.wav [--realtime] |"
          " --synthetic COLUMN ROW]\n"
          "          [--frame N] [--hop H] [--upsample U] [--band LO HI]\n"
          "          [--smooth SECONDS] [--step 1|2|4] [--no-gate]\n"
          "       %s [--display] ALSA_DEVICE | [--display] --synthetic COLUMN ROW"
          "   (ReSpeaker)\n"
          "  NAME: %s;  COLUMN 0..%u (left to right), ROW 0..%u (top to bottom)\n"
          "  --band defaults to the array's band; --band 0 0 keeps every bin\n",
          program, program, auris_config_names(), COLUMNS - 1U, ROWS - 1U);
  return EXIT_FAILURE;
}

static bool parse_size(const char *text, size_t *out) {
  char *end = NULL;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || text[0] == '-') {
    return false;
  }
  *out = (size_t)value;
  return true;
}

static bool parse_real(const char *text, double *out) {
  char *end = NULL;
  errno = 0;
  *out = strtod(text, &end);
  return errno == 0 && end != text && *end == '\0' && isfinite(*out);
}

/* Sleeps until `frames` audio frames after `start` have elapsed. */
static void pace(const struct timespec *start, uint64_t frames,
                 unsigned int rate) {
  const uint64_t ns = frames * 1000000000ULL / rate;
  struct timespec due = {
      .tv_sec = start->tv_sec + (time_t)(ns / 1000000000ULL),
      .tv_nsec = start->tv_nsec + (long)(ns % 1000000000ULL)};
  if (due.tv_nsec >= 1000000000L) {
    due.tv_nsec -= 1000000000L;
    ++due.tv_sec;
  }
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL) ==
         EINTR) {
  }
}

/* A far broadband source along `ray`, sounding from 2-4 s and 6-8 s so the
   activity gate sees onsets and silence. Tones 156 Hz - 7 kHz on a fixed
   31.25 Hz grid at every sample rate. The on/off switching happens at the
   SOURCE: each microphone hears it at its own arrival time, as with a real
   sound. Switching every channel at the same instant would create a
   zero-delay click that points straight ahead. */
static void synthetic_block(auris_audio_block_t *block,
                            const auris_array_t *array, unsigned int rate,
                            auris_vec3f_t ray, uint64_t position) {
  block->sample_rate = rate;
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
      /* Time at the source when this sample's sound left it. */
      const double source_time =
          (double)(position + (uint64_t)n) / (double)rate + advance;
      const bool sounding = (source_time >= 2.0 && source_time < 4.0) ||
                            (source_time >= 6.0 && source_time < 8.0);
      double value = 0.0;
      for (size_t k = 5U; sounding && k < 225U; k += 7U) {
        value += 0.006 * sin(6.283185307179586 * (double)k * 31.25 *
                                 source_time +
                             (double)(k * k));
      }
      block->samples[m][n] = (float)value;
    }
  }
}

static double elapsed_ms(struct timespec start, struct timespec end) {
  return 1000.0 * ((double)end.tv_sec - (double)start.tv_sec) +
         0.000001 * ((double)end.tv_nsec - (double)start.tv_nsec);
}

int main(int argc, char **argv) {
  const char *name = "respeaker", *device = NULL, *file = NULL;
  bool display = false, synthetic = false, realtime = false;
  bool band_given = false, gated = true;
  double target[2] = {-1.0, -1.0}, band[2] = {0.0, 0.0}, smooth = 0.1;
  size_t frame_size = 0U, frame_hop = 0U, upsample = 0U, step = 1U;
  int first = 1;
  if (argc > 1 && strcmp(argv[1], "--display") == 0) {
    display = true;
    first = 2;
  }
  if (argc - first == 1 && argv[first][0] != '-') {
    device = argv[first];
  } else {
    for (int i = first; i < argc; ++i) {
      if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
        name = argv[++i];
      } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
        device = argv[++i];
      } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
        file = argv[++i];
      } else if (strcmp(argv[i], "--realtime") == 0) {
        realtime = true;
      } else if (strcmp(argv[i], "--synthetic") == 0 && i + 2 < argc) {
        synthetic = true;
        if (!parse_real(argv[i + 1], &target[0]) ||
            !parse_real(argv[i + 2], &target[1])) {
          return usage(argv[0]);
        }
        i += 2;
      } else if (strcmp(argv[i], "--band") == 0 && i + 2 < argc) {
        if (!parse_real(argv[i + 1], &band[0]) ||
            !parse_real(argv[i + 2], &band[1])) {
          return usage(argv[0]);
        }
        band_given = true;
        i += 2;
      } else if (strcmp(argv[i], "--no-gate") == 0) {
        gated = false;
      } else if (strcmp(argv[i], "--smooth") == 0 && i + 1 < argc) {
        if (!parse_real(argv[++i], &smooth)) return usage(argv[0]);
      } else if (strcmp(argv[i], "--step") == 0 && i + 1 < argc) {
        if (!parse_size(argv[++i], &step)) return usage(argv[0]);
      } else if (strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
        if (!parse_size(argv[++i], &frame_size)) return usage(argv[0]);
      } else if (strcmp(argv[i], "--hop") == 0 && i + 1 < argc) {
        if (!parse_size(argv[++i], &frame_hop)) return usage(argv[0]);
      } else if (strcmp(argv[i], "--upsample") == 0 && i + 1 < argc) {
        if (!parse_size(argv[++i], &upsample)) return usage(argv[0]);
      } else {
        return usage(argv[0]);
      }
    }
  }
  /* At most one source; with none, capture live from the profile device. */
  if ((device != NULL) + (file != NULL) + synthetic > 1 ||
      (realtime && file == NULL)) {
    return usage(argv[0]);
  }
  auris_vec3f_t truth = {0};
  if (synthetic) {
    if (!(target[0] >= 0.0 && target[0] <= (double)(COLUMNS - 1U) &&
          target[1] >= 0.0 && target[1] <= (double)(ROWS - 1U))) {
      fprintf(stderr, "column must be 0..%u and row 0..%u\n", COLUMNS - 1U,
              ROWS - 1U);
      return EXIT_FAILURE;
    }
    const float u = (float)target[0] / (float)(COLUMNS - 1U) *
                    (float)(view.width - 1U);
    const float v =
        (float)target[1] / (float)(ROWS - 1U) * (float)(view.height - 1U);
    if (auris_view_ray(&view, u, v, &truth) != AURIS_OK) {
      return EXIT_FAILURE;
    }
  }
  auris_config_t config;
  if (!auris_config_lookup(name, &config)) {
    fprintf(stderr, "unknown array '%s'\n", name);
    return usage(argv[0]);
  }
  const auris_array_t *array = config.array;
  const auris_capture_profile_t *profile = config.capture;
  const unsigned int rate = profile->sample_rate;
  if (frame_size == 0U) frame_size = profile->frame_size;
  if (frame_hop == 0U) {
    frame_hop = frame_size == profile->frame_size ? profile->frame_hop
                                                  : frame_size / 2U;
  }
  if (upsample == 0U) upsample = profile->gcc_upsample;
  if (!band_given) {
    band[0] = (double)(profile->snr_weighting ? profile->weighted_low_hz
                                              : profile->band_low_hz);
    band[1] = (double)(profile->snr_weighting ? profile->weighted_high_hz
                                              : profile->band_high_hz);
  }
  const bool full_band = band[0] == 0.0 && band[1] == 0.0;
  if (full_band) {
    band[1] = (double)rate / 2.0;
  }
  if (!full_band && !(band[0] >= 0.0 && band[0] < band[1] &&
                      band[1] <= (double)rate / 2.0)) {
    fprintf(stderr, "band must satisfy 0 <= LO < HI <= %u Hz\n", rate / 2U);
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

  static auris_vec3f_t grid[DIRECTIONS];
  if (auris_view_grid(&view, COLUMNS, ROWS, grid, DIRECTIONS) != AURIS_OK) {
    fprintf(stderr, "view setup failed\n");
    return EXIT_FAILURE;
  }
  auris_fft_t *fft = auris_fft_create_size(frame_size, upsample);
  auris_srp_t *srp = auris_srp_create_sized(array, rate, frame_size, frame_hop,
                                            upsample, grid, DIRECTIONS);
  static auris_framer_t framer;
  static auris_preprocess_t prep;
  static auris_analysis_frame_t analysis;
  static auris_spectrum_t spectrum;
  static auris_srp_result_t output;
  auris_audio_block_t block;
  auris_alsa_t state = {0};
  auris_wav_reader_t recording = {0};
  auris_audio_source_t source = {&state, auris_alsa_read};
  int result = EXIT_FAILURE;
  if (fft == NULL || srp == NULL ||
      auris_framer_init(&framer, frame_size, frame_hop) != AURIS_OK ||
      auris_preprocess_init_size(&prep, frame_size) != AURIS_OK ||
      auris_srp_set_smoothing(srp, (float)smooth) != AURIS_OK) {
    fprintf(stderr, "DSP setup failed (check frame, hop, upsample, smooth)\n");
    goto done;
  }
  if (auris_srp_set_grid(srp, COLUMNS, ROWS, step) != AURIS_OK) {
    fprintf(stderr, "search step must divide %u and %u: use 1, 2 or 4\n",
            COLUMNS - 1U, ROWS - 1U);
    goto done;
  }

  /* The gate measures the same band that localizes, and zeroes every bin
     outside it. Disabled, it still applies the band and reports the level. */
  auris_activity_config_t activity_config = auris_activity_defaults_for(profile);
  activity_config.low_hz = (float)band[0];
  activity_config.high_hz = (float)band[1];
  activity_config.enabled = gated;
  static auris_activity_t activity;
  float window_energy = 0.0f;
  for (size_t n = 0U; n < frame_size; ++n) {
    window_energy += prep.window[n] * prep.window[n];
  }
  if (auris_activity_init_sized(&activity, &activity_config, rate,
                                array->microphone_count, frame_size, frame_hop,
                                window_energy) != AURIS_OK) {
    fprintf(stderr, "activity setup failed (is any bin inside the band?)\n");
    goto done;
  }
  const size_t pair_count = auris_array_pair_count(array->microphone_count);
  if (file != NULL) {
    const int err = auris_wav_open(&recording, file, profile,
                                   array->capture_channels,
                                   array->microphone_count);
    if (err < 0) {
      fprintf(stderr, "%s: %s\n", file,
              err == -EINVAL ? "recording does not match this array's device"
                             : strerror(-err));
      goto done;
    }
    source = (auris_audio_source_t){&recording, auris_wav_read};
  } else if (!synthetic) {
    const int err = auris_alsa_open_profile(&state, profile, device,
                                            array->capture_channels,
                                            array->microphone_count);
    if (err < 0) {
      fprintf(stderr, "open: %s\n", snd_strerror(err));
      goto done;
    }
  }
  fprintf(stderr,
          "%s: %zu pairs, %zu-sample frames every %zu, GCC %zux, band "
          "%.0f-%.0f Hz%s, smoothing %.3f s, grid %ux%u step %zu, gate %s\n",
          array->name, pair_count, frame_size, frame_hop, upsample, band[0],
          band[1], full_band ? " (all bins)" : "", smooth, COLUMNS, ROWS, step,
          gated ? "on" : "off");

  /* Stream metadata before acquisition begins. */
  if (printf("%s %u %u %u %zu %zu %.9g %.9g", display ? "AURIS6" : "AURIS5",
             rate, COLUMNS, ROWS, array->microphone_count, pair_count,
             target[0], target[1]) < 0) {
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
  struct timespec start;
  (void)clock_gettime(CLOCK_MONOTONIC, &start);
  uint64_t position = 0U, next_report = 0U;
  double srp_total_ms = 0.0, srp_max_ms = 0.0;
  size_t srp_calls = 0U;
  while (keep_running) {
    if (synthetic) {
      if (position >= (uint64_t)rate * 10U) {
        break;
      }
      synthetic_block(&block, array, rate, truth, position);
      position += (uint64_t)block.frames;
    } else if (source.read(source.context, &block) != AURIS_OK) {
      if (!keep_running) {
        break;
      }
      fprintf(stderr, "capture failed\n");
      goto done;
    } else if (block.frames == 0U) {
      break; /* end of a recording */
    }
    if (realtime) {
      pace(&start, block.first_frame + block.frames, block.sample_rate);
    }
    bool ready = false;
    if (auris_framer_push(&framer, &block, &analysis, &ready) != AURIS_OK) {
      fprintf(stderr, "invalid or discontinuous audio\n");
      goto done;
    }
    if (!ready) {
      continue;
    }
    /* With the gate off, every frame counts as active evidence. */
    auris_activity_result_t gate;
    if (auris_preprocess_apply(&prep, &analysis) != AURIS_OK ||
        auris_spectrum_apply(fft, &analysis, &spectrum) != AURIS_OK ||
        auris_activity_apply(&activity, &spectrum, &gate) != AURIS_OK) {
      fprintf(stderr, "preprocessing/activity failed\n");
      goto done;
    }
    if (gate.active && gate.evidence) {
      struct timespec t0, t1;
      if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        goto done;
      }
      if (auris_srp_apply(srp, fft, &spectrum, &output) != AURIS_OK) {
        fprintf(stderr, "SRP failed\n");
        goto done;
      }
      if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
        goto done;
      }
      const double ms = elapsed_ms(t0, t1);
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
      if (gated) {
        fprintf(stderr, "level %.1f noise %.1f open %.1f close %.1f dBFS | %s\n",
                (double)gate.level_dbfs, (double)gate.noise_dbfs,
                (double)gate.open_dbfs, (double)gate.close_dbfs,
                !gate.ready     ? "calibrating"
                : !gate.active  ? "closed"
                : gate.evidence ? "active"
                                : "holding");
      } else {
        fprintf(stderr, "level %.1f dBFS | gate off\n",
                (double)gate.level_dbfs);
      }
      fprintf(stderr, "grid %ux%u step %zu | evaluated %zu/%zu | %s\n",
              COLUMNS, ROWS, step, output.evaluated, output.directions,
              output.approximate ? "approximate map" : "exhaustive map");
      if (srp_calls > 0U) {
        fprintf(stderr, "SRP mean %.3f ms, max %.3f ms over %zu calls\n",
                srp_total_ms / (double)srp_calls, srp_max_ms, srp_calls);
      }
      srp_total_ms = 0.0;
      srp_max_ms = 0.0;
      srp_calls = 0U;
      next_report = analysis.first_frame + (uint64_t)rate;
    }

    /* Only the diagnostic time display loses float precision over long runs. */
    const float summary[5] = {
        (float)((double)output.first_frame / (double)rate), (float)output.best,
        output.peak, output.contrast, output.valid ? 1.0f : 0.0f};
    const float telemetry[5] = {
        gate.level_dbfs, gate.ready ? 1.0f : 0.0f, gate.active ? 1.0f : 0.0f,
        gate.evidence ? 1.0f : 0.0f, activity_config.enabled ? 1.0f : 0.0f};
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
  auris_wav_close(&recording);
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return result;
}
