#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_spectrum.h"
#include "auris_wav.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(float) == 4U, "scope requires 32-bit float");

static volatile sig_atomic_t keep_running = 1;

static void stop_capture(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static bool write_floats(const float *data, size_t count) {
  return fwrite(data, sizeof(float), count, stdout) == count;
}

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--array NAME] [--pair A B]\n"
          "          [--device D | --file REC.wav [--realtime] |"
          " --synthetic [--bearing DEG]]\n"
          "          [--frame N] [--hop H] [--upsample U] [--band LO HI]\n"
          "       %s ALSA_DEVICE | --synthetic     (ReSpeaker, as before)\n"
          "  NAME: %s;  A, B: microphone numbers (MIC1 = 1)\n"
          "  --band  bins used, in Hz (default: the array's band; 0 0 = every bin)\n",
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

/* A broadband multitone reaching every microphone of `array`. With a
   bearing, each microphone hears it advanced by its projection onto the
   in-plane direction (a far source in the board's plane). Without one,
   microphone `b` hears it 3 samples late and the others unchanged. */
static void synthetic_block(auris_audio_block_t *block,
                            const auris_array_t *array, unsigned int rate,
                            bool bearing, double degrees, size_t b,
                            uint64_t position) {
  const double angle = degrees * 0.017453292519943295;
  const double ux = cos(angle), uy = sin(angle);
  block->sample_rate = rate;
  block->channels = array->microphone_count;
  block->frames = AURIS_AUDIO_FRAMES;
  block->first_frame = position;
  for (size_t m = 0U; m < block->channels; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance =
        bearing ? ((double)p.x_m * ux + (double)p.y_m * uy) /
                      (double)array->speed_of_sound_mps
                : (m == b ? -3.0 / (double)rate : 0.0);
    for (size_t n = 0U; n < block->frames; ++n) {
      const double time =
          (double)(position + (uint64_t)n) / (double)rate + advance;
      double value = 0.2;
      for (size_t k = 5U; k < 225U; k += 7U) {
        const double frequency = (double)k * 31.25;
        value += 0.006 * sin(6.283185307179586 * frequency * time +
                             (double)(k * k));
      }
      block->samples[m][n] = (float)value;
    }
  }
}

