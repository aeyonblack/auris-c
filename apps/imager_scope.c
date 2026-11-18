#include "auris_alsa.h"
#include "auris_camera.h"
#include "auris_outputs.h"
#include "auris_testmode.h"
#include "auris_ui.h"
#include "auris_wav.h"
#include "auris_overlay.h"
#include "auris_srp.h"
#include "auris_timeline.h"
#include "camera_chain.h"
#include "imager_audio.h"
#include "imager_calibration.h"
#include "imager_clip.h"
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define COLUMNS IMAGER_COLUMNS
#define ROWS IMAGER_ROWS
#define DIRECTIONS IMAGER_DIRECTIONS
#define MAX_AGE_US 250000U
#define MAX_SKEW_US 100000U

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
  float column, row; /* the estimate in grid coordinates, between cells */
  float strength;    /* 0..1 drawing strength */
  bool valid;        /* there is a map to draw */
  bool evidence;     /* this frame added evidence (not a held estimate) */
  uint64_t published_us, sample_us, dsp_us;
} snapshot_t;

typedef struct {
  pthread_mutex_t mutex;
  atomic_bool stop, failed;
  atomic_bool recalibrate; /* the worker relearns the room at its next frame */
  snapshot_t history[IMAGER_HISTORY_MAX];
  size_t history_next, history_count, history_capacity;
  imager_dsp_t dsp; /* worker-owned after start */
  const char *audio_file; /* replay this recording instead of capturing */
  const auris_vec3f_t *directions;
  imager_calibration_capture_t collection;
  auris_settings_t settings; /* Protected by mutex; DSP objects stay worker-owned. */
  auris_ui_status_t status;
  bool smoothing_applied;
  imager_clip_t *clip; /* set before the worker starts; records only in test mode */
} audio_t;

