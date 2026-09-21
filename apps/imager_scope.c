#include "auris_activity.h"
#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_camera.h"
#include "auris_overlay.h"
#include "auris_srp.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COLUMNS 65U
#define ROWS 49U
#define DIRECTIONS (COLUMNS * ROWS)
#define OUTPUT_PERIOD_US 200000U
#define MAX_AGE_US 250000U

_Static_assert(DIRECTIONS <= AURIS_SRP_MAX_DIRECTIONS, "grid too large");

static volatile sig_atomic_t running = 1;
static void stop(int number) { (void)number; running = 0; }

static uint64_t now_us(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) abort();
  return (uint64_t)t.tv_sec * 1000000U + (uint64_t)t.tv_nsec / 1000U;
}

typedef struct {
  float scores[DIRECTIONS];
  size_t best;
  bool valid;
  uint64_t published_us;
} snapshot_t;

typedef struct {
  pthread_mutex_t mutex;
  atomic_bool stop, failed;
  snapshot_t latest;
  const char *device;
  const auris_array_t *array;
  auris_fft_t *fft;
  auris_srp_t *srp;
  auris_preprocess_t prep;
  auris_activity_t activity;
} audio_t;

static void *audio_worker(void *context) {
  audio_t *a = context;
  auris_alsa_t capture = {0};
  auris_audio_source_t source = {&capture, auris_alsa_read};
  int err = auris_alsa_open(&capture, a->device, a->array->capture_channels,
                           a->array->microphone_count);
  if (err < 0) goto done;
  /* A stalled device must not trap shutdown inside a blocking read. */
  err = snd_pcm_nonblock(capture.pcm, 1);
  if (err < 0) goto done;
  auris_framer_t framer;
  auris_framer_reset(&framer);
  auris_audio_block_t block;
  auris_analysis_frame_t analysis;
  auris_spectrum_t spectrum;
  auris_srp_result_t result = {0};
  uint64_t last_audio = now_us(), next_report = 0U;
  while (!atomic_load(&a->stop)) {
    err = snd_pcm_wait(capture.pcm, 100);
    if (err == -EINTR) continue;
    if (err < 0) break;
    if (now_us() - last_audio > 1000000U) { err = -ETIMEDOUT; break; }
    if (err == 0) continue;
    const snd_pcm_sframes_t available = snd_pcm_avail_update(capture.pcm);
    if (available < 0) { err = (int)available; break; }
    if (available < (snd_pcm_sframes_t)AURIS_AUDIO_FRAMES) {
      const struct timespec pause = {.tv_nsec = 1000000L};
      (void)nanosleep(&pause, NULL);
      continue;
    }
    /* There is one reader and a whole block is available. Unexpected short
       reads/EAGAIN fail closed; we never silently bridge a discontinuity. */
    err = -EIO;
    if (source.read(source.context, &block) != AURIS_OK) break;
    last_audio = now_us();
    bool ready;
    if (auris_framer_push(&framer, &block, &analysis, &ready) != AURIS_OK) break;
    err = 0;
    if (!ready) continue;
    auris_activity_result_t gate;
    err = -EIO;
    if (auris_preprocess_apply(&a->prep, &analysis) != AURIS_OK ||
        auris_spectrum_apply(a->fft, &analysis, &spectrum) != AURIS_OK ||
        auris_activity_apply(&a->activity, &spectrum, &gate) != AURIS_OK) break;
    if (gate.active && gate.evidence &&
        auris_srp_apply(a->srp, a->fft, &spectrum, &result) != AURIS_OK) break;
    if (!gate.active || !result.valid) {
      result.valid = false;
      auris_srp_reset(a->srp);
    }
    snapshot_t next = {.best = result.best, .valid = result.valid,
                       .published_us = now_us()};
    if (next.valid) memcpy(next.scores, result.scores, sizeof(next.scores));
    (void)pthread_mutex_lock(&a->mutex);
    a->latest = next;
    (void)pthread_mutex_unlock(&a->mutex);
    if (next.published_us >= next_report) {
      fprintf(stderr, "audio %.1f dBFS | %s\n", (double)gate.level_dbfs,
              !gate.ready ? "calibrating" : !gate.active ? "closed" :
              gate.evidence ? "active" : "holding");
      next_report = next.published_us + 1000000U;
    }
    err = 0;
  }
done:
  if (err < 0 && !atomic_load(&a->stop)) {
    fprintf(stderr, "audio worker: %s\n", snd_strerror(err));
    atomic_store(&a->failed, true);
  }
  auris_alsa_close(&capture);
  return NULL;
}

