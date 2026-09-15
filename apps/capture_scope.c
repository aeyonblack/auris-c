#include "auris_alsa.h"
#include "auris_array_configs.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(float) == 4U, "scope requires 32-bit floats");

static volatile sig_atomic_t keep_running = 1;

static void request_stop(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s hw:CARD=your_card,DEV=0\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (signal(SIGINT, request_stop) == SIG_ERR ||
      signal(SIGTERM, request_stop) == SIG_ERR ||
      signal(SIGHUP, request_stop) == SIG_ERR ||
      signal(SIGPIPE, request_stop) == SIG_ERR) {
    perror("signal");
    return EXIT_FAILURE;
  }

  const auris_array_t *array = auris_config_respeaker_usb_4();
  auris_alsa_t state = {0};
  const int err = auris_alsa_open(&state, argv[1], array->capture_channels,
                                  array->microphone_count);
  if (err < 0) {
    fprintf(stderr, "open: %s\n", snd_strerror(err));
    return EXIT_FAILURE;
  }

  auris_audio_source_t source = {.context = &state, .read = auris_alsa_read};

  auris_audio_block_t block;
  int result = EXIT_FAILURE;

  if (printf("AURIS1 %u %zu %u\n", AURIS_ALSA_RATE, array->microphone_count,
             AURIS_AUDIO_FRAMES) < 0) {
    goto done;
  }
  while (keep_running) {
    if (source.read(source.context, &block) != AURIS_OK) {
      goto done;
    }
    for (size_t c = 0U; c < block.channels; ++c) {
      if (fwrite(block.samples[c], sizeof(float), block.frames, stdout) !=
          block.frames) {
        goto done;
      }
    }
    if (fflush(stdout) == EOF) {
      goto done;
    }
  }
  result = EXIT_SUCCESS;

done:
  if (!keep_running) {
    result = EXIT_SUCCESS;
  }
  auris_alsa_close(&state);
  if (result != EXIT_SUCCESS) {
    fprintf(stderr, "capture failed\n");
  }
  return result;
}
