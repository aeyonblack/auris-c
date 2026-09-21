#include "auris_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <jpeglib.h>

typedef struct {
  int fd, flags;
  uint32_t width, height;
  unsigned int quality;
} stream_t;
typedef struct { struct jpeg_error_mgr base; jmp_buf jump; } jpeg_error_t;

static uint64_t now_us(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000U + (uint64_t)t.tv_nsec / 1000U;
}
static void jpeg_fail(j_common_ptr common) {
  jpeg_error_t *error = (jpeg_error_t *)common->err;
  longjmp(error->jump, 1);
}
static int write_all(stream_t *s, const void *data, size_t bytes,
                      auris_output_cancel_t cancel, void *context) {
  const uint8_t *cursor = data;
  while (bytes) {
    if (cancel(context)) return -ECANCELED;
    ssize_t n = write(s->fd, cursor, bytes);
    if (n > 0) { cursor += (size_t)n; bytes -= (size_t)n; continue; }
    if (n == 0) return -EIO;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
    struct pollfd p = {.fd=s->fd, .events=POLLOUT};
    if (poll(&p, 1U, 50) < 0 && errno != EINTR) return -errno;
  }
  return 0;
}
static int start(void *context, auris_output_cancel_t cancel, void *token) {
  stream_t *s = context;
  char text[96];
  const int n = snprintf(text, sizeof(text), "AURISJPEG1 %u %u\n", s->width, s->height);
  return n < 0 || (size_t)n >= sizeof(text) ? -EOVERFLOW :
      write_all(s, text, (size_t)n, cancel, token);
}
static int encode(stream_t *s, const auris_rgb24_t *rgb,
                   unsigned char **data, unsigned long *bytes) {
  struct jpeg_compress_struct jpeg = {0};
  jpeg_error_t error;
  volatile bool created = false;
  jpeg.err = jpeg_std_error(&error.base);
  error.base.error_exit = jpeg_fail;
  if (setjmp(error.jump)) {
    if (created) jpeg_destroy_compress(&jpeg);
    free(*data); *data = NULL; return -EIO;
  }
  jpeg_create_compress(&jpeg); created = true;
  jpeg_mem_dest(&jpeg, data, bytes);
  jpeg.image_width = rgb->width; jpeg.image_height = rgb->height;
  jpeg.input_components = 3; jpeg.in_color_space = JCS_RGB;
  jpeg_set_defaults(&jpeg);
  jpeg_set_quality(&jpeg, (int)s->quality, TRUE);
  jpeg_start_compress(&jpeg, TRUE);
  while (jpeg.next_scanline < jpeg.image_height) {
    JSAMPROW row = rgb->data + (size_t)jpeg.next_scanline * rgb->stride;
    (void)jpeg_write_scanlines(&jpeg, &row, 1U);
  }
  jpeg_finish_compress(&jpeg); jpeg_destroy_compress(&jpeg);
  return 0;
}
static int present(void *context, const auris_rgb24_t *rgb,
                    const auris_output_frame_t *f, auris_output_stats_t *stats,
                    auris_output_cancel_t cancel, void *token) {
  stream_t *s = context;
  unsigned char *jpeg = NULL;
  unsigned long bytes = 0UL;
  uint64_t began = now_us();
  int err = encode(s, rgb, &jpeg, &bytes);
  stats->encode_us = now_us() - began;
  if (err < 0) return err;
  char header[512];
  int n = snprintf(header, sizeof(header),
      "FRAME %" PRIu32 " %" PRIu64 " %" PRIu32 " %lu %" PRIu64
      " %" PRIu64 " %" PRIu64 " %u %u %" PRIu32 " %" PRIu32
      " %" PRIu32 " %" PRIu64 " %" PRIu64 "\n",
      f->sequence, f->camera_us, f->flags, bytes, f->audio_us,
      f->composed_us, f->published_us, f->visible ? 1U : 0U,
      f->evidence ? 1U : 0U, f->samples, f->fits, f->checks,
      f->dsp_us, stats->dropped);
  began = now_us();
  err = n < 0 || (size_t)n >= sizeof(header) ? -EOVERFLOW :
      write_all(s, header, (size_t)n, cancel, token);
  if (!err) err = write_all(s, jpeg, (size_t)bytes, cancel, token);
  stats->write_us = now_us() - began; stats->encoded_bytes = (size_t)bytes;
  free(jpeg); return err;
}
static void close_stream(void *context) {
  stream_t *s = context;
  if (!s) return;
  if (s->flags >= 0) (void)fcntl(s->fd, F_SETFL, s->flags);
  free(s);
}
int auris_output_open_jpeg_stream(auris_output_t **out, int fd,
                                  uint32_t width, uint32_t height,
                                  unsigned int quality) {
  if (!out || *out || fd < 0 || !quality || quality > 100U) return -EINVAL;
  stream_t *s = calloc(1U, sizeof(*s));
  if (!s) return -ENOMEM;
  s->fd=fd; s->flags=-1; s->width=width; s->height=height; s->quality=quality;
  s->flags=fcntl(fd, F_GETFL);
  if (s->flags < 0 || fcntl(fd, F_SETFL, s->flags | O_NONBLOCK) < 0) {
    const int err=-errno; close_stream(s); return err;
  }
  const auris_output_backend_t backend = {start, present, close_stream};
  return auris_output_open_backend(out, width, height, s, &backend);
}
