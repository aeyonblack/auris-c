#include "auris_output.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct auris_output {
  uint32_t width, height;
  uint8_t *pending_rgb, *worker_rgb;
  auris_output_frame_t frame;
  auris_output_backend_t backend;
  void *context;
  pthread_mutex_t mutex;
  pthread_cond_t ready;
  pthread_t thread;
  bool mutex_ready, cond_ready, thread_ready, pending, processing;
  atomic_bool stop;
  atomic_int error;
  auris_output_stats_t stats;
};

static bool cancelled(void *context) {
  auris_output_t *s = context;
  return atomic_load(&s->stop);
}

static void *worker(void *context) {
  auris_output_t *s = context;
  int err = s->backend.start ? s->backend.start(s->context, cancelled, s) : 0;
  while (err == 0) {
    (void)pthread_mutex_lock(&s->mutex);
    while (!s->pending && !cancelled(s))
      (void)pthread_cond_wait(&s->ready, &s->mutex);
    if (cancelled(s)) {
      (void)pthread_mutex_unlock(&s->mutex);
      break;
    }
    uint8_t *swap = s->worker_rgb;
    s->worker_rgb = s->pending_rgb;
    s->pending_rgb = swap;
    const auris_output_frame_t frame = s->frame;
    auris_output_stats_t measured = s->stats;
    s->pending = false;
    s->processing = true;
    (void)pthread_mutex_unlock(&s->mutex);
    const auris_rgb24_t rgb = {s->worker_rgb, s->width, s->height,
                               (size_t)s->width * 3U,
                               (size_t)s->width * s->height * 3U};
    err = s->backend.present(s->context, &rgb, &frame, &measured, cancelled, s);
    (void)pthread_mutex_lock(&s->mutex);
    s->stats.encode_us = measured.encode_us;
    s->stats.write_us = measured.write_us;
    s->stats.encoded_bytes = measured.encoded_bytes;
    if (err == 0) ++s->stats.sent;
    s->processing = false;
    if (err < 0 && err != -ECANCELED) atomic_store(&s->error, err);
    (void)pthread_cond_broadcast(&s->ready);
    (void)pthread_mutex_unlock(&s->mutex);
  }
  if (err < 0 && err != -ECANCELED) atomic_store(&s->error, err);
  (void)pthread_mutex_lock(&s->mutex);
  (void)pthread_cond_broadcast(&s->ready);
  (void)pthread_mutex_unlock(&s->mutex);
  return NULL;
}

int auris_output_open_backend(auris_output_t **out, uint32_t width,
                              uint32_t height, void *context,
                              const auris_output_backend_t *backend) {
  if (!out || *out || !backend || !backend->present || !backend->close ||
      !width || !height || width > 4096U || height > 4096U) {
    if (backend && backend->close) backend->close(context);
    return -EINVAL;
  }
  auris_output_t *s = calloc(1U, sizeof(*s));
  if (!s) { backend->close(context); return -ENOMEM; }
  s->width = width; s->height = height;
  s->context = context; s->backend = *backend;
  atomic_init(&s->stop, false);
  atomic_init(&s->error, 0);
  const size_t bytes = (size_t)width * height * 3U;
  s->pending_rgb = malloc(bytes); s->worker_rgb = malloc(bytes);
  int err = ENOMEM;
  if (!s->pending_rgb || !s->worker_rgb) goto fail;
  err = pthread_mutex_init(&s->mutex, NULL);
  if (err) goto fail;
  s->mutex_ready = true;
  pthread_condattr_t attr;
  err = pthread_condattr_init(&attr);
  if (err) goto fail;
  err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (!err) err = pthread_cond_init(&s->ready, &attr);
  (void)pthread_condattr_destroy(&attr);
  if (err) goto fail;
  s->cond_ready = true;
  err = pthread_create(&s->thread, NULL, worker, s);
  if (err) goto fail;
  s->thread_ready = true;
  *out = s;
  return 0;
fail:
  auris_output_close(s);
  return -err;
}

int auris_output_submit(auris_output_t *s, const auris_rgb24_t *image,
                        const auris_output_frame_t *frame) {
  if (!s || !image || !frame || !image->data ||
      image->width != s->width || image->height != s->height) return -EINVAL;
  const size_t row = (size_t)s->width * 3U;
  if (image->stride < row || image->bytes < row ||
      (size_t)(s->height - 1U) > (image->bytes - row) / image->stride)
    return -EINVAL;
  int err = atomic_load(&s->error);
  if (err < 0) return err;
  (void)pthread_mutex_lock(&s->mutex);
  if (cancelled(s)) err = -ECANCELED;
  else {
    if (s->pending) ++s->stats.dropped;
    for (uint32_t y = 0U; y < s->height; ++y)
      memcpy(s->pending_rgb + (size_t)y * row,
             image->data + (size_t)y * image->stride, row);
    s->frame = *frame; s->pending = true; ++s->stats.submitted;
    (void)pthread_cond_signal(&s->ready);
  }
  (void)pthread_mutex_unlock(&s->mutex);
  return err;
}

int auris_output_flush(auris_output_t *s, unsigned int timeout_ms) {
  if (!s) return -EINVAL;
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline)) return -errno;
  deadline.tv_sec += (time_t)(timeout_ms / 1000U);
  deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L;
  }
  (void)pthread_mutex_lock(&s->mutex);
  int err = 0;
  while ((s->pending || s->processing) && !err && !atomic_load(&s->error))
    err = pthread_cond_timedwait(&s->ready, &s->mutex, &deadline);
  const int failed = atomic_load(&s->error);
  (void)pthread_mutex_unlock(&s->mutex);
  return failed < 0 ? failed : -err;
}

int auris_output_error(const auris_output_t *s) {
  return s ? atomic_load(&s->error) : -EINVAL;
}

void auris_output_stats(const auris_output_t *s, auris_output_stats_t *stats) {
  if (!stats) return;
  *stats = (auris_output_stats_t){0};
  if (!s) return;
  auris_output_t *mutable = (auris_output_t *)s;
  (void)pthread_mutex_lock(&mutable->mutex);
  *stats = s->stats;
  (void)pthread_mutex_unlock(&mutable->mutex);
}

void auris_output_close(auris_output_t *s) {
  if (!s) return;
  atomic_store(&s->stop, true);
  if (s->mutex_ready) {
    (void)pthread_mutex_lock(&s->mutex);
    if (s->cond_ready) (void)pthread_cond_broadcast(&s->ready);
    (void)pthread_mutex_unlock(&s->mutex);
  }
  if (s->thread_ready) (void)pthread_join(s->thread, NULL);
  s->backend.close(s->context);
  if (s->cond_ready) (void)pthread_cond_destroy(&s->ready);
  if (s->mutex_ready) (void)pthread_mutex_destroy(&s->mutex);
  free(s->pending_rgb); free(s->worker_rgb); free(s);
}
