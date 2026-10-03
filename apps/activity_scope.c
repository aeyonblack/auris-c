#include "auris_activity.h"
#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_wav.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Octave bands centred on 125 Hz x 2^i, kept while the upper edge
   (centre x sqrt 2) is below Nyquist and at least one bin lies inside. */
#define MAX_BANDS 10U

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
          "usage: %s [--array NAME] [--device D | --file REC.wav [--realtime]]\n"
          "          [--frame N] [--hop H] [--band LO HI]\n"
          "          [--open DB] [--close DB] [--minimum DBFS]"
          " [--attack S] [--hold S]\n"
          "  NAME: %s;  --band and --minimum default to the array's values\n",
          program, auris_config_names());
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

static bool parse_real(const char *text, float *out) {
  char *end = NULL;
  errno = 0;
  const double value = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !isfinite(value)) {
    return false;
  }
  *out = (float)value;
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

static float to_db(float power) {
  return 10.0f * log10f(fmaxf(power, 1.0e-12f));
}

int main(int argc, char **argv) {
  const char *name = "respeaker", *device = NULL, *file = NULL;
  bool realtime = false, band_given = false;
  size_t frame_size = 0U, frame_hop = 0U;
  bool minimum_given = false;
  float band[2] = {0.0f, 0.0f};
  auris_activity_config_t gate_config = auris_activity_defaults();
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
      device = argv[++i];
    } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
      file = argv[++i];
    } else if (strcmp(argv[i], "--realtime") == 0) {
      realtime = true;
    } else if (strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
      if (!parse_size(argv[++i], &frame_size)) return usage(argv[0]);
    } else if (strcmp(argv[i], "--hop") == 0 && i + 1 < argc) {
      if (!parse_size(argv[++i], &frame_hop)) return usage(argv[0]);
    } else if (strcmp(argv[i], "--band") == 0 && i + 2 < argc) {
      if (!parse_real(argv[i + 1], &band[0]) ||
          !parse_real(argv[i + 2], &band[1])) {
        return usage(argv[0]);
      }
      band_given = true;
      i += 2;
    } else if (strcmp(argv[i], "--open") == 0 && i + 1 < argc) {
      if (!parse_real(argv[++i], &gate_config.open_db)) return usage(argv[0]);
    } else if (strcmp(argv[i], "--close") == 0 && i + 1 < argc) {
      if (!parse_real(argv[++i], &gate_config.close_db)) return usage(argv[0]);
    } else if (strcmp(argv[i], "--minimum") == 0 && i + 1 < argc) {
      if (!parse_real(argv[++i], &gate_config.minimum_dbfs)) {
        return usage(argv[0]);
      }
      minimum_given = true;
    } else if (strcmp(argv[i], "--attack") == 0 && i + 1 < argc) {
      if (!parse_real(argv[++i], &gate_config.attack_s)) return usage(argv[0]);
    } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
      if (!parse_real(argv[++i], &gate_config.hold_s)) return usage(argv[0]);
    } else {
      return usage(argv[0]);
    }
  }
  /* At most one source; with none, capture live from the profile device. */
  if ((device != NULL && file != NULL) || (realtime && file == NULL)) {
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
  const size_t channels = array->microphone_count;
  if (frame_size == 0U) frame_size = profile->frame_size;
  if (frame_hop == 0U) {
    frame_hop = frame_size == profile->frame_size ? profile->frame_hop
                                                  : frame_size / 2U;
  }
  if (band_given) {
    gate_config.low_hz = band[0];
    gate_config.high_hz = band[1];
  } else {
    gate_config.low_hz = profile->snr_weighting ? profile->weighted_low_hz
                                                : profile->band_low_hz;
    gate_config.high_hz = profile->band_high_hz;
  }
  if (!minimum_given) {
    gate_config.minimum_dbfs = profile->gate_minimum_dbfs;
  }
  gate_config.weighting = profile->snr_weighting;

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

  auris_fft_t *fft = auris_fft_create_size(frame_size, 1U);
  static auris_framer_t framer;
  static auris_preprocess_t prep;
  static auris_analysis_frame_t analysis;
  static auris_spectrum_t spectrum;
  static auris_activity_t gate_state;
  auris_audio_block_t block;
  auris_alsa_t state = {0};
  auris_wav_reader_t recording = {0};
  auris_audio_source_t source = {&state, auris_alsa_read};
  int result = EXIT_FAILURE;
  if (fft == NULL || auris_framer_init(&framer, frame_size, frame_hop) != AURIS_OK ||
      auris_preprocess_init_size(&prep, frame_size) != AURIS_OK) {
    fprintf(stderr, "DSP setup failed (check frame and hop)\n");
    goto done;
  }
  float window_energy = 0.0f;
  for (size_t n = 0U; n < frame_size; ++n) {
    window_energy += prep.window[n] * prep.window[n];
  }
  if (auris_activity_init_sized(&gate_state, &gate_config, rate, channels,
                                frame_size, frame_hop,
                                window_energy) != AURIS_OK) {
    fprintf(stderr, "gate setup failed: need 0 <= LO < HI <= %u Hz with a bin"
                    " inside, CLOSE < OPEN, durations 0-60 s\n",
            rate / 2U);
    goto done;
  }

  /* Octave bands that fit this rate and frame. */
  float centres[MAX_BANDS];
  size_t band_count = 0U;
  const float bin_hz = (float)rate / (float)frame_size;
  for (float centre = 125.0f;
       band_count < MAX_BANDS && centre * 1.41421356f <= (float)rate / 2.0f;
       centre *= 2.0f) {
    if (ceilf(centre / 1.41421356f / bin_hz) <=
        floorf(centre * 1.41421356f / bin_hz)) {
      centres[band_count++] = centre;
    }
  }

  if (file != NULL) {
    const int err =
        auris_wav_open(&recording, file, profile, array->capture_channels,
                       channels);
    if (err < 0) {
      fprintf(stderr, "%s: %s\n", file,
              err == -EINVAL ? "recording does not match this array's device"
                             : strerror(-err));
      goto done;
    }
    source = (auris_audio_source_t){&recording, auris_wav_read};
  } else {
    const int err = auris_alsa_open_profile(&state, profile, device,
                                            array->capture_channels, channels);
    if (err < 0) {
      fprintf(stderr, "open: %s\n", snd_strerror(err));
      goto done;
    }
  }
  fprintf(stderr,
          "%s: %zu-sample frames every %zu (%.1f ms), gate band %.0f-%.0f Hz "
          "(bins %zu-%zu), open +%.1f dB, close +%.1f dB, minimum %.1f dBFS, "
          "attack %zu hops, hold %zu hops, calibration %zu hops\n",
          array->name, frame_size, frame_hop,
          1000.0 * (double)frame_hop / (double)rate,
          (double)gate_state.first_bin * (double)bin_hz,
          (double)gate_state.last_bin * (double)bin_hz, gate_state.first_bin,
          gate_state.last_bin, (double)gate_config.open_db,
          (double)gate_config.close_db, (double)gate_config.minimum_dbfs,
          gate_state.attack_frames, gate_state.hold_frames,
          gate_state.warm_frames);
  fprintf(stderr, "keep quiet for the first second: the gate is calibrating\n");

  /* Header: rate, frame, hop, microphones, gate band, octave centres. */
  if (printf("AURIS7 %u %zu %zu %zu %.9g %.9g %zu", rate, frame_size,
             frame_hop, channels, (double)gate_config.low_hz,
             (double)gate_config.high_hz, band_count) < 0) {
    goto done;
  }
  for (size_t b = 0U; b < band_count; ++b) {
    if (printf(" %.9g", (double)centres[b]) < 0) goto done;
  }
  if (putchar('\n') == EOF || fflush(stdout) == EOF) {
    goto done;
  }

  struct timespec start;
  (void)clock_gettime(CLOCK_MONOTONIC, &start);
  uint64_t next_report = 0U;
  while (keep_running) {
    if (source.read(source.context, &block) != AURIS_OK) {
      if (!keep_running) {
        break;
      }
      fprintf(stderr, "capture failed\n");
      goto done;
    }
    if (block.frames == 0U) {
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
        auris_spectrum_apply(fft, &analysis, &spectrum) != AURIS_OK) {
      fprintf(stderr, "preprocessing failed\n");
      goto done;
    }
    /* Per-microphone levels come first: the gate zeroes out-of-band bins. */
    float octave_db[AURIS_AUDIO_CHANNELS][MAX_BANDS];
    float band_db[AURIS_AUDIO_CHANNELS];
    for (size_t m = 0U; m < channels; ++m) {
      for (size_t b = 0U; b < band_count; ++b) {
        float power = 0.0f;
        if (auris_spectrum_band_power(&spectrum, m, centres[b] / 1.41421356f,
                                      centres[b] * 1.41421356f, window_energy,
                                      &power) != AURIS_OK) {
          goto done;
        }
        octave_db[m][b] = to_db(power);
      }
      float power = 0.0f;
      if (auris_spectrum_band_power(&spectrum, m, gate_config.low_hz,
                                    gate_config.high_hz, window_energy,
                                    &power) != AURIS_OK) {
        goto done;
      }
      band_db[m] = to_db(power);
    }
    auris_activity_result_t gate;
    if (auris_activity_apply(&gate_state, &spectrum, &gate) != AURIS_OK) {
      fprintf(stderr, "activity failed\n");
      goto done;
    }
    if (analysis.first_frame >= next_report) {
      fprintf(stderr,
              "level %.1f noise %.1f open %.1f close %.1f dBFS, %.1f bins | "
              "%-11s|",
              (double)gate.level_dbfs, (double)gate.noise_dbfs,
              (double)gate.open_dbfs, (double)gate.close_dbfs,
              (double)gate.weight_sum,
              !gate.ready     ? "calibrating"
              : !gate.active  ? "closed"
              : gate.evidence ? "active"
                              : "holding");
      for (size_t m = 0U; m < channels; ++m) {
        fprintf(stderr, " MIC%zu %.1f", m + 1U, (double)band_db[m]);
      }
      fputc('\n', stderr);
      next_report = analysis.first_frame + (uint64_t)rate;
    }

    /* Record: gate state, then MIC-major octave levels, then band levels. */
    const float summary[8] = {
        (float)((double)analysis.first_frame / (double)rate),
        gate.level_dbfs,
        gate.noise_dbfs,
        gate.open_dbfs,
        gate.close_dbfs,
        gate.ready ? 1.0f : 0.0f,
        gate.active ? 1.0f : 0.0f,
        gate.evidence ? 1.0f : 0.0f};
    if (!write_floats(summary, 8U)) {
      goto done;
    }
    for (size_t m = 0U; m < channels; ++m) {
      if (!write_floats(octave_db[m], band_count)) goto done;
    }
    if (!write_floats(band_db, channels) || fflush(stdout) == EOF) {
      goto done;
    }
  }
  result = EXIT_SUCCESS;
done:
  auris_alsa_close(&state);
  auris_wav_close(&recording);
  auris_fft_destroy(fft);
  return result;
}
