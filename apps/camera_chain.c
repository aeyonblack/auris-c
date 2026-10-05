#include "camera_chain.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENSOR_WIDTH 1640U
#define SENSOR_HEIGHT 1232U

struct camera_chain {
  auris_camera_t *camera;
  auris_image_t *image;
  auris_exposure_t exposure;
  auris_camera_exposure_range_t range;
  bool exposure_control, shading;
  float *gains;
};

int camera_chain_load_shading(const char *path, uint32_t *columns,
                              uint32_t *rows, float *gains) {
  if (path == NULL || columns == NULL || rows == NULL || gains == NULL)
    return -EINVAL;
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return errno == ENOENT ? -ENOENT : -EINVAL;
  char magic[16] = {0};
  unsigned int c = 0U, r = 0U;
  int err = -EINVAL;
  if (fscanf(f, "%15s %u %u", magic, &c, &r) == 3 &&
      strcmp(magic, "AURISSHADE1") == 0 && c >= 2U && r >= 2U && c <= 64U &&
      r <= 64U) {
    const size_t count = (size_t)c * r * 3U;
    size_t read = 0U;
    while (read < count && fscanf(f, "%f", &gains[read]) == 1 &&
           isfinite(gains[read]) && gains[read] > 0.0f && gains[read] < 100.0f)
      ++read;
    char extra[2];
    if (read == count && fscanf(f, "%1s", extra) != 1) {
      *columns = c;
      *rows = r;
      err = 0;
    }
  }
  (void)fclose(f);
  return err;
}

void camera_chain_close(camera_chain_t *s) {
  if (s == NULL)
    return;
  auris_camera_close(s->camera);
  auris_image_close(s->image);
  free(s->gains);
  free(s);
}

int camera_chain_open(camera_chain_t **out, uint32_t width, uint32_t height,
                      const char *shading_path) {
  if (out == NULL || *out != NULL)
    return -EINVAL;
  camera_chain_t *s = calloc(1U, sizeof(*s));
  if (s == NULL)
    return -ENOMEM;
  const auris_camera_config_t capture = {
      .sensor = "/dev/v4l-subdev2",
      .bridge = "/dev/v4l-subdev1",
      .device = "/dev/v4l/by-path/platform-4510000.ticsi2rx-video-index0",
      .width = SENSOR_WIDTH,
      .height = SENSOR_HEIGHT,
      .ti_5_10_packed = true,
      .bits = 10U};
  int err = auris_camera_open(&s->camera, &capture);
  if (err < 0)
    goto fail;
  auris_image_config_t processing = {
      .source_width = SENSOR_WIDTH,
      .source_height = SENSOR_HEIGHT,
      .width = width,
      .height = height,
      .bits = 10U,
      .black = 64.0f, /* IMX219 */
      /* Where the balance starts: indoor lamp light. */
      .gain = {1.5f, 1.0f, 2.1f},
      .colour_matrix = {{2.18174f, -0.70887f, -0.47287f},
                        {-0.70196f, 2.76426f, -1.06231f},
                        {-0.25157f, -0.71978f, 1.97135f}},
      .target = 0.18f,
      .max_gain = 16.0f,
      /* The matrix above triples the noise; in the dark that shows as
         purple speckle unless colour gives way there. */
      .shadow_knee = 0.02f,
      .contrast = 0.35f,
      .auto_white_balance = true,
      .auto_exposure = true};
  if (shading_path != NULL) {
    s->gains = malloc(CAMERA_SHADING_MAX_NODES * 3U * sizeof(*s->gains));
    if (s->gains == NULL) {
      err = -ENOMEM;
      goto fail;
    }
    uint32_t columns = 0U, rows = 0U;
    const int loaded =
        camera_chain_load_shading(shading_path, &columns, &rows, s->gains);
    if (loaded == 0) {
      processing.shading = (auris_shading_t){columns, rows, s->gains};
      s->shading = true;
    } else if (loaded != -ENOENT) {
      fprintf(stderr, "camera: %s is not a lens-shading table; ignored\n",
              shading_path);
    }
  }
  err = auris_image_open(&s->image, &processing);
  if (err < 0)
    goto fail;
  if (auris_camera_exposure_range(s->camera, &s->range) == 0) {
    /* The sensor's own digital gain adds nothing the software gain does
       not, so it is used only sparingly. */
    const auris_exposure_config_t aim = auris_exposure_defaults(
        s->range.line_us, s->range.min_lines, s->range.max_lines,
        s->range.max_analogue, fminf(s->range.max_digital, 2.0f));
    /* A quarter of the longest flicker-free time, so the first frames of
       a bright scene are not blank. */
    auris_exposure_setting_t start = auris_exposure_split(&aim, 7500.0f);
    if (auris_camera_set_exposure(s->camera, &start.lines, &start.analogue,
                                  &start.digital) == 0 &&
        auris_exposure_init(&s->exposure, &aim, &start) == 0)
      s->exposure_control = true;
  }
  if (!s->exposure_control)
    fprintf(stderr, "camera: no exposure control; brightness is software "
                    "only\n");
  *out = s;
  return 0;
fail:
  camera_chain_close(s);
  return err;
}

