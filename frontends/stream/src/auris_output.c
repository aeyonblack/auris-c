#include "auris_output.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <jpeglib.h>

typedef struct {
  struct jpeg_error_mgr base;
  jmp_buf jump;
} jpeg_error_t;

struct auris_output {
  int fd, original_flags;
  uint32_t width, height;
  unsigned int quality;
  size_t bytes;
  uint8_t *pending_rgb, *worker_rgb;
  auris_output_frame_t pending_frame;
  pthread_mutex_t mutex;
  pthread_cond_t ready;
  pthread_t thread;
  bool mutex_ready, cond_ready, thread_ready, pending, processing, stopping;
  atomic_int error;
  atomic_uint_fast64_t submitted, sent, dropped;
  atomic_uint_fast64_t encode_us, write_us, encoded_bytes;
};

static uint64_t now_us(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0U;
  return (uint64_t)time.tv_sec * 1000000U + (uint64_t)time.tv_nsec / 1000U;
}

static void jpeg_fail(j_common_ptr common) {
  jpeg_error_t *error = (jpeg_error_t *)common->err;
  longjmp(error->jump, 1);
}

static int encode(const auris_output_t *output, const uint8_t *rgb,
                  unsigned char **data, unsigned long *bytes) {
  struct jpeg_compress_struct jpeg = {0};
  jpeg_error_t error;
  bool created = false;
  *data = NULL;
  *bytes = 0UL;
  jpeg.err = jpeg_std_error(&error.base);
  error.base.error_exit = jpeg_fail;
  if (setjmp(error.jump) != 0) {
    if (created) jpeg_destroy_compress(&jpeg);
    free(*data);
    *data = NULL;
    *bytes = 0UL;
    return -EIO;
  }
  jpeg_create_compress(&jpeg);
  created = true;
  jpeg_mem_dest(&jpeg, data, bytes);
  jpeg.image_width = output->width;
  jpeg.image_height = output->height;
  jpeg.input_components = 3;
  jpeg.in_color_space = JCS_RGB;
  jpeg_set_defaults(&jpeg);
  jpeg_set_quality(&jpeg, (int)output->quality, TRUE);
  jpeg_start_compress(&jpeg, TRUE);
  while (jpeg.next_scanline < jpeg.image_height) {
    JSAMPROW row = (JSAMPROW)(rgb + (size_t)jpeg.next_scanline *
                                     (size_t)output->width * 3U);
    (void)jpeg_write_scanlines(&jpeg, &row, 1U);
  }
  jpeg_finish_compress(&jpeg);
  jpeg_destroy_compress(&jpeg);
  return 0;
}

static bool stopping(auris_output_t *output) {
  bool value;
  (void)pthread_mutex_lock(&output->mutex);
  value = output->stopping;
  (void)pthread_mutex_unlock(&output->mutex);
  return value;
}

/* A slow consumer is allowed to apply backpressure to this worker forever.
   The capture thread remains independent and replaces the pending frame. */
static int write_all(auris_output_t *output, const void *data, size_t bytes) {
  const uint8_t *cursor = data;
  while (bytes != 0U) {
    if (stopping(output)) return -ECANCELED;
    const ssize_t count = write(output->fd, cursor, bytes);
    if (count > 0) {
      cursor += (size_t)count;
      bytes -= (size_t)count;
      continue;
    }
    if (count == 0) return -EIO;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
    struct pollfd pollfd = {.fd = output->fd, .events = POLLOUT};
    const int ready = poll(&pollfd, 1U, 100);
    if (ready < 0 && errno != EINTR) return -errno;
  }
  return 0;
}