int main(int argc, char **argv) {
  const char *name = "respeaker", *device = NULL, *file = NULL;
  bool synthetic = false, realtime = false, bearing = false, legacy = false;
  bool pair_given = false;
  double degrees = 0.0, band[2] = {-1.0, -1.0}; /* -1: profile band */
  size_t pair[2] = {0U, 0U}, frame_size = 0U, frame_hop = 0U, upsample = 0U;
  if (argc == 2 && argv[1][0] != '-') {
    device = argv[1];
    legacy = true;
  } else if (argc == 2 && strcmp(argv[1], "--synthetic") == 0) {
    synthetic = true;
    legacy = true;
  } else {
    for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
        name = argv[++i];
      } else if (strcmp(argv[i], "--pair") == 0 && i + 2 < argc) {
        if (!parse_size(argv[i + 1], &pair[0]) ||
            !parse_size(argv[i + 2], &pair[1])) {
          return usage(argv[0]);
        }
        pair_given = true;
        i += 2;
      } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
        device = argv[++i];
      } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
        file = argv[++i];
      } else if (strcmp(argv[i], "--realtime") == 0) {
        realtime = true;
      } else if (strcmp(argv[i], "--synthetic") == 0) {
        synthetic = true;
      } else if (strcmp(argv[i], "--bearing") == 0 && i + 1 < argc) {
        char *end = NULL;
        errno = 0;
        degrees = strtod(argv[++i], &end);
        if (errno != 0 || end == argv[i] || *end != '\0' || !isfinite(degrees)) {
          return usage(argv[0]);
        }
        bearing = true;
      } else if (strcmp(argv[i], "--band") == 0 && i + 2 < argc) {
        for (size_t j = 0U; j < 2U; ++j) {
          char *end = NULL;
          errno = 0;
          band[j] = strtod(argv[i + 1 + (int)j], &end); /* -1 rejected */
          if (errno != 0 || end == argv[i + 1 + (int)j] || *end != '\0' ||
              !(band[j] >= 0.0)) {
            return usage(argv[0]);
          }
        }
        i += 2;
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
      (realtime && file == NULL) || (bearing && !synthetic)) {
    return usage(argv[0]);
  }
  auris_config_t config;
  if (!auris_config_lookup(name, &config)) {
    fprintf(stderr, "unknown array '%s'\n", name);
    return usage(argv[0]);
  }
  const auris_array_t *array = config.array;
  const auris_capture_profile_t *profile = config.capture;
  const unsigned int rate = profile->sample_rate;

  /* Default pairs: the ReSpeaker diagonal MIC1-MIC3 (MIC1-MIC2 for the old
     synthetic mode), and the CMB diameter MIC1-MIC5. */
  size_t mic_a = 0U, mic_b = legacy && synthetic ? 1U : 2U;
  if (array->microphone_count == 8U) {
    mic_b = 4U;
  }
  if (pair_given) {
    if (pair[0] < 1U || pair[1] < 1U || pair[0] > array->microphone_count ||
        pair[1] > array->microphone_count || pair[0] == pair[1]) {
      fprintf(stderr, "pair must be two different microphones 1..%zu\n",
              array->microphone_count);
      return EXIT_FAILURE;
    }
    mic_a = pair[0] - 1U;
    mic_b = pair[1] - 1U;
  }

  if (frame_size == 0U) frame_size = profile->frame_size;
  if (frame_hop == 0U) {
    frame_hop = frame_size == profile->frame_size ? profile->frame_hop
                                                  : frame_size / 2U;
  }
  if (upsample == 0U) upsample = profile->gcc_upsample;
  if (band[0] < 0.0) {
    band[0] = (double)profile->band_low_hz;
    band[1] = (double)profile->band_high_hz;
  }
  if (!auris_analysis_size_valid(frame_size, frame_hop)) {
    fprintf(stderr, "invalid frame %zu / hop %zu\n", frame_size, frame_hop);
    return EXIT_FAILURE;
  }
  const bool full_band = band[0] == 0.0 && band[1] == 0.0;
  if (!full_band && !(band[0] < band[1] && band[1] <= (double)rate / 2.0)) {
    fprintf(stderr, "band must satisfy LO < HI <= %u Hz\n", rate / 2U);
    return EXIT_FAILURE;
  }

  /* Physical lag limit for this pair, and the expected lag if known. */
  const auris_vec3f_t pa = array->positions[mic_a];
  const auris_vec3f_t pb = array->positions[mic_b];
  const double dx = (double)pb.x_m - (double)pa.x_m;
  const double dy = (double)pb.y_m - (double)pa.y_m;
  const double dz = (double)pb.z_m - (double)pa.z_m;
  const double scale = (double)rate / (double)array->speed_of_sound_mps;
  const float physical_limit =
      synthetic && !bearing ? 8.0f
                            : (float)(sqrt(dx * dx + dy * dy + dz * dz) * scale);
  double truth = NAN;
  if (synthetic) {
    const double angle = degrees * 0.017453292519943295;
    truth = bearing ? -scale * (dx * cos(angle) + dy * sin(angle)) : 3.0;
  }
  const int max_lag = (int)ceilf(physical_limit);
  if (max_lag >= (int)frame_size / 2) {
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

  /* Allocate/plan DSP before starting capture. */
  auris_fft_t *fft = auris_fft_create_size(frame_size, upsample);
  if (fft == NULL) {
    fprintf(stderr, "FFT setup failed (upsample must be 1, 2, 4 or 8)\n");
    return EXIT_FAILURE;
  }
  const size_t fine_size = auris_fft_curve_size(fft);
  const size_t bins = frame_size / 2U + 1U;
  float *fine = malloc(fine_size * sizeof(float));
  static auris_framer_t framer;
  static auris_preprocess_t prep;
  static auris_analysis_frame_t analysis;
  static auris_spectrum_t spectrum;
  static auris_gcc_t gcc;
  auris_audio_block_t block;
  auris_alsa_t state = {0};
  auris_wav_reader_t recording = {0};
  auris_audio_source_t source = {&state, auris_alsa_read};
  int result = EXIT_FAILURE;
  if (fine == NULL ||
      auris_framer_init(&framer, frame_size, frame_hop) != AURIS_OK ||
      auris_preprocess_init_size(&prep, frame_size) != AURIS_OK) {
    goto done;
  }
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
          "%s: MIC%zu -> MIC%zu, %.1f mm apart, |lag| <= %.2f samples; "
          "%zu-sample frames, GCC refined %zux (%.2f us steps), band %s\n",
          array->name, mic_a + 1U, mic_b + 1U,
          1000.0 * sqrt(dx * dx + dy * dy + dz * dz), (double)physical_limit,
          frame_size, upsample, 1.0e6 / ((double)rate * (double)upsample),
          full_band ? "all bins" : "limited");
  if (!full_band) {
    fprintf(stderr, "band %.0f-%.0f Hz\n", band[0], band[1]);
  }
  if (printf("AURIS3 %u %zu %zu %zu %.9g %zu %.9g\n", rate, frame_size, mic_a,
             mic_b, (double)physical_limit, upsample, truth) < 0) {
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
      synthetic_block(&block, array, rate, bearing, degrees, mic_b, position);
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
    bool fine_valid = false;
    if (auris_preprocess_apply(&prep, &analysis) != AURIS_OK ||
        auris_spectrum_apply(fft, &analysis, &spectrum) != AURIS_OK ||
        (!full_band &&
         auris_spectrum_band(&spectrum, (float)band[0], (float)band[1]) !=
             AURIS_OK) ||
        auris_gcc_phat(fft, &spectrum, mic_a, mic_b, max_lag, &gcc) !=
            AURIS_OK ||
        auris_gcc_phat_curve(fft, &spectrum, mic_a, mic_b, fine,
                             &fine_valid) != AURIS_OK) {
      fprintf(stderr, "DSP failed\n");
      goto done;
    }
    /* Best fine lag inside the same physical window. */
    const int fine_max = (int)ceilf(physical_limit * (float)upsample);
    float fine_best = -INFINITY;
    int fine_lag = 0;
    for (int lag = -fine_max; lag <= fine_max; ++lag) {
      const size_t index = (size_t)(lag < 0 ? (int)fine_size + lag : lag);
      if (fine[index] > fine_best) {
        fine_best = fine[index];
        fine_lag = lag;
      }
    }
    const float summary[4] = {(float)gcc.lag_samples, gcc.peak,
                              gcc.valid ? 1.0f : 0.0f,
                              (float)fine_lag / (float)upsample};
    if (!write_floats(summary, 4U) ||
        !write_floats(analysis.samples[mic_a], frame_size) ||
        !write_floats(analysis.samples[mic_b], frame_size)) {
      goto done;
    }
    /* Each bin is one array of two floats; transmit those arrays. */
    for (size_t side = 0U; side < 2U; ++side) {
      const size_t mic = side == 0U ? mic_a : mic_b;
      for (size_t k = 0U; k < bins; ++k) {
        if (!write_floats(spectrum.bins[mic][k], 2U)) {
          goto done;
        }
      }
    }
    if (!write_floats(gcc.correlation, frame_size) ||
        !write_floats(fine, fine_size) || fflush(stdout) == EOF) {
      goto done;
    }
  }
  result = EXIT_SUCCESS;
done:
  auris_alsa_close(&state);
  auris_wav_close(&recording);
  auris_fft_destroy(fft);
  free(fine);
  return result;
}
