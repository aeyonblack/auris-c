#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_srp.h"
#include "auris_wav.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--array NAME]\n"
          "          [--device D | --file REC.wav [--realtime] |"
          " --synthetic DEG]\n"
          "          [--frame N] [--hop H] [--upsample U] [--band LO HI]"
          " [--smooth SECONDS]\n"
          "       %s ALSA_DEVICE | --synthetic DEG    (ReSpeaker, as before)\n"
          "  NAME: %s;  DEG: 0 = right, 90 = up, viewed from behind\n"
          "  --band defaults to the array's band; --band 0 0 keeps every bin\n",
          program, program, auris_config_names());
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

/* A far broadband source in the board's plane at `degrees`: every
   microphone hears the same multitone (156 Hz - 7 kHz, 31.25 Hz grid)
   advanced by its projection onto the direction of the source. */
static void synthetic_block(auris_audio_block_t *block,
                            const auris_array_t *array, unsigned int rate,
                            double degrees, uint64_t position) {
  const double angle = degrees * 0.017453292519943295;
  block->sample_rate = rate;
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
          (double)(position + (uint64_t)n) / (double)rate + advance;
      double value = 0.2;
      for (size_t k = 5U; k < 225U; k += 7U) {
        value += 0.006 * sin(6.283185307179586 * (double)k * 31.25 * time +
                             (double)(k * k));
      }
      block->samples[m][n] = (float)value;
    }
  }
}

int main(int argc, char **argv) {
  const char *name = "respeaker", *device = NULL, *file = NULL;
  bool synthetic = false, realtime = false;
  double target = -1.0, band[2] = {-1.0, -1.0}, smooth = 0.0; /* -1: profile */
  size_t frame_size = 0U, frame_hop = 0U, upsample = 0U;
  if (argc == 2 && argv[1][0] != '-') {
    device = argv[1];
  } else if (argc == 3 && strcmp(argv[1], "--synthetic") == 0) {
    synthetic = true;
    if (!parse_real(argv[2], &target)) return usage(argv[0]);
  } else {
    for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
        name = argv[++i];
      } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
        device = argv[++i];
      } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
        file = argv[++i];
      } else if (strcmp(argv[i], "--realtime") == 0) {
        realtime = true;
      } else if (strcmp(argv[i], "--synthetic") == 0 && i + 1 < argc) {
        synthetic = true;
        if (!parse_real(argv[++i], &target)) return usage(argv[0]);
      } else if (strcmp(argv[i], "--band") == 0 && i + 2 < argc) {
        if (!parse_real(argv[i + 1], &band[0]) ||
            !parse_real(argv[i + 2], &band[1])) {
          return usage(argv[0]);
        }
        i += 2;
      } else if (strcmp(argv[i], "--smooth") == 0 && i + 1 < argc) {
        if (!parse_real(argv[++i], &smooth)) return usage(argv[0]);
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
  if (synthetic && !(target >= 0.0 && target < 360.0)) {
    fprintf(stderr, "angle must be in [0, 360)\n");
    return EXIT_FAILURE;
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
  if (band[0] < 0.0 && band[1] < 0.0) {
    band[0] = (double)profile->band_low_hz;
    band[1] = (double)profile->band_high_hz;
  }
  const bool full_band = band[0] == 0.0 && band[1] == 0.0;
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

  /* One-degree ring in the board's plane: 0 = +X (right), 90 = +Y (up). */
  auris_vec3f_t grid[DIRECTIONS];
  for (size_t q = 0U; q < DIRECTIONS; ++q) {
    const float angle = (float)q * 0.01745329251994329577f;
    grid[q] = (auris_vec3f_t){cosf(angle), sinf(angle), 0.0f};
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
  char band_text[32] = "all bins";
  if (!full_band) {
    (void)snprintf(band_text, sizeof band_text, "%.0f-%.0f Hz", band[0],
                   band[1]);
  }
  fprintf(stderr,
          "%s: %zu pairs, %zu-sample frames every %zu, GCC %zux, "
          "band %s, smoothing %.3f s\n",
          array->name, pair_count, frame_size, frame_hop, upsample,
          band_text, smooth);

  /* Stream metadata before acquisition begins. */
  if (printf("AURIS4 %u %u %zu %zu %.9g\n", rate, DIRECTIONS,
             array->microphone_count, pair_count, target) < 0) {
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
  uint64_t position = 0U;
  while (keep_running) {
    if (synthetic) {
      if (position >= (uint64_t)rate * 10U) {
        break;
      }
      synthetic_block(&block, array, rate, target, position);
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
    if (auris_preprocess_apply(&prep, &analysis) != AURIS_OK ||
        auris_spectrum_apply(fft, &analysis, &spectrum) != AURIS_OK ||
        (!full_band &&
         auris_spectrum_band(&spectrum, (float)band[0], (float)band[1]) !=
             AURIS_OK) ||
        auris_srp_apply(srp, fft, &spectrum, &output) != AURIS_OK) {
      fprintf(stderr, "DSP failed\n");
      goto done;
    }
    /* Only the diagnostic time display loses float precision over long runs. */
    const float summary[5] = {
        (float)((double)output.first_frame / (double)rate), (float)output.best,
        output.peak, output.contrast, output.valid ? 1.0f : 0.0f};
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
  auris_wav_close(&recording);
  auris_srp_destroy(srp);
  auris_fft_destroy(fft);
  return result;
}