/* This app's output frontend. A partially sent frame is never abandoned in
   favour of a newer frame: on failure we terminate the stream. */
static int send_bytes(const void *data, size_t bytes, uint64_t deadline) {
  const uint8_t *p = data;
  while (bytes != 0U && running) {
    if (now_us() >= deadline) return -ETIMEDOUT;
    const ssize_t sent = write(STDOUT_FILENO, p, bytes);
    if (sent > 0) { p += (size_t)sent; bytes -= (size_t)sent; continue; }
    if (sent == 0) return -EIO;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
    struct pollfd fd = {.fd = STDOUT_FILENO, .events = POLLOUT};
    if (poll(&fd, 1U, 50) < 0 && errno != EINTR) return -errno;
  }
  return bytes == 0U ? 0 : -EINTR;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s ALSA_DEVICE\n", argv[0]);
    return EXIT_FAILURE;
  }
  struct sigaction action = {0};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
    return EXIT_FAILURE;
  action.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &action, NULL)) return EXIT_FAILURE;

  const auris_view_t search = {
      .width = 640U, .height = 480U, .fx = 480.0f, .fy = 480.0f,
      .cx = 319.5f, .cy = 239.5f,
      .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  const auris_view_t camera_view = {
      .width = 1640U, .height = 1232U, .fx = 1358.5007f, .fy = 1356.8622f,
      .cx = 819.5f, .cy = 615.5f,
      .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  const auris_camera_config_t capture = {
      .sensor = "/dev/v4l-subdev2", .bridge = "/dev/v4l-subdev1",
      .device = "/dev/v4l/by-path/platform-4510000.ticsi2rx-video-index0",
      .width = 1640U, .height = 1232U, .ti_5_10_packed = true};
  const auris_image_config_t processing = {
      .source_width = 1640U, .source_height = 1232U,
      .width = 640U, .height = 360U, .black = 0.0f,
      .gain = {1.0f, 1.0f, 1.0f}, .exposure_ev = 0.0f, .gamma = 2.2f};
  audio_t audio = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                   .device = argv[1], .array = auris_config_respeaker_usb_4()};
  atomic_init(&audio.stop, false);
  atomic_init(&audio.failed, false);
  auris_camera_t *camera = NULL;
  auris_image_t *image = NULL;
  auris_overlay_t *overlay = NULL;
  uint8_t *rgb = NULL;
  pthread_t thread;
  bool started = false;
  int result = EXIT_FAILURE, err = 0, stdout_flags = -1;
  auris_vec3f_t directions[DIRECTIONS];
  if (auris_view_grid(&search, COLUMNS, ROWS, directions, DIRECTIONS) != AURIS_OK)
    goto done;
  audio.fft = auris_fft_create();
  audio.srp = auris_srp_create(audio.array, AURIS_ALSA_RATE, directions, DIRECTIONS);
  if (audio.fft == NULL || audio.srp == NULL) goto done;
  if (auris_srp_set_grid(audio.srp, COLUMNS, ROWS, 1U) != AURIS_OK ||
      auris_srp_set_smoothing(audio.srp, 0.100f) != AURIS_OK) goto done;
  auris_preprocess_init(&audio.prep);
  float energy = 0.0f;
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n)
    energy += audio.prep.window[n] * audio.prep.window[n];
  auris_activity_config_t gate = auris_activity_defaults();
  gate.enabled = true;
  if (auris_activity_init(&audio.activity, &gate, AURIS_ALSA_RATE,
                          audio.array->microphone_count, energy) != AURIS_OK)
    goto done;
  err = auris_camera_open(&camera, &capture);
  if (err < 0) goto done;
  const auris_camera_format_t *format = auris_camera_format(camera);
  err = auris_image_open(&image, &processing);
  if (err < 0) goto done;
  const auris_image_rect_t *rect = auris_image_content(image);
  const auris_overlay_config_t drawing = {
      .camera = camera_view, .search = search,
      .layout = {processing.width, processing.height, rect->x, rect->y,
                  rect->width, rect->height},
      .columns = COLUMNS, .rows = ROWS,
      .cutoff = 0.55f, .opacity = 0.75f, .mark_peak = true};
  err = auris_overlay_open(&overlay, &drawing);
  if (err < 0) goto done;
  const size_t bytes = (size_t)processing.width * processing.height * 3U;
  rgb = malloc(bytes);
  if (rgb == NULL) { err = -ENOMEM; goto done; }
  auris_rgb24_t canvas = {rgb, processing.width, processing.height,
                          (size_t)processing.width * 3U, bytes};
  stdout_flags = fcntl(STDOUT_FILENO, F_GETFL);
  if (stdout_flags < 0 ||
      fcntl(STDOUT_FILENO, F_SETFL, stdout_flags | O_NONBLOCK) < 0) {
    err = -errno; goto done;
  }
  char header[160];
  int length = snprintf(header, sizeof(header), "AURISCAM1 %u %u\n",
                         canvas.width, canvas.height);
  err = send_bytes(header, (size_t)length, now_us() + 1000000U);
  if (err < 0) goto done;
  err = auris_camera_start(camera);
  if (err < 0) goto done;
  /* Worker inherits blocked termination signals. Main owns the signal flag. */
  sigset_t blocked, previous;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGTERM);
  err = -pthread_sigmask(SIG_BLOCK, &blocked, &previous);
  if (err < 0) goto done;
  err = -pthread_create(&thread, NULL, audio_worker, &audio);
  started = err == 0;
  const int restored = pthread_sigmask(SIG_SETMASK, &previous, NULL);
  if (err < 0) goto done;
  if (restored != 0) { err = -restored; goto done; }

  uint64_t next_output = 0U, last_camera = now_us();
  while (running && !atomic_load(&audio.failed)) {
    auris_camera_frame_t frame;
    err = auris_camera_acquire(camera, &frame, 100);
    if (err == -EINTR || err == -EAGAIN || err == -ETIMEDOUT || err == -EBADMSG) {
      if (now_us() - last_camera > 2000000U) { err = -ETIMEDOUT; goto done; }
      continue;
    }
    if (err < 0) goto done;
    const uint64_t received = now_us();
    last_camera = received;
    const bool monotonic = (frame.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
                           V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    const bool stale = monotonic && (frame.timestamp_us > received ||
                                    received - frame.timestamp_us > MAX_AGE_US);
    const bool present = !stale && received >= next_output;
    if (present) {
      const auris_bayer8_t raw = {frame.data, format->width, format->height,
                                  format->stride, frame.bytes};
      err = auris_image_render(image, &raw, &canvas);
    }
    const int released = auris_camera_release(camera);
    if (err < 0) goto done;
    if (released < 0) { err = released; goto done; }
    if (!present) continue;

    snapshot_t latest;
    (void)pthread_mutex_lock(&audio.mutex);
    latest = audio.latest;
    (void)pthread_mutex_unlock(&audio.mutex);
    const uint64_t composed = now_us();
    const bool fresh = latest.published_us != 0U &&
        composed >= latest.published_us &&
        composed - latest.published_us <= MAX_AGE_US;
    err = auris_overlay_apply(overlay, latest.scores, DIRECTIONS, latest.best,
                               latest.valid && fresh, &canvas);
    if (err < 0) goto done;
    length = snprintf(header, sizeof(header), "FRAME %u %llu %u %zu\n",
        frame.sequence, (unsigned long long)frame.timestamp_us, frame.flags, bytes);
    const uint64_t deadline = now_us() + 1000000U;
    err = send_bytes(header, (size_t)length, deadline);
    if (err == 0) err = send_bytes(rgb, bytes, deadline);
    if (err < 0) goto done;
    next_output = now_us() + OUTPUT_PERIOD_US;
  }
  result = atomic_load(&audio.failed) ? EXIT_FAILURE : EXIT_SUCCESS;
done:
  atomic_store(&audio.stop, true);
  if (started) (void)pthread_join(thread, NULL);
  if (atomic_load(&audio.failed)) result = EXIT_FAILURE;
  if (err < 0 && running) fprintf(stderr, "imager: %s\n", strerror(-err));
  else if (result != EXIT_SUCCESS && running && !atomic_load(&audio.failed))
    fprintf(stderr, "imager setup/processing failed\n");
  auris_camera_close(camera);
  auris_overlay_close(overlay);
  auris_image_close(image);
  free(rgb);
  auris_srp_destroy(audio.srp);
  auris_fft_destroy(audio.fft);
  (void)pthread_mutex_destroy(&audio.mutex);
  if (stdout_flags >= 0) (void)fcntl(STDOUT_FILENO, F_SETFL, stdout_flags);
  return running ? result : EXIT_SUCCESS;
}
