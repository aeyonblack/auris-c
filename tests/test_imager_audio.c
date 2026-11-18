#include "auris_timeline.h"
#include "auris_view.h"
#include "imager_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

#define COLUMNS 65U
#define ROWS 49U
#define DIRECTIONS (COLUMNS * ROWS)

static const auris_view_t view = {
    .width = 640U, .height = 480U, .fx = 480.0f, .fy = 480.0f, .cx = 319.5f,
    .cy = 239.5f, .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

static auris_vec3f_t grid[DIRECTIONS];
static imager_dsp_t dsp;
static auris_framer_t framer;
static auris_analysis_frame_t frame;
static auris_audio_block_t block;
static auris_srp_result_t result;

static int test_plans(void) {
  imager_audio_plan_t plan;
  imager_audio_options_t options = {.array = "respeaker", .step = 1U};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.rate == 16000U && plan.frame_size == 512U &&
        plan.frame_hop == 256U && plan.upsample == 8U);
  CHECK(plan.band_low_hz == 500.0f && plan.band_high_hz == 3000.0f);
  CHECK(strcmp(plan.device, "hw:CARD=ArrayUAC10,DEV=0") == 0);
  CHECK(plan.history == 32U); /* 512 ms of 16 ms hops, as before */

  options.array = "cmb";
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.rate == 48000U && plan.frame_size == 1024U &&
        plan.frame_hop == 512U && plan.upsample == 4U);
  /* Module 27: SNR-weighted, so the band reaches down to 200 Hz. */
  CHECK(plan.weighting && plan.coherence_min == 0.15f);
  CHECK(plan.band_low_hz == 200.0f && plan.band_high_hz == 6000.0f);
  CHECK(strcmp(plan.device, "hw:AurisCMB") == 0);
  options.weighting = -1;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(!plan.weighting && plan.band_low_hz == 500.0f);
  options.weighting = 0;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.config.array == auris_config_cmb_8());
  CHECK(plan.history == 48U); /* 512 ms of 10.67 ms hops */
  CHECK(plan.history <= IMAGER_HISTORY_MAX);

  options.device = "hw:Other";
  options.band_given = true;
  options.band_low_hz = 300.0f;
  options.band_high_hz = 4000.0f;
  options.step = 4U;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(strcmp(plan.device, "hw:Other") == 0 && plan.step == 4U);
  CHECK(plan.band_low_hz == 300.0f && plan.band_high_hz == 4000.0f);
  options.band_low_hz = 0.0f; /* 0 0: every bin up to Nyquist */
  options.band_high_hz = 0.0f;
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.band_low_hz == 0.0f && plan.band_high_hz == 24000.0f);

  options.band_low_hz = 4000.0f;
  options.band_high_hz = 500.0f;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  options.band_low_hz = 500.0f;
  options.band_high_hz = 30000.0f;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  options.band_given = false;
  options.step = 3U;
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  options.step = 1U;
  options.array = "nope";
  CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
  CHECK(imager_audio_plan(NULL, &plan) == -EINVAL);
  return EXIT_SUCCESS;
}

/* The CMB's history must still reach a map as old as the matcher accepts
   (250 ms) when maps arrive every hop. */
static int test_history_reach(void) {
  imager_audio_plan_t plan;
  const imager_audio_options_t options = {.array = "cmb", .step = 1U};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  auris_timeline_point_t points[IMAGER_HISTORY_MAX];
  const uint64_t hop_us = 1000000U * plan.frame_hop / plan.rate;
  const uint64_t now = 10000000U;
  for (size_t i = 0U; i < plan.history; ++i) {
    const uint64_t t = now - (plan.history - 1U - i) * hop_us;
    points[i] = (auris_timeline_point_t){t, t, true};
  }
  const uint64_t oldest = now - (plan.history - 1U) * hop_us;
  CHECK(now - oldest >= 250000U);
  const size_t found =
      auris_timeline_match(points, plan.history, now - 240000U, now, 250000U,
                           100000U);
  CHECK(found != SIZE_MAX);
  const uint64_t skew = points[found].sample_us > now - 240000U
                            ? points[found].sample_us - (now - 240000U)
                            : (now - 240000U) - points[found].sample_us;
  CHECK(skew <= hop_us / 2U + 1U);
  return EXIT_SUCCESS;
}

/* A far broadband source along `ray`, switched on at the source from
   `on` seconds, plus a little independent noise on every microphone. */
static uint32_t seed = 7U;
static float noise(float a) {
  seed = seed * 1664525U + 1013904223U;
  return a * ((float)(seed >> 8) / 8388608.0f - 1.0f);
}

