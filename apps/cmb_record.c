#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_wav.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t keep_running = 1;

static void request_stop(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--array NAME] [--device ALSA_DEVICE] [--seconds S] "
          "OUT.wav\n  NAME: %s (default cmb)\n"
          "  Records raw device frames until S seconds or Ctrl+C.\n",
          program, auris_config_names());
  return EXIT_FAILURE;
}

int main(int argc, char **argv) {
  const char *name = "cmb", *device = NULL, *path = NULL;
  double seconds = 0.0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
      device = argv[++i];
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      char *end = NULL;
      errno = 0;
      seconds = strtod(argv[++i], &end);
      if (errno != 0 || end == argv[i] || *end != '\0' || !(seconds > 0.0)) {
        return usage(argv[0]);
      }
    } else if (argv[i][0] != '-' && path == NULL) {
      path = argv[i];
    } else {
      return usage(argv[0]);
    }
  }
  auris_config_t config;
  if (path == NULL || !auris_config_lookup(name, &config)) {
    return usage(argv[0]);
  }
  const auris_capture_profile_t *profile = config.capture;
  const auris_array_t *array = config.array;

  struct sigaction action = {0};
  action.sa_handler = request_stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) != 0 ||
      sigaction(SIGTERM, &action, NULL) != 0) {
    return EXIT_FAILURE;
  }

  auris_alsa_t capture = {0};
  int err = auris_alsa_open_profile(&capture, profile, device,
                                    array->capture_channels,
                                    array->microphone_count);
  if (err < 0) {
    fprintf(stderr, "open %s: %s\n", device != NULL ? device : profile->device,
            snd_strerror(err));
    return EXIT_FAILURE;
  }
  auris_wav_writer_t writer = {0};
  err = auris_wav_writer_open(&writer, path, profile);
  if (err < 0) {
    fprintf(stderr, "%s: %s\n", path, strerror(-err));
    auris_alsa_close(&capture);
    return EXIT_FAILURE;
  }

  const uint64_t limit =
      seconds > 0.0 ? (uint64_t)(seconds * (double)profile->sample_rate) : 0U;
  fprintf(stderr, "recording %s: %zu device channels, %u Hz, %s -> %s\n",
          array->name, profile->raw_channels, profile->sample_rate,
          profile->format == AURIS_SAMPLE_S32_LE ? "S32_LE" : "S16_LE", path);

  int result = EXIT_SUCCESS;
  auris_audio_block_t block;
  double energy[AURIS_AUDIO_CHANNELS] = {0.0};
  uint64_t frames = 0U, report_at = profile->sample_rate, since_report = 0U;
  while (keep_running && (limit == 0U || frames < limit)) {
    if (auris_alsa_read(&capture, &block) != AURIS_OK) {
      if (keep_running) {
        result = EXIT_FAILURE;
      }
      break;
    }
    size_t bytes = 0U;
    const unsigned char *raw = auris_alsa_raw(&capture, &bytes);
    size_t count = block.frames;
    if (limit != 0U && frames + count > limit) {
      count = (size_t)(limit - frames);
    }
    err = auris_wav_writer_write(&writer, raw, count);
    if (err < 0) {
      fprintf(stderr, "write: %s\n", strerror(-err));
      result = EXIT_FAILURE;
      break;
    }
    for (size_t m = 0U; m < block.channels; ++m) {
      for (size_t n = 0U; n < count; ++n) {
        energy[m] += (double)block.samples[m][n] * (double)block.samples[m][n];
      }
    }
    frames += count;
    since_report += count;
    if (frames >= report_at) {
      fprintf(stderr, "%6.1f s  rms dBFS:", (double)frames /
                                               (double)profile->sample_rate);
      for (size_t m = 0U; m < block.channels; ++m) {
        const double rms = sqrt(energy[m] / (double)since_report);
        fprintf(stderr, " MIC%zu %5.1f", m + 1U, 20.0 * log10(rms + 1e-12));
        energy[m] = 0.0;
      }
      fputc('\n', stderr);
      since_report = 0U;
      report_at += profile->sample_rate;
    }
  }
  auris_alsa_close(&capture);
  err = auris_wav_writer_close(&writer);
  if (err < 0) {
    fprintf(stderr, "finishing %s: %s\n", path, strerror(-err));
    result = EXIT_FAILURE;
  }
  fprintf(stderr, "wrote %llu frames (%.2f s) to %s\n",
          (unsigned long long)frames,
          (double)frames / (double)profile->sample_rate, path);
  return result;
}