static int send_frame(auris_output_t *output,
                      const auris_output_frame_t *frame,
                      const uint8_t *rgb) {
  unsigned char *jpeg = NULL;
  unsigned long jpeg_bytes = 0UL;
  const uint64_t encode_start = now_us();
  int err = encode(output, rgb, &jpeg, &jpeg_bytes);
  const uint64_t encoded = now_us();
  if (err < 0) return err;
  char header[512];
  const int length = snprintf(
      header, sizeof(header),
      "FRAME %" PRIu32 " %" PRIu64 " %" PRIu32 " %lu %" PRIu64
      " %" PRIu64 " %" PRIu64 " %u %u %" PRIu32 " %" PRIu32
      " %" PRIu32 " %" PRIu64 " %" PRIuFAST64 "\n",
      frame->sequence, frame->camera_us, frame->flags, jpeg_bytes,
      frame->audio_us, frame->composed_us, frame->published_us,
      frame->visible ? 1U : 0U, frame->evidence ? 1U : 0U,
      frame->samples, frame->fits, frame->checks, frame->dsp_us,
      atomic_load(&output->dropped));
  if (length < 0 || (size_t)length >= sizeof(header)) {
    free(jpeg);
    return -EOVERFLOW;
  }
  const uint64_t write_start = now_us();
  err = write_all(output, header, (size_t)length);
  if (err == 0) err = write_all(output, jpeg, (size_t)jpeg_bytes);
  const uint64_t written = now_us();
  free(jpeg);
  atomic_store(&output->encode_us, encoded - encode_start);
  atomic_store(&output->write_us, written - write_start);
  atomic_store(&output->encoded_bytes, (uint64_t)jpeg_bytes);
  if (err == 0) (void)atomic_fetch_add(&output->sent, 1U);
  return err;
}

static void *worker(void *context) {
  auris_output_t *output = context;
  char header[96];
  const int length = snprintf(header, sizeof(header),
                              "AURISJPEG1 %" PRIu32 " %" PRIu32 "\n",
                              output->width, output->height);
  int err = length < 0 || (size_t)length >= sizeof(header)
                ? -EOVERFLOW
                : write_all(output, header, (size_t)length);
  while (err == 0) {
    (void)pthread_mutex_lock(&output->mutex);
    while (!output->pending && !output->stopping)
      (void)pthread_cond_wait(&output->ready, &output->mutex);
    if (output->stopping) {
      (void)pthread_mutex_unlock(&output->mutex);
      break;
    }
    uint8_t *swap = output->worker_rgb;
    output->worker_rgb = output->pending_rgb;
    output->pending_rgb = swap;
    const auris_output_frame_t frame = output->pending_frame;
    output->pending = false;
    output->processing = true;
    (void)pthread_mutex_unlock(&output->mutex);
    err = send_frame(output, &frame, output->worker_rgb);
    if (err < 0 && err != -ECANCELED) atomic_store(&output->error, err);
    (void)pthread_mutex_lock(&output->mutex);
    output->processing = false;
    (void)pthread_cond_broadcast(&output->ready);
    (void)pthread_mutex_unlock(&output->mutex);
  }
  if (err < 0 && err != -ECANCELED && atomic_load(&output->error) == 0)
    atomic_store(&output->error, err);
  (void)pthread_mutex_lock(&output->mutex);
  (void)pthread_cond_broadcast(&output->ready);
  (void)pthread_mutex_unlock(&output->mutex);
  return NULL;
}

int auris_output_open_jpeg_stream(auris_output_t **out, int fd,
                                  uint32_t width, uint32_t height,
                                  unsigned int quality) {
  if (out == NULL || *out != NULL || fd < 0 || width == 0U || height == 0U ||
      width > 4096U || height > 4096U || quality == 0U || quality > 100U ||
      (uint64_t)width * height * 3U > SIZE_MAX)
    return -EINVAL;
  auris_output_t *output = calloc(1U, sizeof(*output));
  if (output == NULL) return -ENOMEM;
  output->fd = fd;
  output->original_flags = -1;
  output->width = width;
  output->height = height;
  output->quality = quality;
  output->bytes = (size_t)width * height * 3U;
  output->pending_rgb = malloc(output->bytes);
  output->worker_rgb = malloc(output->bytes);
  if (output->pending_rgb == NULL || output->worker_rgb == NULL) {
    auris_output_close(output);
    return -ENOMEM;
  }
  int rc = pthread_mutex_init(&output->mutex, NULL);
  if (rc != 0) {
    auris_output_close(output);
    return -rc;
  }
  output->mutex_ready = true;
  rc = pthread_cond_init(&output->ready, NULL);
  if (rc != 0) {
    auris_output_close(output);
    return -rc;
  }
  output->cond_ready = true;
  atomic_init(&output->error, 0);
  atomic_init(&output->submitted, 0U);
  atomic_init(&output->sent, 0U);
  atomic_init(&output->dropped, 0U);
  atomic_init(&output->encode_us, 0U);
  atomic_init(&output->write_us, 0U);
  atomic_init(&output->encoded_bytes, 0U);
  output->original_flags = fcntl(fd, F_GETFL);
  if (output->original_flags < 0 ||
      fcntl(fd, F_SETFL, output->original_flags | O_NONBLOCK) < 0) {
    const int err = -errno;
    auris_output_close(output);
    return err;
  }
  rc = pthread_create(&output->thread, NULL, worker, output);
  if (rc != 0) {
    auris_output_close(output);
    return -rc;
  }
  output->thread_ready = true;
  *out = output;
  return 0;
}