static void source_block(const imager_audio_plan_t *plan, auris_vec3f_t ray,
                         uint64_t position, double on) {
  const auris_array_t *array = plan->config.array;
  block = (auris_audio_block_t){.sample_rate = plan->rate,
                                .channels = array->microphone_count,
                                .frames = AURIS_AUDIO_FRAMES,
                                .first_frame = position};
  for (size_t m = 0U; m < block.channels; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const double advance =
        ((double)p.x_m * ray.x_m + (double)p.y_m * ray.y_m +
         (double)p.z_m * ray.z_m) /
        (double)array->speed_of_sound_mps;
    for (size_t n = 0U; n < AURIS_AUDIO_FRAMES; ++n) {
      const double t = (double)(position + n) / (double)plan->rate + advance;
      double value = 0.0;
      for (size_t k = 5U; t >= on && k < 225U; k += 7U) {
        value += 0.006 * sin(6.283185307179586 * (double)k * 31.25 * t +
                             (double)(k * k));
      }
      block.samples[m][n] = (float)value + noise(0.0005f);
    }
  }
}

/* Runs 1.5 s of audio through the imager's DSP: quiet, then a source from
   1.2 s. Returns what the last frame would draw. `tracking` -1 selects the
   earlier 0.1 s smoothing in place of the tracker. */
static int run(const char *name, size_t step, int tracking, auris_vec3f_t ray,
               imager_view_t *last, size_t *shown_frames) {
  imager_audio_plan_t plan;
  const imager_audio_options_t options = {.array = name, .step = step,
                                          .tracking = tracking};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(plan.tracking == (tracking >= 0));
  CHECK(imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, true, true) == 0);
  CHECK(dsp.tracking == (tracking >= 0));
  CHECK(auris_framer_init(&framer, plan.frame_size, plan.frame_hop) ==
        AURIS_OK);
  result = (auris_srp_result_t){0};
  auris_activity_result_t gate = {0};
  bool closed_before_onset = true;
  const uint64_t end = (uint64_t)plan.rate * 3U / 2U; /* 1.5 s */
  uint64_t position = 0U;
  for (; position < end; position += AURIS_AUDIO_FRAMES) {
    source_block(&plan, ray, position, 1.2);
    bool ready = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (!ready) continue;
    CHECK(imager_dsp_process(&dsp, &frame, &gate, &result, last) == AURIS_OK);
    if (frame.first_frame + plan.frame_size < (uint64_t)plan.rate * 6U / 5U) {
      closed_before_onset = closed_before_onset && !gate.active &&
                            !last->valid;
    } else if (last->valid) {
      ++*shown_frames;
      CHECK(last->map != NULL && last->strength > 0.0f &&
            last->strength <= 1.0f);
      CHECK(last->column >= 0.0f && last->column <= (float)(COLUMNS - 1U) &&
            last->row >= 0.0f && last->row <= (float)(ROWS - 1U));
    }
  }
  CHECK(closed_before_onset); /* calibration and noise never open it */
  /* The map handed out is the one to draw: its largest value is at the
     estimate (within a cell, for the tracker's spread map). */
  if (last->valid) {
    size_t top = 0U;
    for (size_t q = 0U; q < DIRECTIONS; ++q) {
      top = last->map[q] > last->map[top] ? q : top;
    }
    const int dc = (int)(top % COLUMNS) - (int)(last->best % COLUMNS);
    const int dr = (int)(top / COLUMNS) - (int)(last->best / COLUMNS);
    CHECK(dc >= -1 && dc <= 1 && dr >= -1 && dr <= 1);
  }
  /* Process again with silence until the gate closes: nothing is left. */
  size_t held = 0U;
  for (const uint64_t stop = position + (uint64_t)plan.rate * 2U;
       position < stop; position += AURIS_AUDIO_FRAMES) {
    source_block(&plan, ray, position, 1.0e9);
    bool ready = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (!ready) continue;
    imager_view_t after;
    CHECK(imager_dsp_process(&dsp, &frame, &gate, &result, &after) ==
          AURIS_OK);
    if (after.valid && after.fresh) {
      /* Only frames that still overlap the end of the source are fresh. */
      CHECK(frame.first_frame < end + plan.frame_size);
    } else if (after.valid) {
      ++held;
    }
  }
  CHECK(!gate.active);
  /* Tracked estimates are held and faded for 0.6 s; the earlier smoothing
     held its last map for the gate's 0.192 s. */
  const double held_s = (double)held * (double)plan.frame_hop / plan.rate;
  printf("  held for %.2f s after the source stopped\n", held_s);
  CHECK(tracking >= 0 ? (held_s > 0.45 && held_s < 0.75)
                      : (held_s > 0.1 && held_s < 0.3));
  imager_dsp_close(&dsp);
  return EXIT_SUCCESS;
}

