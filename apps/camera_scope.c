#include "camera_chain.h"
#include "auris_outputs.h"
#include <errno.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void stop(int signal_number) {
  (void)signal_number;
  running = 0;
}

int main(int argc, char **argv) {
  auris_outputs_config_t outputs={0};
  unsigned long limit = 0U;
  unsigned long fps = 15U;
  bool limit_set = false;
  for (int i = 1; i < argc; ++i) {
    int parsed=auris_outputs_option(&outputs,argc,argv,&i);
    if (parsed<0) {
      fprintf(stderr,"output options: --output stream|display (repeatable), --rotation 0|90|180|270, --drm-device PATH, --connector ID\n");
      return EXIT_FAILURE;
    }
    if (parsed) continue;
    const bool rate = strcmp(argv[i], "--fps") == 0;
    if ((rate && i + 1 >= argc) || (!rate && limit_set)) {
      fprintf(stderr, "usage: %s [--fps 1..30] [captured_frames]\n", argv[0]);
      return EXIT_FAILURE;
    }
    const char *text = rate ? argv[++i] : argv[i];
    char *end = NULL;
    errno = 0;
    if (text[0] < '0' || text[0] > '9')
      end = (char *)text;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        (rate && (value < 1U || value > 30U))) {
      fprintf(stderr, "usage: %s [--fps 1..30] [captured_frames]\n", argv[0]);
      return EXIT_FAILURE;
    }
    if (rate)
      fps = value;
    else
      limit = value, limit_set = true;
  }
  struct sigaction action = {0};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
    return EXIT_FAILURE;
  action.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &action, NULL))
    return EXIT_FAILURE;
  camera_chain_t *chain = NULL;
  auris_outputs_t *output_stream = NULL;
  uint8_t *rgb = NULL;
  int result = EXIT_FAILURE;
  int err = camera_chain_open(&chain, 640U, 360U, "configs/lens_shading.txt");
  if (err < 0) {
    fprintf(stderr, "camera open: %s\n", strerror(-err));
    goto done;
  }
  const auris_camera_format_t *f = camera_chain_format(chain);
  const size_t bytes = 640U * 360U * 3U;
  rgb = malloc(bytes);
  if (rgb == NULL)
    goto done;
  auris_rgb24_t output = {rgb, 640U, 360U, 640U * 3U, bytes};
  const auris_image_rect_t *rect = camera_chain_content(chain);
  fprintf(stderr, "RGB %ux%u, content %ux%u at (%u,%u)\n", output.width,
          output.height, rect->width, rect->height, rect->x, rect->y);
  fprintf(stderr,
          "RGGB %ux%u, %u bits: reported stride %u, used stride %u, image %u "
          "bytes\n",
          f->width, f->height, f->bits, f->reported_stride, f->stride,
          f->sizeimage);
  err = auris_outputs_open(&output_stream, &outputs, STDOUT_FILENO,
                            output.width, output.height);
  if (err < 0) {
    fprintf(stderr, "output open: %s\n", strerror(-err));
    goto done;
  }
  err = camera_chain_start(chain);
  if (err < 0) {
    fprintf(stderr, "camera start: %s\n", strerror(-err));
    goto done;
  }
  unsigned long frames = 0U;
  uint32_t previous = 0U;
  uint64_t first_us = 0U, gaps = 0U, next_output = 0U;
  const uint64_t output_period_us = 1000000U / fps;
  while (running && (limit == 0U || frames < limit)) {
    err = auris_outputs_error(output_stream);
    if (err < 0) {
      fprintf(stderr, "output: %s\n", strerror(-err));
      goto done;
    }
    auris_camera_frame_t frame;
    err = camera_chain_acquire(chain, &frame, 1000);
    if (err == -EINTR || err == -EAGAIN)
      continue;
    if (err == -EBADMSG) {
      fprintf(stderr, "discarded damaged frame\n");
      continue;
    }
    if (err < 0) {
      fprintf(stderr, "camera acquire: %s\n", strerror(-err));
      goto done;
    }
    if (frames == 0U)
      first_us = frame.timestamp_us;
    else {
      const uint32_t delta = frame.sequence - previous;
      if (delta > 1U)
        gaps += (uint64_t)delta - 1U;
    }
    previous = frame.sequence;
    const bool send = next_output == 0U || frame.timestamp_us >= next_output;
    if (send) {
      err = camera_chain_render(chain, &frame, &output);
      if (err < 0) {
        fprintf(stderr, "image render: %s\n", strerror(-err));
        goto done;
      }
      if (next_output == 0U)
        next_output = frame.timestamp_us;
      do {
        next_output += output_period_us;
      } while (next_output <= frame.timestamp_us);
    }
    err = camera_chain_release(chain);
    if (err < 0) {
      fprintf(stderr, "camera release: %s\n", strerror(-err));
      goto done;
    }
    ++frames;
    /* The output frontend copies RGB into a latest-frame mailbox. */
    if (send) {
      const auris_output_frame_t metadata = {
          .sequence = frame.sequence,
          .camera_us = frame.timestamp_us,
          .flags = frame.flags};
      err = auris_outputs_submit(output_stream, &output, &metadata);
      if (err < 0) {
        fprintf(stderr, "output submit: %s\n", strerror(-err));
        goto done;
      }
    }
    if (frames % 60U == 0U && frame.timestamp_us > first_us &&
        (frame.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
            V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
      fprintf(stderr,
              "received %lu, sequence gaps %llu, mean %.2f fps, flags 0x%x\n",
              frames, (unsigned long long)gaps,
              (double)(frames - 1U) * 1000000.0 /
                  (double)(frame.timestamp_us - first_us),
              frame.flags);
      camera_chain_status_t now;
      camera_chain_status(chain, &now);
      fprintf(stderr,
              "EXPOSURE %.1f ms, analogue x%.2f, digital x%.2f, software "
              "x%.2f | balance R %.2f B %.2f | level %.3f, %.1f %% at the "
              "limit%s\n",
              (double)now.exposure_ms, (double)now.analogue,
              (double)now.digital, (double)now.software,
              (double)now.balance_red, (double)now.balance_blue,
              (double)now.mean, 100.0 * (double)now.clipped,
              now.shading ? " | lens shading corrected" : "");
    }
  }
  if (limit != 0U) {
    err = auris_outputs_flush(output_stream, 3000U);
    if (err < 0) {
      fprintf(stderr, "output flush: %s\n", strerror(-err));
      goto done;
    }
  }
  result = EXIT_SUCCESS;
done:
  if (output_stream != NULL) {
    auris_output_stats_t stats;
    auris_outputs_stats(output_stream, &stats);
    fprintf(stderr,
            "primary output %lu fps: submitted %llu, sent %llu, replaced %llu, last payload %zu bytes\n",
            fps,
            (unsigned long long)stats.submitted,
            (unsigned long long)stats.sent,
            (unsigned long long)stats.dropped, stats.encoded_bytes);
  }
  auris_outputs_close(output_stream);
  camera_chain_close(chain);
  free(rgb);
  return running ? result : EXIT_SUCCESS;
}