int auris_output_flush(auris_output_t *output, unsigned int timeout_ms) {
  if (output == NULL) return -EINVAL;
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -errno;
  deadline.tv_sec += (time_t)(timeout_ms / 1000U);
  deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= 1000000000L;
  }
  (void)pthread_mutex_lock(&output->mutex);
  int err = 0;
  while ((output->pending || output->processing) && err == 0 &&
         atomic_load(&output->error) == 0)
    err = pthread_cond_timedwait(&output->ready, &output->mutex, &deadline);
  const int worker_error = atomic_load(&output->error);
  (void)pthread_mutex_unlock(&output->mutex);
  if (worker_error < 0) return worker_error;
  return err == ETIMEDOUT ? -ETIMEDOUT : err == 0 ? 0 : -err;
}

int auris_output_submit(auris_output_t *output, const auris_rgb24_t *image,
                        const auris_output_frame_t *frame) {
  if (output == NULL || image == NULL || frame == NULL || image->data == NULL ||
      image->width != output->width || image->height != output->height ||
      image->stride < (size_t)output->width * 3U ||
      image->bytes < (size_t)(output->height - 1U) * image->stride +
                         (size_t)output->width * 3U)
    return -EINVAL;
  const int prior = atomic_load(&output->error);
  if (prior < 0) return prior;
  (void)pthread_mutex_lock(&output->mutex);
  if (output->stopping) {
    (void)pthread_mutex_unlock(&output->mutex);
    return -ECANCELED;
  }
  if (output->pending) (void)atomic_fetch_add(&output->dropped, 1U);
  for (uint32_t y = 0U; y < output->height; ++y)
    memcpy(output->pending_rgb + (size_t)y * output->width * 3U,
           image->data + (size_t)y * image->stride,
           (size_t)output->width * 3U);
  output->pending_frame = *frame;
  output->pending = true;
  (void)atomic_fetch_add(&output->submitted, 1U);
  (void)pthread_cond_signal(&output->ready);
  (void)pthread_mutex_unlock(&output->mutex);
  return 0;
}

int auris_output_error(const auris_output_t *output) {
  return output == NULL ? -EINVAL : atomic_load(&output->error);
}

void auris_output_stats(const auris_output_t *output,
                        auris_output_stats_t *stats) {
  if (stats == NULL) return;
  *stats = (auris_output_stats_t){0};
  if (output == NULL) return;
  stats->submitted = atomic_load(&output->submitted);
  stats->sent = atomic_load(&output->sent);
  stats->dropped = atomic_load(&output->dropped);
  stats->encode_us = atomic_load(&output->encode_us);
  stats->write_us = atomic_load(&output->write_us);
  stats->encoded_bytes = (size_t)atomic_load(&output->encoded_bytes);
}

void auris_output_close(auris_output_t *output) {
  if (output == NULL) return;
  if (output->mutex_ready) {
    (void)pthread_mutex_lock(&output->mutex);
    output->stopping = true;
    if (output->cond_ready) (void)pthread_cond_signal(&output->ready);
    (void)pthread_mutex_unlock(&output->mutex);
  }
  if (output->thread_ready) (void)pthread_join(output->thread, NULL);
  if (output->original_flags >= 0)
    (void)fcntl(output->fd, F_SETFL, output->original_flags);
  if (output->cond_ready) (void)pthread_cond_destroy(&output->ready);
  if (output->mutex_ready) (void)pthread_mutex_destroy(&output->mutex);
  free(output->pending_rgb);
  free(output->worker_rgb);
  free(output);
}