static void *audio_worker(void *context) {
  audio_t *a = context;
  const imager_audio_plan_t *plan = &a->dsp.plan;
  const auris_array_t *array = plan->config.array;
  auris_alsa_t capture = {0};
  auris_wav_reader_t recording = {0};
  auris_audio_source_t source = {&capture, auris_alsa_read};
  const bool replay = a->audio_file != NULL;
  uint64_t replay_start = 0U; /* clock time of the recording's first sample */
  int err;
  if (replay) {
    err = auris_wav_open(&recording, a->audio_file, plan->config.capture,
                         array->capture_channels, array->microphone_count);
    if (err < 0) goto done;
    source = (auris_audio_source_t){&recording, auris_wav_read};
    replay_start = now_us();
  } else {
    err = auris_alsa_open_profile(&capture, plan->config.capture, plan->device,
                                  array->capture_channels,
                                  array->microphone_count);
    if (err < 0) goto done;
    /* A stalled device must not trap shutdown inside a blocking read. */
    err = snd_pcm_nonblock(capture.pcm, 1);
    if (err < 0) goto done;
  }
  static auris_framer_t framer;
  static auris_audio_block_t block;
  static auris_analysis_frame_t analysis;
  static auris_srp_result_t result;
  err = -EINVAL;
  if (auris_framer_init(&framer, plan->frame_size, plan->frame_hop) != AURIS_OK)
    goto done;
  err = 0;
  uint64_t last_audio = now_us(), next_report = 0U;
  while (!atomic_load(&a->stop)) {
    if (replay) {
      /* A recording is played at its own pace, over and over: each block
         becomes available when its last sample would have been heard. */
      err = -EIO;
      if (source.read(source.context, &block) != AURIS_OK) break;
      if (block.frames == 0U) {
        auris_wav_close(&recording);
        err = auris_wav_open(&recording, a->audio_file, plan->config.capture,
                             array->capture_channels, array->microphone_count);
        if (err < 0) break;
        auris_framer_restart(&framer);
        replay_start = now_us();
        continue;
      }
      const uint64_t due = replay_start + (block.first_frame + block.frames) *
                                              1000000U / plan->rate;
      while (!atomic_load(&a->stop) && now_us() < due) {
        const uint64_t left = due - now_us();
        const struct timespec pause = {.tv_nsec = (long)(left > 5000U ? 5000U : left) * 1000L};
        (void)nanosleep(&pause, NULL);
      }
    } else {
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
    }
    last_audio = now_us();
    if (a->clip) {
      size_t raw_bytes = 0U;
      imager_clip_audio(a->clip, replay ? (const void *)recording.raw
                                        : auris_alsa_raw(&capture, &raw_bytes),
                        block.frames, block.first_frame);
    }
    bool ready;
    if (auris_framer_push(&framer, &block, &analysis, &ready) != AURIS_OK) break;
    err = 0;
    if (!ready) continue;
    auris_settings_t desired;
    (void)pthread_mutex_lock(&a->mutex);
    desired=a->settings;
    (void)pthread_mutex_unlock(&a->mutex);
    if (desired.gating!=a->dsp.activity.enabled || desired.smoothing!=a->smoothing_applied) {
      if (desired.gating!=a->dsp.activity.enabled) {
        a->dsp.activity.enabled=desired.gating;
        auris_activity_reset(&a->dsp.activity);
      }
      if (imager_dsp_set_smoothing(&a->dsp,desired.smoothing)!=AURIS_OK) {
        err=-EINVAL; break;
      }
      a->smoothing_applied=desired.smoothing;
      result.valid=false;
      (void)pthread_mutex_lock(&a->mutex);
      a->history_count=0U; a->history_next=0U;
      (void)pthread_mutex_unlock(&a->mutex);
    }
    if (atomic_exchange(&a->recalibrate, false)) {
      if (imager_dsp_recalibrate(&a->dsp, a->smoothing_applied) != AURIS_OK) {
        err = -EINVAL; break;
      }
      result.valid = false;
      (void)pthread_mutex_lock(&a->mutex);
      a->history_count = 0U; a->history_next = 0U;
      (void)pthread_mutex_unlock(&a->mutex);
    }
    /* Estimate the analysis-centre time using the capture delay after read.
       This is a host/ALSA estimate, not an ADC hardware timestamp. */
    uint64_t centre_us = 0U;
    const uint64_t query_start = now_us();
    snd_pcm_sframes_t queued = 0, delayed = 0;
    const int timing = replay ? -1
                              : snd_pcm_avail_delay(capture.pcm, &queued, &delayed);
    const uint64_t query_end = now_us();
    if (replay) {
      centre_us = replay_start + (analysis.first_frame + plan->frame_size / 2U) *
                                     1000000U / plan->rate;
    } else if (timing == 0 && delayed >= 0 && delayed < (snd_pcm_sframes_t)plan->rate &&
        query_end - query_start <= 5000U) {
      const uint64_t behind = block.first_frame + block.frames -
                              (analysis.first_frame + plan->frame_size / 2U);
      const uint64_t offset = ((uint64_t)delayed + behind) * 1000000U / plan->rate;
      const uint64_t at = query_start + (query_end - query_start) / 2U;
      if (at > offset) centre_us = at - offset;
    }
    const uint64_t dsp_start = now_us();
    auris_activity_result_t gate;
    err = -EIO;
    imager_view_t view;
    if (imager_dsp_process(&a->dsp, &analysis, &gate, &result, &view) != AURIS_OK) break;
    /* A held estimate is as current as the frame that kept it, so every
       drawable frame carries its own time, fresh or not. */
    snapshot_t next = {.best = view.best, .column = view.column, .row = view.row,
                       .strength = view.strength, .valid = view.valid,
                       .evidence = view.valid && view.fresh,
                       .published_us = now_us(),
                       .sample_us = view.valid ? centre_us : 0U};
    next.dsp_us = next.published_us - dsp_start;
    if (next.valid) memcpy(next.scores, view.map, sizeof(next.scores));
    (void)pthread_mutex_lock(&a->mutex);
    a->status=(auris_ui_status_t){.dbfs=gate.level_dbfs,
        .contrast=next.evidence ? result.contrast:0.0f,.ready=gate.ready,
        .active=gate.active,.evidence=gate.evidence};
    a->history[a->history_next] = next;
    a->history_next = (a->history_next + 1U) % a->history_capacity;
    if (a->history_count < a->history_capacity) ++a->history_count;
    const size_t col = view.best % COLUMNS, row = view.best / COLUMNS;
    const bool sure = a->dsp.tracking ? view.confidence >= 0.5f
                                      : result.contrast >= 0.05f;
    const bool usable = next.evidence && sure &&
                         col > 0U && col + 1U < COLUMNS && row > 0U && row + 1U < ROWS;
    imager_calibration_feed(&a->collection, a->directions[view.best], centre_us,
                            next.published_us, usable);
    (void)pthread_mutex_unlock(&a->mutex);
    if (a->clip) {
      const auris_vec3f_t way = a->directions[view.best];
      const imager_clip_row_t logged = {
          .centre_us = centre_us, .published_us = next.published_us,
          .first_frame = analysis.first_frame, .level_dbfs = gate.level_dbfs,
          .noise_dbfs = gate.noise_dbfs, .weight_sum = gate.weight_sum,
          .column = view.column, .row = view.row, .x = way.x_m, .y = way.y_m, .z = way.z_m,
          .strength = view.strength, .confidence = view.confidence,
          .contrast = next.evidence ? result.contrast : 0.0f,
          .dsp_us = (uint32_t)next.dsp_us, .ready = gate.ready, .active = gate.active,
          .evidence = gate.evidence, .valid = view.valid, .fresh = view.fresh};
      imager_clip_row(a->clip, &logged);
    }
    if (next.published_us >= next_report) {
      fprintf(stderr, "audio %.1f dBFS (noise %.1f, %.1f bins) | %s | %s\n",
              (double)gate.level_dbfs, (double)gate.noise_dbfs,
              (double)gate.weight_sum,
              !a->dsp.activity.enabled ? "gate off" :
              !gate.ready ? "calibrating" : !gate.active ? "closed" :
              gate.evidence ? "active" : "holding",
              view.valid ? "shown" : "hidden");
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
  auris_wav_close(&recording);
  return NULL;
}

static snapshot_t match_audio(audio_t *audio, uint64_t camera_us,
                              uint64_t composed_us, bool monotonic) {
  snapshot_t match = {0};
  if (!monotonic || camera_us == 0U || camera_us > composed_us) return match;
  (void)pthread_mutex_lock(&audio->mutex);
  auris_timeline_point_t points[IMAGER_HISTORY_MAX];
  for (size_t i = 0U; i < audio->history_count; ++i) {
    const snapshot_t *candidate = &audio->history[i];
    points[i] = (auris_timeline_point_t){candidate->sample_us,
                                         candidate->published_us,
                                         candidate->valid};
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

typedef struct { auris_ui_t *ui; auris_testmode_t *test; auris_display_info_t display; } touch_context_t;
static void touch_event(void *context,const auris_touch_event_t *event) {
  touch_context_t *c=context;
  auris_touch_event_t mapped=*event;
  if (!auris_touch_project(event->x,event->y,65535,65535,c->display.width,
      c->display.height,640U,360U,c->display.rotation,&mapped.x,&mapped.y))
    mapped.kind=AURIS_TOUCH_CANCEL;
  if (c->test && c->test->phase!=AURIS_TESTMODE_OFF) auris_testmode_touch(c->test,&mapped,now_us());
  else auris_ui_touch(c->ui,&mapped,now_us());
}

/* Test mode (the qualification clips). The screens are auris_testmode; this
   carries out what they ask for. */
typedef struct {
  auris_testplan_t plan;
  auris_testmode_t mode;
  imager_clip_t *clip;
  const char *plan_path, *results;
  char final[256], part[272];
  char commit[16];
  uint8_t *held;       /* the picture the review shows */
  bool have_held, hold_next, save_failed;
  uint64_t next_picture;
  const char *array;
  float pan_deg, tilt_deg, diffuse;
  auris_settings_t settings;
  atomic_bool *recalibrate;
} test_t;

#define TEST_PICTURE_PERIOD_US 200000U

static unsigned int test_free_mb(const test_t *t) {
  (void)mkdir(t->results, 0755);
  struct statvfs seen;
  if (statvfs(t->results, &seen) != 0) return 0U;
  return (unsigned int)((uint64_t)seen.f_bavail * seen.f_frsize / (1024U * 1024U));
}

/* A folder with meta.json is a finished clip, so a power cut loses only the
   clip being recorded. */
static void test_find_finished(test_t *t) {
  for (size_t l = 0U; l < t->plan.location_count; ++l)
    for (size_t c = 0U; c < t->plan.clip_count; ++c) {
      char folder[256], meta[300];
      t->mode.done[l][c] = false;
      if (auris_testplan_clip_dir(&t->plan, l, c, t->results, folder, sizeof(folder)) != 0)
        continue;
      (void)snprintf(meta, sizeof(meta), "%s/meta.json", folder);
      t->mode.done[l][c] = access(meta, F_OK) == 0;
    }
}

/* The commit the tree was on, read directly: root's git refuses this repository. */
static void test_read_commit(test_t *t) {
  char line[160] = "", path[200];
  (void)snprintf(t->commit, sizeof(t->commit), "unknown");
  FILE *f = fopen(".git/HEAD", "r");
  if (!f) return;
  const bool read = fgets(line, sizeof(line), f) != NULL;
  (void)fclose(f);
  if (!read) return;
  line[strcspn(line, "\n")] = '\0';
  if (strncmp(line, "ref: ", 5U) == 0) {
    (void)snprintf(path, sizeof(path), ".git/%s", line + 5);
    f = fopen(path, "r");
    if (!f) return;
    const bool found = fgets(line, sizeof(line), f) != NULL;
    (void)fclose(f);
    if (!found) return;
  }
  (void)snprintf(t->commit, sizeof(t->commit), "%.12s", line);
}

static void test_begin(test_t *t) {
  auris_testmode_t *m = &t->mode;
  const auris_testplan_clip_t *c = auris_testmode_clip(m);
  if (auris_testplan_clip_dir(&t->plan, m->location, m->clip, t->results, t->final,
                              sizeof(t->final)) != 0) {
    auris_testmode_fail(m, "THE FOLDER NAME IS TOO LONG");
    return;
  }
  (void)snprintf(t->part, sizeof(t->part), "%s.part", t->final);
  /* Left by a power cut while saving. */
  (void)imager_clip_set_aside(t->part, "stale");
  char meta[1024];
  (void)snprintf(meta, sizeof(meta),
      "  \"location\": \"%s\",\n  \"test\": %u,\n  \"id\": \"%s\",\n"
      "  \"distance\": \"%s\",\n  \"position\": \"%s\",\n  \"source\": \"%s\",\n"
      "  \"device\": \"%s\",\n  \"has_target\": %s,\n  \"target_right_deg\": %.1f,\n"
      "  \"target_up_deg\": %.1f,\n  \"quiet_s\": %.1f,\n  \"source_s\": %.1f,\n"
      "  \"commit\": \"%s\",\n  \"array\": \"%s\",\n  \"pan_deg\": %.1f,\n  \"tilt_deg\": %.1f,\n"
      "  \"diffuse\": %.2f,\n  \"gating\": %s,\n  \"smoothing\": %s,\n  \"fps_setting\": %u,\n"
      "  \"gate_recalibrated_at_start\": true",
      t->plan.locations[m->location], c->test, c->id, c->distance, c->position, c->source,
      c->device, c->has_target ? "true" : "false", (double)c->right_deg, (double)c->up_deg,
      (double)AURIS_TESTMODE_QUIET_S, (double)c->seconds, t->commit, t->array,
      (double)t->pan_deg, (double)t->tilt_deg, (double)t->diffuse,
      t->settings.gating ? "true" : "false", t->settings.smoothing ? "true" : "false",
      t->settings.fps);
  /* Every clip learns its own noise floor from the start of its quiet part,
     as a replay of its audio.wav does, not from whatever was going on when
     the imager started. Asked for first, so it covers the whole recording. */
  atomic_store(t->recalibrate, true);
  const int begun = imager_clip_begin(t->clip, t->part, meta,
      (double)AURIS_TESTMODE_QUIET_S + (double)c->seconds + 2.0, c->pictures);
  if (begun < 0) {
    fprintf(stderr, "test mode: %s\n", strerror(-begun));
    auris_testmode_fail(&t->mode, "THE RECORDING COULD NOT START");
    return;
  }
  t->next_picture = 0U;
  t->have_held = false;
  t->hold_next = false;
}

static void test_step(test_t *t, uint64_t now) {
  auris_testmode_t *m = &t->mode;
  for (;;) {
    const auris_testmode_action_t action = auris_testmode_take(m, now);
    if (action == AURIS_TESTMODE_ACT_NONE) break;
    switch (action) {
      case AURIS_TESTMODE_ACT_BEGIN: test_begin(t); break;
      case AURIS_TESTMODE_ACT_SOURCE: imager_clip_source(t->clip, now); break;
      case AURIS_TESTMODE_ACT_END:
        t->hold_next = true;
        if (imager_clip_end(t->clip) < 0) auris_testmode_fail(m, "THE RECORDING WAS LOST");
        break;
      case AURIS_TESTMODE_ACT_CANCEL: imager_clip_cancel(t->clip); break;
      case AURIS_TESTMODE_ACT_KEEP:
        if (rename(t->part, t->final) != 0) {
          fprintf(stderr, "test mode: %s: %s\n", t->final, strerror(errno));
          auris_testmode_fail(m, "THE CLIP COULD NOT BE KEPT");
        } else auris_testmode_kept(m, test_free_mb(t));
        break;
      case AURIS_TESTMODE_ACT_REDO:
        /* Kept to one side: it may be a real result of the system failing. */
        (void)imager_clip_set_aside(t->part, "redo");
        break;
      default: break;
    }
  }
  if (m->phase == AURIS_TESTMODE_SAVING) {
    imager_clip_summary_t s;
    const int saved = imager_clip_poll(t->clip, &s);
    if (saved != 1) {
      if (saved < 0) fprintf(stderr, "test mode: saving: %s\n", strerror(-saved));
      auris_testmode_saved(m, &(auris_testmode_review_t){
          .shown_pct = s.shown_pct, .source_dbfs = s.source_dbfs, .quiet_dbfs = s.quiet_dbfs,
          .video_fps = s.video_fps, .lost_frames = s.lost_frames, .replaced = s.replaced,
          .failed = saved < 0});
    }
  }
}

int main(int argc, char **argv) {
  auris_outputs_config_t outputs={0};
  bool calibrating = false;
  bool no_ui=false, fps_override=false;
  const char *settings_path="configs/runtime_settings.txt", *touch_path=NULL;
  const char *test_plan_path="configs/test_plan.txt", *results_path="results";
  unsigned long fps = 15U;
  const char *calibration_path = "configs/camera_calibration.txt";
  imager_audio_options_t audio_options = {.array = "respeaker", .step = 1U};
  const char *audio_file = NULL;
  float pan_deg = 0.0f, tilt_deg = 0.0f;
  bool usage_error = false;
  for (int i = 1; i < argc; ++i) {
    int parsed=auris_outputs_option(&outputs,argc,argv,&i);
    if (parsed<0) {
      fprintf(stderr,"output options: --output stream|display (repeatable), --rotation 0|90|180|270, --drm-device PATH, --connector ID\n");
      return EXIT_FAILURE;
    }
    if (parsed) continue;
    if (strcmp(argv[i], "--calibrate") == 0) calibrating = true;
    else if (strcmp(argv[i], "--no-ui") == 0) no_ui=true;
    else if (strcmp(argv[i], "--settings") == 0 && i+1<argc) settings_path=argv[++i];
    else if (strcmp(argv[i], "--touch-device") == 0 && i+1<argc) touch_path=argv[++i];
    else if (strcmp(argv[i], "--test-plan") == 0 && i+1<argc) test_plan_path=argv[++i];
    else if (strcmp(argv[i], "--results") == 0 && i+1<argc) results_path=argv[++i];
    else if (strcmp(argv[i], "--calibration") == 0 && i + 1 < argc)
      calibration_path = argv[++i];
    else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
      char *end = NULL;
      errno = 0;
      fps = strtoul(argv[++i], &end, 10);
      fps_override=true;
      if (errno != 0 || end == argv[i] || *end != '\0' || fps < 1U || fps > 30U) {
        usage_error = true; break;
      }
    }
    else if (strcmp(argv[i], "--no-track") == 0) audio_options.tracking = -1;
    else if (strcmp(argv[i], "--height") == 0 && i + 3 < argc) {
      char *end1 = NULL, *end2 = NULL, *end3 = NULL;
      audio_options.height_split_hz = strtof(argv[i + 1], &end1);
      audio_options.height_keep = strtof(argv[i + 2], &end2);
      audio_options.height_share = strtof(argv[i + 3], &end3);
      audio_options.height_given = true;
      if (*end1 != '\0' || *end2 != '\0' || *end3 != '\0' ||
          end1 == argv[i + 1] || end2 == argv[i + 2] || end3 == argv[i + 3]) {
        usage_error = true; break;
      }
      i += 3;
    }
    else if (strcmp(argv[i], "--diffuse") == 0 && i + 1 < argc) {
      char *end = NULL;
      audio_options.diffuse = strtof(argv[++i], &end);
      audio_options.diffuse_given = true;
      if (end == argv[i] || *end != '\0') { usage_error = true; break; }
    }
    else if (strcmp(argv[i], "--audio-file") == 0 && i + 1 < argc) audio_file = argv[++i];
    else if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) audio_options.array = argv[++i];
    else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) audio_options.device = argv[++i];
    else if (strcmp(argv[i], "--band") == 0 && i + 2 < argc) {
      char *end1 = NULL, *end2 = NULL;
      audio_options.band_low_hz = strtof(argv[i + 1], &end1);
      audio_options.band_high_hz = strtof(argv[i + 2], &end2);
      audio_options.band_given = true;
      if (*end1 != '\0' || *end2 != '\0' || end1 == argv[i + 1] || end2 == argv[i + 2]) {
        usage_error = true; break;
      }
      i += 2;
    }
    else if ((strcmp(argv[i], "--pan") == 0 || strcmp(argv[i], "--tilt") == 0) &&
             i + 1 < argc) {
      char *end = NULL;
      const bool pan = argv[i][2] == 'p';
      const float value = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0' || !isfinite(value) ||
          fabsf(value) > 30.0f) { usage_error = true; break; }
      if (pan) pan_deg = value; else tilt_deg = value;
    }
    else if (strcmp(argv[i], "--step") == 0 && i + 1 < argc) {
      char *end = NULL;
      audio_options.step = strtoul(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0') { usage_error = true; break; }
    }
    /* A bare ALSA device name: the original ReSpeaker form. */
    else if (argv[i][0] != '-' && audio_options.device == NULL) audio_options.device = argv[i];
    else { usage_error = true; break; }
  }
  imager_audio_plan_t plan;
  if (!usage_error && imager_audio_plan(&audio_options, &plan) < 0) {
    fprintf(stderr, "unknown array, or bad --band/--step/--diffuse (band 0 <= LO < HI <= Nyquist, step 1, 2 or 4, diffuse 0..1)\n");
    usage_error = true;
  }
  if (usage_error) {
    fprintf(stderr,"runtime: [--settings FILE] [--no-ui] [--touch-device PATH]; touch UI defaults on with display, off during calibration\n");
    fprintf(stderr,"test mode: [--test-plan FILE] [--results DIR]; opened from the touch panel\n");
    fprintf(stderr, "usage: %s [--calibrate] [--calibration FILE] [--fps 1..30]\n"
                    "          [--array NAME] [--device D] [--band LO HI] [--step 1|2|4]"
                    " [--no-track] [--diffuse 0..1]\n"
                    "          [--height HZ KEEP SHARE]   (bins below HZ keep KEEP of their say on height once the bins above weigh SHARE of them; 0 1 0: off)\n"
                    "          [--audio-file REC.wav]   (replay a recording, looped, with the live camera)\n"
                    "          [--pan DEG] [--tilt DEG]   (move the heat map right / up over the picture, -30..30)\n"
                    "       %s [options] ALSA_DEVICE   (ReSpeaker form)\n"
                    "  NAME: %s; the device, band and frame come from the array's profile\n",
            argv[0], argv[0], auris_config_names());
    return EXIT_FAILURE;
  }
  auris_settings_t settings=auris_settings_defaults();
  if (!calibrating) {
    int loaded=auris_settings_load(settings_path,&settings);
    if (loaded<0 && loaded!=-ENOENT)
      fprintf(stderr,"settings: %s rejected: %s; using defaults\n",settings_path,strerror(-loaded));
    else if (!loaded) fprintf(stderr,"settings: loaded %s\n",settings_path);
  } else settings.smoothing=false;
  if (fps_override) settings.fps=(unsigned int)fps;
  fps=settings.fps;
  auris_ui_t ui;
  auris_ui_init(&ui,&settings,now_us());
  struct sigaction action = {0};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
    return EXIT_FAILURE;
  action.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &action, NULL)) return EXIT_FAILURE;

  const auris_view_t search = imager_search_view();
  auris_view_t camera_view = {
      .width = 1640U, .height = 1232U, .fx = 1358.5007f, .fy = 1356.8622f,
      .cx = 819.5f, .cy = 615.5f,
      .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  const uint32_t canvas_width = 640U, canvas_height = 360U;
  /* Static: the map history alone is about 0.8 MB. */
  static audio_t audio = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  audio.settings = settings;
  audio.smoothing_applied = settings.smoothing;
  audio.status = (auris_ui_status_t){.dbfs = -120.0f};
  audio.history_capacity = plan.history;
  audio.audio_file = audio_file;
  atomic_init(&audio.stop, false);
  atomic_init(&audio.failed, false);
  atomic_init(&audio.recalibrate, false);
  camera_chain_t *camera = NULL;
  auris_outputs_t *output_stream = NULL;
  auris_touch_t *touch=NULL;
  auris_settings_writer_t *writer=NULL;
  touch_context_t touch_context={.ui=&ui};
  static test_t test;
  bool ui_enabled=false,save_busy=false;
  unsigned int saved_revision=0U;
  imager_calibration_t session = {.path = calibration_path};
  uint8_t *rgb = NULL;
  pthread_t thread;
  bool started = false;
  int result = EXIT_FAILURE, err = 0, stdin_flags = -1;
  session.array_id = auris_calibration_array_id(plan.config.array);
  err = auris_camera_calibration_load(calibration_path, session.array_id, &camera_view);
  if (err == -ENOENT) {
    fprintf(stderr, "CAL no saved rotation; using nominal alignment\n");
    err = 0;
  } else if (err < 0) goto done;
  else fprintf(stderr, "CAL loaded %s\n", calibration_path);
  if (auris_view_nudge(&camera_view, pan_deg, tilt_deg) != AURIS_OK) goto done;
  if (pan_deg != 0.0f || tilt_deg != 0.0f)
    fprintf(stderr, "CAL heat map moved %+.2f deg right, %+.2f deg up\n",
            (double)pan_deg, (double)tilt_deg);
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
  err = imager_dsp_open(&audio.dsp, &plan, directions, COLUMNS, ROWS,
                        settings.gating, settings.smoothing);
  if (err < 0) goto done;
  fprintf(stderr, "audio: %s on %s, %zu-sample frames every %zu (%.1f ms), "
          "band %.0f-%.0f Hz, %s, %s, search step %zu, %zu maps kept\n",
          plan.config.array->name, plan.device, plan.frame_size, plan.frame_hop,
          1000.0 * (double)plan.frame_hop / (double)plan.rate,
          (double)plan.band_low_hz, (double)plan.band_high_hz,
          plan.weighting ? (plan.diffuse > 0.0f ? "bins weighted by SNR and against "
                                           "the room" : "bins weighted by SNR")
                         : "level gate",
          !settings.smoothing ? "raw maps (smoothing off)"
          : plan.tracking ? "tracked, held 0.6 s"
                          : "0.1 s smoothing (tracker off)",
          plan.step, plan.history);
  if (plan.height_split_hz > 0.0f)
    fprintf(stderr, "audio: height from the bins above %.0f Hz once they "
            "weigh %.2f of the rest; lower bins then keep %.2f of their say "
            "on it\n", (double)plan.height_split_hz,
            (double)plan.height_share, (double)plan.height_keep);
  err = camera_chain_open(&camera, canvas_width, canvas_height,
                          "configs/lens_shading.txt");
  if (err < 0) goto done;
  const auris_image_rect_t *rect = camera_chain_content(camera);
  const auris_overlay_config_t drawing = {
      .camera = camera_view, .search = search,
      .layout = {canvas_width, canvas_height, rect->x, rect->y,
                  rect->width, rect->height},
      .columns = COLUMNS, .rows = ROWS,
      .cutoff = settings.cutoff, .opacity = settings.opacity, .mark_peak = true};
  session.drawing = drawing;
  err = auris_overlay_open(&session.overlay, &drawing);
  if (err < 0) goto done;
  const size_t bytes = (size_t)canvas_width * canvas_height * 3U;
  rgb = malloc(bytes);
  if (rgb == NULL) { err = -ENOMEM; goto done; }
  auris_rgb24_t canvas = {rgb, canvas_width, canvas_height,
                          (size_t)canvas_width * 3U, bytes};
  err = auris_outputs_open(&output_stream, &outputs, STDOUT_FILENO,
                            canvas.width, canvas.height);
  if (err < 0) goto done;
  if (!no_ui && !calibrating && auris_outputs_display_info(output_stream,&touch_context.display)) {
    int opened=auris_touch_open(&touch,touch_path);
    if (opened<0) fprintf(stderr,"touch unavailable: %s; continuing without controls\n",strerror(-opened));
    else {
      ui_enabled=true;
      opened=auris_settings_writer_open(&writer,settings_path);
      if (opened<0) fprintf(stderr,"settings writer unavailable: %s\n",strerror(-opened));
    }
  }
  if (ui_enabled) {
    size_t bad_line=0U;
    int prepared=auris_testplan_load(test_plan_path,&test.plan,&bad_line);
    if (prepared==-EINVAL) fprintf(stderr,"test mode off: %s line %zu is not valid\n",test_plan_path,bad_line);
    else if (prepared<0) fprintf(stderr,"test mode off: %s: %s\n",test_plan_path,strerror(-prepared));
    else {
      const auris_testmode_layout_t where={camera_view.fx,camera_view.fy,camera_view.cx,camera_view.cy,
          (float)camera_view.width,(float)camera_view.height,(int)rect->x,(int)rect->y,
          (int)rect->width,(int)rect->height};
      auris_testmode_init(&test.mode,&test.plan,&where);
      test.results=results_path; test.plan_path=test_plan_path;
      test.array=plan.config.array->name; test.pan_deg=pan_deg; test.tilt_deg=tilt_deg;
      test.diffuse=plan.diffuse;
      test.recalibrate=&audio.recalibrate;
      test_read_commit(&test);
      test.held=malloc(bytes);
      prepared=test.held ? imager_clip_open(&test.clip,plan.config.capture,rect->width,rect->height) : -ENOMEM;
      if (prepared<0) fprintf(stderr,"test mode off: %s\n",strerror(-prepared));
      else { audio.clip=test.clip; touch_context.test=&test.mode; }
    }
  }
  err = camera_chain_start(camera);
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
  uint64_t output_period_us = 1000000U / fps;
  uint64_t fps_start=now_us(),fps_frames=0U;
  float measured_fps=0.0f;
  while (running && !atomic_load(&audio.failed)) {
    err = auris_outputs_error(output_stream);
    if (err < 0) goto done;
    if (touch) {
      if (!auris_outputs_display_info(output_stream,&touch_context.display)) {
        auris_touch_close(touch); touch=NULL; ui_enabled=false;
      } else {
        int polled=auris_touch_poll(touch,touch_event,&touch_context);
        if (polled<0) {
          fprintf(stderr,"touch stopped: %s; capture continues (restart to reconnect)\n",strerror(-polled));
          auris_touch_close(touch); touch=NULL; ui_enabled=false;
        }
      }
    }
    if (ui.settings.fps!=fps) {
      fps=ui.settings.fps; output_period_us=1000000U/fps; next_output=0U;
      fps_start=now_us(); fps_frames=0U;
    }
    if (settings.cutoff!=ui.settings.cutoff || settings.opacity!=ui.settings.opacity) {
      err=auris_overlay_set_style(session.overlay,ui.settings.cutoff,ui.settings.opacity);
      if (err<0) goto done;
      session.drawing.cutoff=ui.settings.cutoff; session.drawing.opacity=ui.settings.opacity;
    }
    settings=ui.settings;
    (void)pthread_mutex_lock(&audio.mutex);
    audio.settings=settings;
    (void)pthread_mutex_unlock(&audio.mutex);
    if (save_busy && auris_settings_save_status(writer)!=1) {
      int saved=auris_settings_save_status(writer);
      save_busy=false;
      if (!saved) ui.saved_revision=saved_revision;
      fprintf(stderr,"settings save: %s\n",saved ? strerror(-saved):"complete");
    }
    if (ui.save_requested) {
      ui.save_requested=false;
      int saved=auris_settings_save(writer,&settings);
      if (!saved) { save_busy=true; saved_revision=ui.revision; }
      else fprintf(stderr,"settings save: %s\n",strerror(-saved));
    }
    if (ui.test_requested) {
      ui.test_requested=false;
      if (test.clip) { test_find_finished(&test); auris_testmode_open(&test.mode,test_free_mb(&test)); }
      else fprintf(stderr,"test mode is not available\n");
    }
    const bool test_was_open=test.clip && test.mode.phase!=AURIS_TESTMODE_OFF;
    if (test_was_open) {
      test.settings=settings;
      test_step(&test,now_us());
      if (test.mode.phase==AURIS_TESTMODE_OFF) { ui.visible=false; ui.last_touch=now_us(); }
    }
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
    err = camera_chain_acquire(camera, &frame, 100);
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
    if (present) err = camera_chain_render(camera, &frame, &canvas);
    const int released = camera_chain_release(camera);
    if (err < 0) goto done;
    if (released < 0) { err = released; goto done; }
    if (!present) continue;

    const uint64_t composed = now_us();
    const snapshot_t latest = match_audio(&audio, frame.timestamp_us, composed, monotonic);
    const uint64_t skew = latest.sample_us == 0U ? 0U :
        (frame.timestamp_us > latest.sample_us ? frame.timestamp_us - latest.sample_us
                                               : latest.sample_us - frame.timestamp_us);
    const bool visible = latest.valid && settings.overlay;
    const bool test_open = test.clip && test.mode.phase != AURIS_TESTMODE_OFF;
    const bool recording = test_open && auris_testmode_recording(&test.mode);
    int32_t picture = -1;
    if (recording && composed >= test.next_picture) {
      picture = imager_clip_next_picture(test.clip);
      if (picture >= 0) {
        (void)imager_clip_picture(test.clip, &canvas, rect->x, rect->y, picture, false);
        test.next_picture = composed + TEST_PICTURE_PERIOD_US;
      }
    }
    err = auris_overlay_draw(session.overlay, latest.scores, DIRECTIONS,
                              visible ? latest.column : 0.0f,
                              visible ? latest.row : 0.0f,
                              visible ? latest.strength : 0.0f, &canvas);
    if (err < 0) goto done;
    if (picture >= 0)
      (void)imager_clip_picture(test.clip, &canvas, rect->x, rect->y, picture, true);
    if (test_open && test.hold_next) {
      memcpy(test.held, canvas.data, bytes);
      test.hold_next = false; test.have_held = true;
    }
    if (test_open && test.have_held && (test.mode.phase == AURIS_TESTMODE_SAVING ||
                                        test.mode.phase == AURIS_TESTMODE_REVIEW))
      memcpy(canvas.data, test.held, bytes);
    ++fps_frames;
    if (composed-fps_start>=1000000U) {
      measured_fps=(float)((double)fps_frames*1000000.0/(double)(composed-fps_start));
      fps_frames=0U; fps_start=composed;
    }
    if (ui_enabled) {
      auris_ui_status_t status;
      (void)pthread_mutex_lock(&audio.mutex);
      status=audio.status;
      (void)pthread_mutex_unlock(&audio.mutex);
      auris_output_stats_t stats;
      auris_outputs_stats(output_stream,&stats);
      status.fps=measured_fps; status.dropped=stats.dropped;
      status.save_status=auris_settings_save_status(writer);
      if (test_open) {
        const auris_testmode_live_t live={status.dbfs,visible};
        err=auris_testmode_draw(&test.mode,&canvas,&live,composed);
      } else err=auris_ui_draw(&ui,&canvas,&status,composed);
      if (err<0) goto done;
    }
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
    err = auris_outputs_submit(output_stream, &canvas, &metadata);
    if (err < 0) goto done;
    if (recording) {
      auris_output_stats_t sent;
      auris_outputs_stats(output_stream, &sent);
      const imager_clip_frame_t logged = {
          .sequence = frame.sequence, .camera_us = frame.timestamp_us,
          .audio_us = latest.sample_us, .composed_us = composed,
          .replaced = sent.dropped, .strength = visible ? latest.strength : 0.0f,
          .visible = visible, .picture = picture};
      imager_clip_frame(test.clip, &logged);
    }
    if (next_output == 0U)
      next_output = received;
    do {
      next_output += output_period_us;
    } while (next_output <= received);
    if (calibrating)
      imager_calibration_frame(&session, frame.sequence, frame.timestamp_us, monotonic);
    if (composed >= next_timing) {
      auris_output_stats_t stats;
      auris_outputs_stats(output_stream, &stats);
      fprintf(stderr, "TIMING camera %.1f ms | matched map %.1f ms | skew %.1f ms | DSP %.2f ms | output convert %.2f ms/%zu B | present %.2f ms | dropped %llu | overlay %s\n",
          monotonic && composed >= frame.timestamp_us ?
            (double)(composed - frame.timestamp_us)/1000.0 : -1.0,
          latest.sample_us != 0U && composed >= latest.sample_us ?
            (double)(composed - latest.sample_us)/1000.0 : -1.0,
          monotonic && latest.sample_us != 0U ? (double)skew/1000.0 : -1.0,
          (double)latest.dsp_us/1000.0, (double)stats.encode_us/1000.0,
          stats.encoded_bytes, (double)stats.write_us/1000.0,
          (unsigned long long)stats.dropped,
          visible ? "visible":"hidden");
      camera_chain_status_t seen;
      camera_chain_status(camera, &seen);
      fprintf(stderr, "EXPOSURE %.1f ms, analogue x%.2f, digital x%.2f, software x%.2f | balance R %.2f B %.2f | level %.3f, %.1f %% at the limit%s\n",
          (double)seen.exposure_ms, (double)seen.analogue, (double)seen.digital,
          (double)seen.software, (double)seen.balance_red, (double)seen.balance_blue,
          (double)seen.mean, 100.0 * (double)seen.clipped,
          seen.hardware ? " | image processor" : "");
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
    auris_outputs_stats(output_stream, &stats);
    fprintf(stderr, "primary output %lu fps: submitted %llu, sent %llu, replaced %llu\n",
            fps,
            (unsigned long long)stats.submitted,
            (unsigned long long)stats.sent,
            (unsigned long long)stats.dropped);
  }
  auris_outputs_close(output_stream);
  auris_touch_close(touch);
  imager_clip_close(test.clip);
  free(test.held);
  auris_settings_writer_close(writer);
  camera_chain_close(camera);
  auris_overlay_close(session.overlay);
  free(rgb);
  imager_dsp_close(&audio.dsp);
  (void)pthread_mutex_destroy(&audio.mutex);
  if (stdin_flags >= 0) (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
  return running ? result : EXIT_SUCCESS;
}
