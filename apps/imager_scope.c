#include "auris_activity.h"
#include "auris_alsa.h"
#include "auris_array_configs.h"
#include "auris_camera.h"
#include "auris_output.h"
#include "auris_overlay.h"
#include "auris_srp.h"
#include "auris_timeline.h"
#include "imager_calibration.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
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
#define MAX_AGE_US 250000U
#define MAX_SKEW_US 100000U
#define AUDIO_HISTORY 32U

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
  bool evidence;
  uint64_t published_us, sample_us, dsp_us;
} snapshot_t;

typedef struct {
  pthread_mutex_t mutex;
  atomic_bool stop, failed;
  snapshot_t history[AUDIO_HISTORY];
  size_t history_next, history_count;
  const char *device;
  const auris_array_t *array;
  auris_fft_t *fft;
  auris_srp_t *srp;
  auris_preprocess_t prep;
  auris_activity_t activity;
  const auris_vec3f_t *directions;
  imager_calibration_capture_t collection;
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
  uint64_t last_audio = now_us(), next_report = 0U, evidence_us = 0U;
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
    /* Estimate the analysis-centre time using the capture delay after read.
       This is a host/ALSA estimate, not an ADC hardware timestamp. */
    const uint64_t query_start = now_us();
    snd_pcm_sframes_t queued, delayed;
    const int timing = snd_pcm_avail_delay(capture.pcm, &queued, &delayed);
    const uint64_t query_end = now_us();
    uint64_t centre_us = 0U;
    if (timing == 0 && delayed >= 0 && delayed < (snd_pcm_sframes_t)AURIS_ALSA_RATE &&
        query_end - query_start <= 5000U) {
      const uint64_t behind = block.first_frame + block.frames -
                              (analysis.first_frame + AURIS_ANALYSIS_SIZE / 2U);
      const uint64_t offset = ((uint64_t)delayed + behind) * 1000000U / AURIS_ALSA_RATE;
      const uint64_t at = query_start + (query_end - query_start) / 2U;
      if (at > offset) centre_us = at - offset;
    }
    const uint64_t dsp_start = now_us();
    auris_activity_result_t gate;
    err = -EIO;
    if (auris_preprocess_apply(&a->prep, &analysis) != AURIS_OK ||
        auris_spectrum_apply(a->fft, &analysis, &spectrum) != AURIS_OK ||
        auris_activity_apply(&a->activity, &spectrum, &gate) != AURIS_OK) break;
    if (gate.active && gate.evidence &&
        auris_srp_apply(a->srp, a->fft, &spectrum, &result) != AURIS_OK) break;
    if (gate.active && gate.evidence) evidence_us = centre_us;
    if (!gate.active || !result.valid) {
      result.valid = false;
      evidence_us = 0U;
      auris_srp_reset(a->srp);
    }
    snapshot_t next = {.best = result.best, .valid = result.valid,
                       .evidence = gate.active && gate.evidence && result.valid,
                       .published_us = now_us(), .sample_us = evidence_us};
    next.dsp_us = next.published_us - dsp_start;
    if (next.valid) memcpy(next.scores, result.scores, sizeof(next.scores));
    (void)pthread_mutex_lock(&a->mutex);
    a->history[a->history_next] = next;
    a->history_next = (a->history_next + 1U) % AUDIO_HISTORY;
    if (a->history_count < AUDIO_HISTORY) ++a->history_count;
    const size_t col = result.best % COLUMNS, row = result.best / COLUMNS;
    const bool usable = next.evidence && result.contrast >= 0.05f &&
                         col > 0U && col + 1U < COLUMNS && row > 0U && row + 1U < ROWS;
    imager_calibration_feed(&a->collection, a->directions[result.best], centre_us,
                            next.published_us, usable);
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

static snapshot_t match_audio(audio_t *audio, uint64_t camera_us,
                              uint64_t composed_us, bool monotonic) {
  snapshot_t match = {0};
  if (!monotonic || camera_us == 0U || camera_us > composed_us) return match;
  (void)pthread_mutex_lock(&audio->mutex);
  auris_timeline_point_t points[AUDIO_HISTORY];
  for (size_t i = 0U; i < audio->history_count; ++i) {
    const snapshot_t *candidate = &audio->history[i];
    points[i] = (auris_timeline_point_t){candidate->sample_us,
                                         candidate->published_us,
                                         candidate->valid && candidate->evidence};
  }
  const size_t selected = auris_timeline_match(
      points, audio->history_count, camera_us, composed_us, MAX_AGE_US,
      MAX_SKEW_US);
  if (selected != SIZE_MAX) match = audio->history[selected];
  (void)pthread_mutex_unlock(&audio->mutex);
  return match;
}

static int controls(imager_calibration_t *session, audio_t *audio) {
  static char line[128];
  static size_t used = 0U;
  static bool overflow = false;
  char buffer[256];
  const ssize_t bytes = read(STDIN_FILENO, buffer, sizeof(buffer));
  if (bytes == 0) { running = 0; return 0; }
  if (bytes < 0)
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -errno;
  for (ssize_t i = 0; i < bytes; ++i) {
    const char ch = buffer[i];
    if (ch == '\n') {
      if (overflow) fprintf(stderr, "CAL command too long\n");
      else {
        line[used] = '\0';
        if (strcmp(line, "quit") == 0) { running = 0; return 0; }
        imager_calibration_capture_t collection;
        (void)pthread_mutex_lock(&audio->mutex);
        collection = audio->collection;
        (void)pthread_mutex_unlock(&audio->mutex);
        const bool pending = session->pending;
        imager_calibration_command(session, line, now_us(), &collection);
        /* Only a new/cancelled request changes the worker's capture state.
           Solving and table rebuilding happen outside the audio mutex. */
        if (session->pending != pending) {
          (void)pthread_mutex_lock(&audio->mutex);
          audio->collection = collection;
          (void)pthread_mutex_unlock(&audio->mutex);
        }
      }
      used = 0U; overflow = false;
    } else if (ch != '\r') {
      if (used + 1U < sizeof(line) && !overflow) line[used++] = ch;
      else overflow = true;
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  bool calibrating = false;
  unsigned long fps = 15U;
  const char *device = NULL, *calibration_path = "configs/camera_calibration.txt";
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--calibrate") == 0) calibrating = true;
    else if (strcmp(argv[i], "--calibration") == 0 && i + 1 < argc)
      calibration_path = argv[++i];
    else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
      char *end = NULL;
      errno = 0;
      fps = strtoul(argv[++i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0' || fps < 1U || fps > 30U) {
        fprintf(stderr, "usage: %s [--calibrate] [--calibration FILE] [--fps 1..30] ALSA_DEVICE\n", argv[0]);
        return EXIT_FAILURE;
      }
    }
    else if (argv[i][0] != '-' && device == NULL) device = argv[i];
    else { device = NULL; break; }
  }
  if (device == NULL) {
    fprintf(stderr, "usage: %s [--calibrate] [--calibration FILE] [--fps 1..30] ALSA_DEVICE\n", argv[0]);
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
  auris_view_t camera_view = {
      .width = 1640U, .height = 1232U, .fx = 1358.5007f, .fy = 1356.8622f,
      .cx = 819.5f, .cy = 615.5f,
      .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  const auris_camera_config_t capture = {
      .sensor = "/dev/v4l-subdev2", .bridge = "/dev/v4l-subdev1",
      .device = "/dev/v4l/by-path/platform-4510000.ticsi2rx-video-index0",
      .width = 1640U, .height = 1232U, .ti_5_10_packed = true};
  const auris_image_config_t processing = {
      .source_width = 1640U, .source_height = 1232U,
      .width = 640U, .height = 360U, .black = 16.0f,
      .gain = {1.0f, 1.0f, 1.0f}, .exposure_ev = 0.0f, .gamma = 2.2f,
      .colour_matrix = {{2.18174f, -0.70887f, -0.47287f},
                        {-0.70196f, 2.76426f, -1.06231f},
                        {-0.25157f, -0.71978f, 1.97135f}},
      .auto_white_balance = true, .auto_exposure = true};
  audio_t audio = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                   .device = device, .array = auris_config_respeaker_usb_4()};
  atomic_init(&audio.stop, false);
  atomic_init(&audio.failed, false);
  auris_camera_t *camera = NULL;
  auris_image_t *image = NULL;
  auris_output_t *output_stream = NULL;
  imager_calibration_t session = {.path = calibration_path};
  uint8_t *rgb = NULL;
  pthread_t thread;
  bool started = false;
  int result = EXIT_FAILURE, err = 0, stdin_flags = -1;
  session.array_id = auris_calibration_array_id(audio.array);
  err = auris_camera_calibration_load(calibration_path, session.array_id, &camera_view);
  if (err == -ENOENT) {
    fprintf(stderr, "CAL no saved rotation; using nominal alignment\n");
    err = 0;
  } else if (err < 0) goto done;
  else fprintf(stderr, "CAL loaded %s\n", calibration_path);
  session.baseline = camera_view;
  if (calibrating) {
    stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (stdin_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK) < 0) {
      err = -errno; goto done;
    }
  }
  auris_vec3f_t directions[DIRECTIONS];
  if (auris_view_grid(&search, COLUMNS, ROWS, directions, DIRECTIONS) != AURIS_OK)
    goto done;
  audio.directions = directions;
  audio.fft = auris_fft_create();
  audio.srp = auris_srp_create(audio.array, AURIS_ALSA_RATE, directions, DIRECTIONS);
  if (audio.fft == NULL || audio.srp == NULL) goto done;
  if (auris_srp_set_grid(audio.srp, COLUMNS, ROWS, 1U) != AURIS_OK ||
      auris_srp_set_smoothing(audio.srp, calibrating ? 0.0f : 0.100f) != AURIS_OK) goto done;
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
  session.drawing = drawing;
  err = auris_overlay_open(&session.overlay, &drawing);
  if (err < 0) goto done;
  const size_t bytes = (size_t)processing.width * processing.height * 3U;
  rgb = malloc(bytes);
  if (rgb == NULL) { err = -ENOMEM; goto done; }
  auris_rgb24_t canvas = {rgb, processing.width, processing.height,
                          (size_t)processing.width * 3U, bytes};
  err = auris_output_open_jpeg_stream(&output_stream, STDOUT_FILENO,
                                      canvas.width, canvas.height, 80U);
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

  uint64_t next_output = 0U, last_camera = now_us(), next_timing = 0U;
  const uint64_t output_period_us = 1000000U / fps;
  while (running && !atomic_load(&audio.failed)) {
    err = auris_output_error(output_stream);
    if (err < 0) goto done;
    if (calibrating) {
      err = controls(&session, &audio);
      if (err < 0) goto done;
      if (!running) break;
      if (session.pending) {
        imager_calibration_capture_t collected;
        bool finished = false;
        (void)pthread_mutex_lock(&audio.mutex);
        if (audio.collection.count == CAL_CAPTURE_SAMPLES ||
            now_us() >= audio.collection.deadline_us) {
          collected = audio.collection; audio.collection.active = false; finished = true;
        }
        (void)pthread_mutex_unlock(&audio.mutex);
        if (finished) imager_calibration_finish(&session, &collected);
      }
    }
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

    const uint64_t composed = now_us();
    const snapshot_t latest = match_audio(&audio, frame.timestamp_us, composed, monotonic);
    const uint64_t skew = latest.sample_us == 0U ? 0U :
        (frame.timestamp_us > latest.sample_us ? frame.timestamp_us - latest.sample_us
                                               : latest.sample_us - frame.timestamp_us);
    const bool visible = latest.valid;
    err = auris_overlay_apply(session.overlay, latest.scores, DIRECTIONS, latest.best,
                               visible, &canvas);
    if (err < 0) goto done;
    uint32_t samples = 0U;
    if (calibrating) {
      (void)pthread_mutex_lock(&audio.mutex);
      samples = audio.collection.active ? (uint32_t)audio.collection.count : 0U;
      (void)pthread_mutex_unlock(&audio.mutex);
    }
    const auris_output_frame_t metadata = {
        .sequence = frame.sequence, .camera_us = frame.timestamp_us,
        .flags = frame.flags, .audio_us = latest.sample_us,
        .composed_us = composed, .published_us = latest.published_us,
        .visible = visible, .evidence = latest.evidence, .samples = samples,
        .fits = (uint32_t)session.count, .checks = (uint32_t)session.check_count,
        .dsp_us = latest.dsp_us};
    err = auris_output_submit(output_stream, &canvas, &metadata);
    if (err < 0) goto done;
    if (next_output == 0U)
      next_output = received;
    do {
      next_output += output_period_us;
    } while (next_output <= received);
    if (calibrating)
      imager_calibration_frame(&session, frame.sequence, frame.timestamp_us, monotonic);
    if (composed >= next_timing) {
      auris_output_stats_t stats;
      auris_output_stats(output_stream, &stats);
      fprintf(stderr, "TIMING camera %.1f ms | matched map %.1f ms | skew %.1f ms | DSP %.2f ms | JPEG %.2f ms/%zu B | write %.2f ms | dropped %llu | overlay %s\n",
          monotonic && composed >= frame.timestamp_us ?
            (double)(composed - frame.timestamp_us)/1000.0 : -1.0,
          latest.sample_us != 0U && composed >= latest.sample_us ?
            (double)(composed - latest.sample_us)/1000.0 : -1.0,
          monotonic && latest.sample_us != 0U ? (double)skew/1000.0 : -1.0,
          (double)latest.dsp_us/1000.0, (double)stats.encode_us/1000.0,
          stats.encoded_bytes, (double)stats.write_us/1000.0,
          (unsigned long long)stats.dropped,
          visible ? "visible":"hidden");
      next_timing = composed + 1000000U;
    }
  }
  result = atomic_load(&audio.failed) ? EXIT_FAILURE : EXIT_SUCCESS;
done:
  atomic_store(&audio.stop, true);
  if (started) (void)pthread_join(thread, NULL);
  if (atomic_load(&audio.failed)) result = EXIT_FAILURE;
  if (err < 0 && running) fprintf(stderr, "imager: %s\n", strerror(-err));
  else if (result != EXIT_SUCCESS && running && !atomic_load(&audio.failed))
    fprintf(stderr, "imager setup/processing failed\n");
  if (output_stream != NULL) {
    auris_output_stats_t stats;
    auris_output_stats(output_stream, &stats);
    fprintf(stderr, "stream %lu fps: submitted %llu, sent %llu, dropped %llu\n",
            fps,
            (unsigned long long)stats.submitted,
            (unsigned long long)stats.sent,
            (unsigned long long)stats.dropped);
  }
  auris_output_close(output_stream);
  auris_camera_close(camera);
  auris_overlay_close(session.overlay);
  auris_image_close(image);
  free(rgb);
  auris_srp_destroy(audio.srp);
  auris_fft_destroy(audio.fft);
  (void)pthread_mutex_destroy(&audio.mutex);
  if (stdin_flags >= 0) (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
  return running ? result : EXIT_SUCCESS;
}
