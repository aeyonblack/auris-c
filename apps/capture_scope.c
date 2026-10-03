#include "auris_alsa.h"
#include "auris_analysis.h"
#include "auris_array_configs.h"
#include "auris_wav.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(float) == 4U, "scope requires 32-bit floats");

static volatile sig_atomic_t keep_running = 1;

static void request_stop(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--array NAME] [--device ALSA_DEVICE | --file REC.wav "
          "[--realtime]] [--raw]\n"
          "       %s ALSA_DEVICE            (ReSpeaker, as before)\n"
          "  NAME: %s\n"
          "  --raw       stream capture blocks (AURIS1) instead of analysis "
          "frames (AURIS2)\n"
          "  --file      replay a recording made with cmb_record\n"
          "  --realtime  with --file: deliver blocks at the recording's rate\n"
          "  --frame N --hop H  analysis frame and hop in samples (default: "
          "the array's profile)\n",
          program, program, auris_config_names());
  return EXIT_FAILURE;
}

static bool write_block(const auris_audio_block_t *block) {
  for (size_t c = 0U; c < block->channels; ++c) {
    if (fwrite(block->samples[c], sizeof(float), block->frames, stdout) !=
        block->frames) {
      return false;
    }
  }
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

int main(int argc, char **argv) {
  const char *name = "respeaker";
  const char *device = NULL, *file = NULL;
  bool raw_blocks = false, realtime = false;
  size_t frame_size = 0U, frame_hop = 0U;
  if (argc == 2 && argv[1][0] != '-') {
    device = argv[1];
  } else {
    for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
        name = argv[++i];
      } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
        device = argv[++i];
      } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
        file = argv[++i];
      } else if ((strcmp(argv[i], "--frame") == 0 ||
                  strcmp(argv[i], "--hop") == 0) &&
                 i + 1 < argc) {
        char *end = NULL;
        errno = 0;
        const unsigned long value = strtoul(argv[i + 1], &end, 10);
        if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
            argv[i + 1][0] == '-') {
          return usage(argv[0]);
        }
        if (argv[i][2] == 'f') {
          frame_size = (size_t)value;
        } else {
          frame_hop = (size_t)value;
        }
        ++i;
      } else if (strcmp(argv[i], "--realtime") == 0) {
        realtime = true;
      } else if (strcmp(argv[i], "--raw") == 0) {
        raw_blocks = true;
      } else {
        return usage(argv[0]);
      }
    }
  }
  auris_config_t config;
  if ((file != NULL && device != NULL) || (realtime && file == NULL)) {
    return usage(argv[0]);
  }
  if (!auris_config_lookup(name, &config)) {
    fprintf(stderr, "unknown array '%s'\n", name);
    return usage(argv[0]);
  }
  const auris_array_t *array = config.array;
  const auris_capture_profile_t *profile = config.capture;
  if (frame_size == 0U) {
    frame_size = profile->frame_size;
  }
  if (frame_hop == 0U) {
    frame_hop = frame_size == profile->frame_size ? profile->frame_hop
                                                  : frame_size / 2U;
  }
  if (!auris_analysis_size_valid(frame_size, frame_hop)) {
    fprintf(stderr,
            "frame must be a power of two from %u to %u samples, and the "
            "hop from %u samples to the frame size\n",
            AURIS_ANALYSIS_MIN_SIZE, AURIS_ANALYSIS_MAX_SIZE,
            AURIS_AUDIO_FRAMES);
    return EXIT_FAILURE;
  }

  if (signal(SIGINT, request_stop) == SIG_ERR ||
      signal(SIGTERM, request_stop) == SIG_ERR ||
      signal(SIGHUP, request_stop) == SIG_ERR ||
      signal(SIGPIPE, request_stop) == SIG_ERR) {
    perror("signal");
    return EXIT_FAILURE;
  }

  auris_alsa_t state = {0};
  auris_wav_reader_t recording = {0};
  auris_audio_source_t source;
  if (file != NULL) {
    const int err = auris_wav_open(&recording, file, profile,
                                   array->capture_channels,
                                   array->microphone_count);
    if (err < 0) {
      fprintf(stderr, "%s: %s\n", file,
              err == -EINVAL ? "recording does not match this array's device"
                             : strerror(-err));
      return EXIT_FAILURE;
    }
    source = (auris_audio_source_t){&recording, auris_wav_read};
  } else {
    const int err = auris_alsa_open_profile(&state, profile, device,
                                            array->capture_channels,
                                            array->microphone_count);
    if (err < 0) {
      fprintf(stderr, "open %s: %s\n",
              device != NULL ? device : profile->device, snd_strerror(err));
      return EXIT_FAILURE;
    }
    source = (auris_audio_source_t){&state, auris_alsa_read};
  }
  fprintf(stderr, "%s: %zu mics from %zu device channels, %u Hz, %s%s\n",
          array->name, array->microphone_count, profile->raw_channels,
          profile->sample_rate,
          profile->format == AURIS_SAMPLE_S32_LE ? "S32_LE" : "S16_LE",
          file != NULL ? (realtime ? ", replay in real time" : ", replay")
                       : "");
  struct timespec start;
  (void)clock_gettime(CLOCK_MONOTONIC, &start);

  auris_audio_block_t block;
  int result = EXIT_FAILURE;

  auris_framer_t framer;
  auris_preprocess_t prep;
  if (auris_framer_init(&framer, frame_size, frame_hop) != AURIS_OK ||
      auris_preprocess_init_size(&prep, frame_size) != AURIS_OK) {
    goto done;
  }
  if (!raw_blocks) {
    fprintf(stderr, "analysis: %zu-sample frames every %zu samples "
                    "(%.1f ms, hop %.1f ms, %.3f Hz bins)\n",
            frame_size, frame_hop,
            1000.0 * (double)frame_size / (double)profile->sample_rate,
            1000.0 * (double)frame_hop / (double)profile->sample_rate,
            (double)profile->sample_rate / (double)frame_size);
  }
  auris_analysis_frame_t analysis;
  auris_analysis_frame_t raw_analysis;
  size_t analysis_count = 0U;

  if (raw_blocks) {
    if (printf("AURIS1 %u %zu %u\n", profile->sample_rate,
               array->microphone_count, AURIS_AUDIO_FRAMES) < 0) {
      goto done;
    }
  } else if (printf("AURIS2 %u %zu %zu\n", profile->sample_rate,
                    array->microphone_count, frame_size) < 0) {
    goto done;
  }
  while (keep_running) {
    if (source.read(source.context, &block) != AURIS_OK) {
      goto done;
    }
    if (block.frames == 0U) {
      break; /* end of a recording */
    }
    if (realtime) {
      pace(&start, block.first_frame + block.frames, block.sample_rate);
    }
    if (raw_blocks) {
      if (!write_block(&block) || fflush(stdout) == EOF) {
        goto done;
      }
      continue;
    }
    bool ready = false;
    if (auris_framer_push(&framer, &block, &analysis, &ready) != AURIS_OK) {
      fprintf(stderr, "analysis input is invalid or discontinuous\n");
      goto done;
    }

    if (!ready) {
      continue;
    }

    raw_analysis = analysis;
    if (auris_preprocess_apply(&prep, &analysis) != AURIS_OK) {
      goto done;
    }

    for (size_t c = 0U; c < analysis.channels; ++c) {
      if (fwrite(raw_analysis.samples[c], sizeof(float), analysis.size,
                 stdout) != analysis.size) {
        goto done;
      }
    }
    for (size_t c = 0U; c < analysis.channels; ++c) {
      if (fwrite(analysis.samples[c], sizeof(float), analysis.size,
                 stdout) != analysis.size) {
        goto done;
      }
    }
    if (fflush(stdout) == EOF) {
      goto done;
    }

    ++analysis_count;
    if (analysis_count % 64U == 0U) {
      fprintf(stderr, "analysis frames: %zu\n", analysis_count);
    }
  }
  result = EXIT_SUCCESS;

done:
  if (!keep_running) {
    result = EXIT_SUCCESS;
  }
  auris_alsa_close(&state);
  auris_wav_close(&recording);
  if (result != EXIT_SUCCESS) {
    fprintf(stderr, "capture failed\n");
  }
  return result;
}