static int test_pipeline(void) {
  CHECK(auris_view_grid(&view, COLUMNS, ROWS, grid, DIRECTIONS) == AURIS_OK);
  auris_vec3f_t ray;
  const float u = 46.8f / 64.0f * 639.0f, v = 15.2f / 48.0f * 479.0f;
  CHECK(auris_view_ray(&view, u, v, &ray) == AURIS_OK);
  const char *arrays[2] = {"cmb", "respeaker"};
  for (size_t a = 0U; a < 2U; ++a) {
    for (int tracking = 0; tracking >= -1; --tracking) {
      for (size_t step = 1U; step <= 4U; step *= 4U) {
        imager_view_t last = {0};
        size_t shown = 0U;
        CHECK(run(arrays[a], step, tracking, ray, &last, &shown) ==
              EXIT_SUCCESS);
        printf("%s step %zu %s: estimate (%.2f, %.2f), cell (%zu, %zu), "
               "strength %.2f, %zu frames shown\n",
               arrays[a], step, tracking >= 0 ? "tracked" : "smoothed",
               (double)last.column, (double)last.row, last.best % COLUMNS,
               last.best / COLUMNS, (double)last.strength, shown);
        CHECK(last.valid && last.fresh);
        /* 0.3 s of source is about 28 CMB frames or 19 ReSpeaker frames. */
        CHECK(shown >= 14U);
        if (tracking >= 0) {
          /* The tracker reports the position between cells. The CMB's
             frames are weighted by SNR and agree closely; the ReSpeaker's
             are not (most of its bins hold only noise here), so after 0.3 s
             its estimate is still within a cell or so. */
          const float tolerance = a == 0U ? 0.5f : 1.5f;
          CHECK(fabsf(last.column - 46.8f) < tolerance &&
                fabsf(last.row - 15.2f) < tolerance);
          CHECK(fabsf((float)(last.best % COLUMNS) - last.column) <= 1.0f &&
                fabsf((float)(last.best / COLUMNS) - last.row) <= 1.0f);
          CHECK(last.strength > 0.9f && last.confidence > 0.6f);
        } else {
          CHECK(last.best % COLUMNS == 47U && last.best / COLUMNS == 15U);
          CHECK(last.column == 47.0f && last.row == 15.0f &&
                last.strength == 1.0f);
        }
      }
    }
  }
  /* Turning smoothing off at run time gives the raw per-frame map. */
  {
    imager_audio_plan_t plan;
    const imager_audio_options_t options = {.array = "cmb", .step = 1U};
    CHECK(imager_audio_plan(&options, &plan) == 0);
    CHECK(plan.hold_s == 0.6f && plan.fade_from_s == 0.15f);
    CHECK(imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, true, true) == 0);
    CHECK(dsp.tracking);
    CHECK(imager_dsp_set_smoothing(&dsp, false) == AURIS_OK && !dsp.tracking);
    CHECK(imager_dsp_set_smoothing(&dsp, true) == AURIS_OK && dsp.tracking);
    CHECK(imager_dsp_set_smoothing(NULL, true) == AURIS_ERROR_INVALID_ARGUMENT);
    imager_dsp_close(&dsp);
  }
  /* Tracker settings are checked with the rest of the plan. */
  {
    imager_audio_plan_t plan;
    imager_audio_options_t options = {.array = "cmb", .step = 1U,
                                      .track_kappa = 1.0f, .track_drift = 3.0f,
                                      .track_display = 3.0f, .hold_s = 1.0f};
    CHECK(imager_audio_plan(&options, &plan) == 0);
    CHECK(plan.track.kappa == 1.0f && plan.track.drift_cells == 3.0f &&
          plan.track.display_cells == 3.0f && plan.hold_s == 1.0f);
    options.hold_s = 9.0f;
    CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
    options.hold_s = 0.0f;
    options.track_kappa = -1.0f;
    CHECK(imager_audio_plan(&options, &plan) == -EINVAL);
    options.track_kappa = 0.0f;
    options.track_display = 9.0f; /* accepted by the plan, refused at open */
    CHECK(imager_audio_plan(&options, &plan) == 0);
    CHECK(imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, true, true) ==
          -EINVAL);
    CHECK(dsp.fft == NULL && dsp.srp == NULL);
  }
  /* A grid step that does not divide 64 x 48 is refused. */
  imager_audio_plan_t plan;
  const imager_audio_options_t options = {.array = "cmb", .step = 1U};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  plan.step = 3U;
  CHECK(imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, true, true) ==
        -EINVAL);
  CHECK(dsp.fft == NULL && dsp.srp == NULL);
  return EXIT_SUCCESS;
}