const auris_camera_format_t *camera_chain_format(const camera_chain_t *s) {
  return s == NULL ? NULL : auris_camera_format(s->camera);
}

const auris_image_rect_t *camera_chain_content(const camera_chain_t *s) {
  return s == NULL ? NULL : auris_image_content(s->image);
}

/* What the frame says about the exposure, sent on to the sensor. */
static void steer(camera_chain_t *s, const auris_camera_frame_t *frame) {
  auris_image_stats_t seen;
  auris_exposure_setting_t next;
  if (s->exposure_control && auris_image_stats(s->image, &seen) == 0 &&
      auris_exposure_step(&s->exposure, frame->timestamp_us, seen.mean,
                          seen.bright, &next)) {
    /* A failed write is not fatal: the picture is still gained in
       software, and the next frame tries again. */
    if (auris_camera_set_exposure(s->camera, &next.lines, &next.analogue,
                                  &next.digital) == 0)
      auris_exposure_applied(&s->exposure, next, frame->timestamp_us);
  }
}

int camera_chain_start(camera_chain_t *s) {
  if (s == NULL)
    return -EINVAL;
  int err = auris_camera_start(s->camera);
  /* Up to half a second of frames are looked at but not shown, so the
     first picture is already exposed for the scene. */
  const auris_camera_format_t *f = auris_camera_format(s->camera);
  for (unsigned int n = 0U; err == 0 && s->exposure_control && n < 16U; ++n) {
    auris_camera_frame_t frame;
    const int got = auris_camera_acquire(s->camera, &frame, 500);
    if (got == -EBADMSG || got == -EINTR || got == -EAGAIN)
      continue;
    if (got < 0)
      break; /* the caller's own first acquire reports it */
    const auris_bayer_t raw = {frame.data, f->width, f->height, f->stride,
                               frame.bytes, f->bits};
    if (auris_image_measure(s->image, &raw) == 0)
      steer(s, &frame);
    err = auris_camera_release(s->camera);
  }
  return err;
}

int camera_chain_acquire(camera_chain_t *s, auris_camera_frame_t *frame,
                         int timeout_ms) {
  return s == NULL ? -EINVAL
                   : auris_camera_acquire(s->camera, frame, timeout_ms);
}

int camera_chain_release(camera_chain_t *s) {
  return s == NULL ? -EINVAL : auris_camera_release(s->camera);
}

int camera_chain_render(camera_chain_t *s, const auris_camera_frame_t *frame,
                        auris_rgb24_t *canvas) {
  if (s == NULL || frame == NULL || canvas == NULL)
    return -EINVAL;
  const auris_camera_format_t *f = auris_camera_format(s->camera);
  const auris_bayer_t raw = {frame->data, f->width, f->height, f->stride,
                             frame->bytes, f->bits};
  const int err = auris_image_render(s->image, &raw, canvas);
  if (err == 0)
    steer(s, frame);
  return err;
}

void camera_chain_status(const camera_chain_t *s, camera_chain_status_t *out) {
  if (s == NULL || out == NULL)
    return;
  *out = (camera_chain_status_t){.exposure_control = s->exposure_control,
                                 .shading = s->shading, .software = 1.0f};
  auris_image_stats_t seen;
  if (auris_image_stats(s->image, &seen) == 0) {
    out->software = seen.exposure;
    out->balance_red = seen.gain[0];
    out->balance_blue = seen.gain[2];
    out->mean = seen.mean;
    out->clipped = seen.clipped;
  }
  if (s->exposure_control) {
    out->exposure_ms =
        (float)s->exposure.setting.lines * s->range.line_us / 1000.0f;
    out->analogue = s->exposure.setting.analogue;
    out->digital = s->exposure.setting.digital;
  }
}
