#include "camera_chain.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENSOR_WIDTH 1640U
#define SENSOR_HEIGHT 1232U
#define ISP_TUNING "/opt/imaging/imx219/dcc_viss.bin"
/* The light the image processor's tuning is chosen for: indoor lamps. */
#define COLOUR_TEMPERATURE 4000U
/* Exposure aim and digital gain limit with the image processor (the
   software path aims at 0.16 and stops digital gain at x2). */
#define HARDWARE_TARGET 0.22f
#define HARDWARE_MAX_DIGITAL 4.0f

struct camera_chain {
  auris_camera_t *camera;
  /* Software processing; with the image processor, still what measures
     each frame for exposure and white balance. */
  auris_image_t *image;
  auris_vpac_t *vpac;     /* the image processor; NULL in software */
  auris_image_rect_t rect; /* the image processor's picture on the canvas */
  uint32_t width, height; /* the canvas */
  auris_exposure_t exposure;
  auris_camera_exposure_range_t range;
  bool exposure_control, shading;
  float *gains;
};

int camera_chain_hardware_rect(uint32_t source_width, uint32_t source_height,
                               uint32_t width, uint32_t height,
                               auris_image_rect_t *rect) {
  if (width < 2U || height < 2U || rect == NULL)
    return -EINVAL;
  auris_image_rect_t r;
  const int err =
      auris_image_fit(source_width, source_height, width, height, &r);
  if (err < 0)
    return err;
  /* An odd side goes up by one if the canvas has room, else down. */
  if (r.width % 2U != 0U)
    r.width = r.width + 1U <= width ? r.width + 1U : r.width - 1U;
  if (r.height % 2U != 0U)
    r.height = r.height + 1U <= height ? r.height + 1U : r.height - 1U;
  *rect = (auris_image_rect_t){(width - r.width) / 2U,
                               (height - r.height) / 2U, r.width, r.height};
  return 0;
}

bool camera_chain_wants_hardware(const char *software) {
  return software == NULL || software[0] == '\0' ||
         strcmp(software, "0") == 0;
}

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
  auris_vpac_close(s->vpac);
  auris_camera_close(s->camera);
  auris_image_close(s->image);
  free(s->gains);
  free(s);
}

/* The image processor for the picture; our own measurements still steer
   the exposure and white balance (TI's need H3A statistics, which its
   tuning only allows on 1920-wide frames). */
static int open_hardware(camera_chain_t *s) {
  const auris_camera_format_t *f = auris_camera_format(s->camera);
  int err = camera_chain_hardware_rect(SENSOR_WIDTH, SENSOR_HEIGHT, s->width,
                                       s->height, &s->rect);
  if (err < 0)
    return err;
  const auris_vpac_config_t config = {.source_width = f->width,
                                      .source_height = f->height,
                                      .source_stride = f->stride,
                                      .bits = f->bits,
                                      .width = s->rect.width,
                                      .height = s->rect.height,
                                      .isp_tuning = ISP_TUNING};
  err = auris_vpac_open(&s->vpac, &config);
  if (err < 0)
    return err;
  fprintf(stderr, "camera: image processor (VPAC) with TI's tuning, %ux%u "
                  "picture at %u,%u\n",
          s->rect.width, s->rect.height, s->rect.x, s->rect.y);
  return 0;
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
  s->width = width;
  s->height = height;
  int err = auris_camera_open(&s->camera, &capture);
  if (err < 0)
    goto fail;
  const bool ranged = auris_camera_exposure_range(s->camera, &s->range) == 0;
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
  /* The image processor has no software gain to fall back on, so it is
     used only when the sensor's exposure can be set. */
  if (camera_chain_wants_hardware(getenv("AURIS_CAMERA_SOFTWARE"))) {
    const int tried = ranged ? open_hardware(s) : -ENOTSUP;
    if (tried < 0)
      fprintf(stderr, "camera: image processor unavailable (%s); software "
                      "processing\n",
              strerror(-tried));
  }
  if (ranged) {
    /* In software, the sensor's own digital gain adds nothing the software
       gain does not, so it is used only sparingly. The image processor has
       no software gain, and TI's tone curve shows a given exposure darker
       than ours, so there the aim is higher and digital gain goes further. */
    auris_exposure_config_t aim = auris_exposure_defaults(
        s->range.line_us, s->range.min_lines, s->range.max_lines,
        s->range.max_analogue,
        fminf(s->range.max_digital, s->vpac != NULL ? HARDWARE_MAX_DIGITAL
                                                    : 2.0f));
    if (s->vpac != NULL)
      aim.target = HARDWARE_TARGET;
    /* A quarter of the longest flicker-free time, so the first frames of
       a bright scene are not blank. */
    auris_exposure_setting_t start = auris_exposure_split(&aim, 7500.0f);
    if (auris_camera_set_exposure(s->camera, &start.lines, &start.analogue,
                                  &start.digital) == 0 &&
        auris_exposure_init(&s->exposure, &aim, &start) == 0)
      s->exposure_control = true;
  }
  if (!s->exposure_control) {
    fprintf(stderr, "camera: no exposure control; brightness is software "
                    "only\n");
    if (s->vpac != NULL) {
      auris_vpac_close(s->vpac);
      s->vpac = NULL;
      fprintf(stderr, "camera: software processing\n");
    }
  }
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
  if (s == NULL)
    return NULL;
  return s->vpac != NULL ? &s->rect : auris_image_content(s->image);
}

