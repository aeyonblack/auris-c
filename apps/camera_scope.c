#include "auris_camera.h"
#include "auris_image.h"
#include <errno.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t running = 1;
static void stop(int signal_number) {
  (void)signal_number;
  running = 0;
}

int main(int argc, char **argv) {
  unsigned long limit = 0U;
  if (argc > 2)
    return EXIT_FAILURE;
  if (argc == 2) {
    char *end = NULL;
    errno = 0;
    if (argv[1][0] < '0' || argv[1][0] > '9')
      return EXIT_FAILURE;
    limit = strtoul(argv[1], &end, 10);
    if (errno != 0 || *end != '\0')
      return EXIT_FAILURE;
  }
  struct sigaction action = {0};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
    return EXIT_FAILURE;
  action.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &action, NULL))
    return EXIT_FAILURE;
  const auris_camera_config_t config = {
      .sensor = "/dev/v4l-subdev2",
      .bridge = "/dev/v4l-subdev1",
      .device = "/dev/v4l/by-path/platform-4510000.ticsi2rx-video-index0",
      .width = 1640U,
      .height = 1232U,
      .ti_5_10_packed = true};
  auris_camera_t *camera = NULL;
  auris_image_t *image = NULL;
  uint8_t *rgb = NULL;
  int result = EXIT_FAILURE;
  int err = auris_camera_open(&camera, &config);
  if (err < 0) {
    fprintf(stderr, "camera open: %s\n", strerror(-err));
    goto done;
  }
  const auris_camera_format_t *f = auris_camera_format(camera);
  const auris_image_config_t processing = {
      .source_width = f->width,
      .source_height = f->height,
      .width = 640U,
      .height = 360U,
      .black = 0.0f,
      .gain = {1.0f, 1.0f, 1.0f},
      .exposure_ev = 0.0f,
      .gamma = 2.2f};
  err = auris_image_open(&image, &processing);
  if (err < 0) {
    fprintf(stderr, "image open: %s\n", strerror(-err));
    goto done;
  }
  const size_t bytes =
      (size_t)processing.width * processing.height * 3U;
  rgb = malloc(bytes);
  if (rgb == NULL)
    goto done;
  auris_rgb24_t output = {rgb, processing.width, processing.height,
                          (size_t)processing.width * 3U, bytes};
  const auris_image_rect_t *rect = auris_image_content(image);
  fprintf(stderr, "RGB %ux%u, content %ux%u at (%u,%u)\n", output.width,
          output.height, rect->width, rect->height, rect->x, rect->y);
  fprintf(stderr,
          "RGGB %ux%u: reported stride %u, used stride %u, image %u bytes\n",
          f->width, f->height, f->reported_stride, f->stride, f->sizeimage);
  (void)setvbuf(stdout, NULL, _IONBF, 0);
  if (printf("AURISCAM1 %u %u\n", output.width, output.height) < 0)
    goto done;
  err = auris_camera_start(camera);
  if (err < 0) {
    fprintf(stderr, "camera start: %s\n", strerror(-err));
    goto done;
  }
  unsigned long frames = 0U;
  uint32_t previous = 0U;
  uint64_t first_us = 0U, gaps = 0U;
  while (running && (limit == 0U || frames < limit)) {
    auris_camera_frame_t frame;
    err = auris_camera_acquire(camera, &frame, 1000);
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
    const bool send = frames % 6U == 0U;
    if (send) {
      const auris_bayer8_t input = {frame.data, f->width, f->height,
                                    f->stride, frame.bytes};
      err = auris_image_render(image, &input, &output);
      if (err < 0) {
        fprintf(stderr, "image render: %s\n", strerror(-err));
        goto done;
      }
    }
    err = auris_camera_release(camera);
    if (err < 0) {
      fprintf(stderr, "camera release: %s\n", strerror(-err));
      goto done;
    }
    ++frames;
    /* Only the copied RGB buffer is used after release. */
    if (send && (printf("FRAME %u %llu %u %zu\n", frame.sequence,
                        (unsigned long long)frame.timestamp_us, frame.flags,
                        bytes) < 0 ||
                 fwrite(rgb, 1U, bytes, stdout) != bytes))
      goto done;
    if (frames % 60U == 0U && frame.timestamp_us > first_us &&
        (frame.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
            V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
      fprintf(stderr,
              "received %lu, sequence gaps %llu, mean %.2f fps, flags 0x%x\n",
              frames, (unsigned long long)gaps,
              (double)(frames - 1U) * 1000000.0 /
                  (double)(frame.timestamp_us - first_us),
              frame.flags);
    }
  }
  result = EXIT_SUCCESS;
done:
  auris_camera_close(camera);
  auris_image_close(image);
  free(rgb);
  return running ? result : EXIT_SUCCESS;
}