/* Recalibrating forgets a noise floor learned while the room was loud: from
   then on the chain behaves exactly like one opened at that moment. */
static int test_recalibrate(void) {
  static imager_dsp_t fresh;
  static auris_framer_t framer_b;
  static auris_analysis_frame_t frame_b;
  static auris_srp_result_t result_b;
  imager_audio_plan_t plan;
  const imager_audio_options_t options = {.array = "cmb", .step = 1U};
  const auris_vec3f_t ray = {0.0f, 0.0f, 1.0f};
  CHECK(imager_audio_plan(&options, &plan) == 0);
  CHECK(imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, true, true) == 0);
  CHECK(auris_framer_init(&framer, plan.frame_size, plan.frame_hop) == AURIS_OK);
  result = (auris_srp_result_t){0};
  auris_activity_result_t gate = {0}, gate_b = {0};
  imager_view_t seen = {0}, seen_b = {0};
  /* Start-up with a loud sound going on: the floor is learned far too high. */
  uint64_t position = 0U;
  const uint64_t loud_end = (uint64_t)plan.rate * 3U / 2U;
  for (; position < loud_end; position += AURIS_AUDIO_FRAMES) {
    source_block(&plan, ray, position, 0.0);
    bool ready = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    if (ready) CHECK(imager_dsp_process(&dsp, &frame, &gate, &result, &seen) == AURIS_OK);
  }
  const float loud_floor = gate.noise_dbfs;
  CHECK(imager_dsp_recalibrate(NULL, true) == AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(imager_dsp_recalibrate(&dsp, true) == AURIS_OK);
  result = (auris_srp_result_t){0};
  CHECK(imager_dsp_open(&fresh, &plan, grid, COLUMNS, ROWS, true, true) == 0);
  CHECK(auris_framer_init(&framer, plan.frame_size, plan.frame_hop) == AURIS_OK);
  CHECK(auris_framer_init(&framer_b, plan.frame_size, plan.frame_hop) == AURIS_OK);
  result_b = (auris_srp_result_t){0};
  /* Quiet, then a source, seen by both chains. */
  const uint64_t onset = position + (uint64_t)plan.rate * 12U / 10U;
  const uint64_t end = position + (uint64_t)plan.rate * 2U;
  size_t compared = 0U, shown = 0U;
  for (; position < end; position += AURIS_AUDIO_FRAMES) {
    source_block(&plan, ray, position, (double)onset / (double)plan.rate);
    bool ready = false, ready_b = false;
    CHECK(auris_framer_push(&framer, &block, &frame, &ready) == AURIS_OK);
    CHECK(auris_framer_push(&framer_b, &block, &frame_b, &ready_b) == AURIS_OK);
    CHECK(ready == ready_b);
    if (!ready) continue;
    CHECK(imager_dsp_process(&dsp, &frame, &gate, &result, &seen) == AURIS_OK);
    CHECK(imager_dsp_process(&fresh, &frame_b, &gate_b, &result_b, &seen_b) == AURIS_OK);
    CHECK(gate.noise_dbfs == gate_b.noise_dbfs && gate.ready == gate_b.ready &&
          gate.active == gate_b.active && gate.weight_sum == gate_b.weight_sum);
    CHECK(seen.valid == seen_b.valid && seen.fresh == seen_b.fresh &&
          seen.strength == seen_b.strength);
    if (seen.valid) {
      CHECK(seen.best == seen_b.best && seen.column == seen_b.column &&
            seen.row == seen_b.row);
      ++shown;
    }
    ++compared;
  }
  CHECK(compared > 100U && shown > 0U);
  CHECK(gate.noise_dbfs < loud_floor - 10.0f);
  imager_dsp_close(&dsp);
  imager_dsp_close(&fresh);
  return EXIT_SUCCESS;
}
int main(void) {
  if (test_plans() != EXIT_SUCCESS || test_history_reach() != EXIT_SUCCESS ||
      test_pipeline() != EXIT_SUCCESS || test_recalibrate() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("imager audio tests: ok");
  return EXIT_SUCCESS;
}