static void steer(camera_chain_t *s, const auris_camera_frame_t *frame);

/* One frame through the image processor, drawn into the canvas with the
   canvas outside the picture made black. The frame is measured first (a
   sparse pass over the raw samples), so its exposure is steered and its
   white balance is the one the processor applies. */
static int process_hardware(camera_chain_t *s,
                            const auris_camera_frame_t *frame,
                            auris_rgb24_t *canvas) {
  if (canvas->width != s->width || canvas->height != s->height)
    return -EINVAL;
  const auris_camera_format_t *f = auris_camera_format(s->camera);
  const auris_bayer_t raw = {frame->data, f->width,     f->height,
                             f->stride,   frame->bytes, f->bits};
  int err = auris_image_measure(s->image, &raw);
  if (err < 0)
    return err;
  steer(s, frame);
  auris_image_stats_t seen;
  err = auris_image_stats(s->image, &seen);
  if (err < 0)
    return err;
  const auris_exposure_setting_t *now = &s->exposure.setting;
  err = auris_vpac_set_balance(
      s->vpac, seen.gain[0] / seen.gain[1], seen.gain[2] / seen.gain[1],
      (uint32_t)((float)now->lines * s->range.line_us),
      now->analogue * now->digital, COLOUR_TEMPERATURE);
  if (err < 0)
    return err;
  auris_nv12_t picture;
  auris_vpac_result_t unused;
  err = auris_vpac_process(s->vpac, frame->data, frame->bytes, &picture,
                           &unused);
  if (err < 0)
    return err;
  err = auris_nv12_to_rgb24(&picture, canvas, s->rect.x, s->rect.y);
  if (err < 0)
    return err;
  const auris_image_rect_t *r = &s->rect;
  for (uint32_t y = 0U; y < canvas->height; ++y) {
    uint8_t *row = canvas->data + (size_t)y * canvas->stride;
    if (y < r->y || y >= r->y + r->height) {
      memset(row, 0, (size_t)canvas->width * 3U);
      continue;
    }
    memset(row, 0, (size_t)r->x * 3U);
    memset(row + (size_t)(r->x + r->width) * 3U, 0,
           (size_t)(canvas->width - r->x - r->width) * 3U);
  }
  return 0;
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
    const auris_bayer_t raw = {frame.data, f->width,    f->height,
                               f->stride,  frame.bytes, f->bits};
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
  if (s->vpac != NULL)
    return process_hardware(s, frame, canvas);
  const auris_camera_format_t *f = auris_camera_format(s->camera);
  const auris_bayer_t raw = {frame->data, f->width,     f->height,
                             f->stride,   frame->bytes, f->bits};
  const int err = auris_image_render(s->image, &raw, canvas);
  if (err == 0)
    steer(s, frame);
  return err;
}

void camera_chain_status(const camera_chain_t *s, camera_chain_status_t *out) {
  if (s == NULL || out == NULL)
    return;
  *out = (camera_chain_status_t){.exposure_control = s->exposure_control,
                                 .shading = s->shading,
                                 .hardware = s->vpac != NULL,
                                 .software = 1.0f};
  auris_image_stats_t seen;
  if (auris_image_stats(s->image, &seen) == 0) {
    /* The image processor applies no software gain. */
    out->software = s->vpac != NULL ? 1.0f : seen.exposure;
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
