/* Bring-up check for the image processor: frames from our own capture at a
   chosen sensor mode go through auris_vpac, each step is reported as it
   happens (so a hang shows where it is), and the last picture is saved as a
   PPM to look at. Run as root:
     vpac_probe <width> <height> <bits> <frames> [out.ppm [crop_w crop_h]]
   With crop_w x crop_h, only the centre of each frame goes to the
   processor. */
#include "auris_camera.h"
#include "auris_vpac.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1.0e6;
}

static void say(const char *what) {
  printf("probe: %s\n", what);
  (void)fflush(stdout);
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s width height bits frames [out.ppm]\n", argv[0]);
    return 2;
  }
  const uint32_t width = (uint32_t)strtoul(argv[1], NULL, 10);
  const uint32_t height = (uint32_t)strtoul(argv[2], NULL, 10);
  const unsigned int bits = (unsigned int)strtoul(argv[3], NULL, 10);
  const unsigned long frames = strtoul(argv[4], NULL, 10);
  const char *ppm = argc > 5 && strcmp(argv[5], "-") != 0 ? argv[5] : NULL;
  const uint32_t crop_w = argc > 7 ? (uint32_t)strtoul(argv[6], NULL, 10) : width;
  const uint32_t crop_h = argc > 7 ? (uint32_t)strtoul(argv[7], NULL, 10) : height;
  setvbuf(stdout, NULL, _IOLBF, 0);

  auris_camera_t *camera = NULL;
  const auris_camera_config_t capture = {
      .sensor = "/dev/v4l-subdev2",
      .bridge = "/dev/v4l-subdev1",
      .device = "/dev/v4l/by-path/platform-4510000.ticsi2rx-video-index0",
      .width = width,
      .height = height,
      .ti_5_10_packed = true,
      .bits = bits};
  int err = auris_camera_open(&camera, &capture);
  if (err < 0) {
    fprintf(stderr, "camera: %s\n", strerror(-err));
    return 1;
  }
  const auris_camera_format_t *f = auris_camera_format(camera);
  printf("probe: camera %ux%u, %u bits, stride %u\n", f->width, f->height,
         f->bits, f->stride);
  auris_camera_exposure_range_t range = {0};
  (void)auris_camera_exposure_range(camera, &range);
  uint32_t lines = range.max_lines / 2U;
  float analogue = 2.0f, digital = 1.0f;
  (void)auris_camera_set_exposure(camera, &lines, &analogue, &digital);

  const uint32_t out_w = (crop_w / 4U) & ~1U, out_h = (crop_h / 4U) & ~1U;
  if (crop_w > f->width || crop_h > f->height) {
    fprintf(stderr, "crop larger than the frame\n");
    auris_camera_close(camera);
    return 1;
  }
  const size_t sample = f->bits == 8U ? 1U : 2U;
  const size_t offset = (size_t)((f->height - crop_h) / 2U) * f->stride +
                        (size_t)((f->width - crop_w) / 2U) * sample;
  printf("probe: processor input %ux%u\n", crop_w, crop_h);
  const auris_vpac_config_t config = {
      .source_width = crop_w,
      .source_height = crop_h,
      .source_stride = f->stride,
      .bits = f->bits,
      .width = out_w,
      .height = out_h,
      .isp_tuning = "/opt/imaging/imx219/dcc_viss.bin",
      .aewb_tuning = "/opt/imaging/imx219/dcc_2a.bin",
      .statistics = getenv("PROBE_STATISTICS") != NULL,
      .min_exposure_us = 100U,
      .max_exposure_us = 33000U,
      .max_analogue = 8.0f};
  auris_vpac_t *vpac = NULL;
  say("opening the image processor");
  double t0 = now_ms();
  err = auris_vpac_open(&vpac, &config);
  if (err < 0) {
    fprintf(stderr, "vpac: %s\n", strerror(-err));
    auris_camera_close(camera);
    return 1;
  }
  printf("probe: opened in %.0f ms, output %ux%u\n", now_ms() - t0, out_w,
         out_h);
  err = auris_camera_start(camera);
  if (err < 0) {
    fprintf(stderr, "start: %s\n", strerror(-err));
    goto done;
  }
  auris_nv12_t picture = {0};
  double total = 0.0, worst = 0.0;
  unsigned long done_frames = 0U;
  for (unsigned long n = 0U; n < frames; ++n) {
    auris_camera_frame_t frame;
    err = auris_camera_acquire(camera, &frame, 1000);
    if (err == -EBADMSG || err == -EAGAIN || err == -EINTR)
      continue;
    if (err < 0) {
      fprintf(stderr, "acquire: %s\n", strerror(-err));
      goto done;
    }
    if (n == 0U)
      say("first frame captured; processing it");
    auris_vpac_result_t result;
    t0 = now_ms();
    err = auris_vpac_process(vpac, frame.data + offset, frame.bytes - offset,
                             &picture, &result);
    const double took = now_ms() - t0;
    (void)auris_camera_release(camera);
    if (err < 0) {
      fprintf(stderr, "process: %s\n", strerror(-err));
      goto done;
    }
    total += took;
    worst = took > worst ? took : worst;
    ++done_frames;
    if (n == 0U || n % 30U == 29U) {
      float mean = 0.0f;
      (void)auris_nv12_mean(&picture, &mean);
      printf("probe: frame %lu took %.2f ms | 2A wants %u us x%.2f (%s) | "
             "balance R %.2f B %.2f | %u K | level %.3f | gains %u %u %u %u\n",
             n + 1U, took, result.exposure_us,
             (double)result.analogue_q10 / 1024.0,
             result.exposure_valid ? "valid" : "invalid",
             (double)result.balance_red, (double)result.balance_blue,
             result.colour_temperature, (double)mean, result.wb_gains[0],
             result.wb_gains[1], result.wb_gains[2], result.wb_gains[3]);
    }
  }
  if (done_frames > 0U)
    printf("probe: %lu frames, %.2f ms each on average, worst %.2f ms\n",
           done_frames, total / (double)done_frames, worst);
  if (ppm != NULL && picture.y != NULL) {
    uint8_t *rgb = malloc((size_t)picture.width * picture.height * 3U);
    FILE *out = fopen(ppm, "wb");
    if (rgb != NULL && out != NULL) {
      const auris_rgb24_t canvas = {rgb, picture.width, picture.height,
                                    (size_t)picture.width * 3U,
                                    (size_t)picture.width * picture.height *
                                        3U};
      if (auris_nv12_to_rgb24(&picture, &canvas, 0U, 0U) == 0) {
        fprintf(out, "P6\n%u %u\n255\n", picture.width, picture.height);
        (void)fwrite(rgb, 1U, canvas.bytes, out);
        printf("probe: saved %s\n", ppm);
      }
    }
    if (out != NULL)
      (void)fclose(out);
    free(rgb);
  }
done:
  auris_vpac_close(vpac);
  auris_camera_close(camera);
  return err < 0 ? 1 : 0;
}
