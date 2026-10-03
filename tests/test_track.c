#include "auris_track.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

#define COLUMNS 65U
#define ROWS 49U
#define CELLS (COLUMNS * ROWS)
#define HOP 0.0106667f /* 512 samples at 48 kHz */
#define SCALE 512.0f   /* score -> bins for 1024-sample frames */

static auris_track_t track;
static float scores[CELLS];
static float map[CELLS];

static uint32_t seed = 4242U;
static float uniform(void) { /* 0..1 */
  seed = seed * 1664525U + 1013904223U;
  return (float)(seed >> 8) / 16777216.0f;
}
static float normal(void) { /* Box-Muller */
  const float u = fmaxf(uniform(), 1.0e-7f), v = uniform();
  return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

/* One frame's SRP scores for a source at (x, y): a broad lobe, as the CMB
   gives. `coherence` is the peak as a share of the frame's weight. */
static void lobe(float x, float y, float weight, float coherence) {
  const float peak = coherence * weight / SCALE;
  for (size_t r = 0U; r < ROWS; ++r) {
    for (size_t c = 0U; c < COLUMNS; ++c) {
      const float dx = (float)c - x, dy = (float)r - y;
      scores[r * COLUMNS + c] =
          peak * expf(-(dx * dx + dy * dy) / (2.0f * 25.0f * 25.0f));
    }
  }
}

static double total(void) {
  double sum = (double)track.none;
  for (size_t q = 0U; q < CELLS; ++q) {
    sum += (double)track.posterior[q];
  }
  return sum;
}

static int start(void) {
  const auris_track_config_t c = auris_track_defaults();
  CHECK(auris_track_init(&track, &c, COLUMNS, ROWS, HOP) == AURIS_OK);
  return EXIT_SUCCESS;
}

static int test_setup(void) {
  auris_track_config_t c = auris_track_defaults();
  CHECK(c.kappa == 0.5f && c.none_prior == 0.5f && c.display_cells == 2.0f);
  CHECK(auris_track_init(&track, &c, COLUMNS, ROWS, HOP) == AURIS_OK);
  CHECK(track.count == CELLS);
  /* 5.8 cells per root second is 0.6 cells per 10.7 ms hop. */
  const float variance = 2.0f * track.share * (float)track.passes;
  CHECK(fabsf(variance - 5.8f * 5.8f * HOP) < 1.0e-4f);
  CHECK(track.share <= 0.2f);
  /* About one redraw every ten seconds. */
  CHECK(fabsf(track.jump - 0.1f * HOP) < 1.0e-5f);
  CHECK(fabs(total() - 1.0) < 1.0e-5 && track.none == 0.5f);
  CHECK(track.kernel_radius == 6U);
  /* A 16 ms hop (the ReSpeaker) needs more diffusion per hop. */
  CHECK(auris_track_init(&track, &c, COLUMNS, ROWS, 0.016f) == AURIS_OK);
  CHECK(fabsf(2.0f * track.share * (float)track.passes - 5.8f * 5.8f * 0.016f) <
        1.0e-4f);
  CHECK(track.passes >= 2U && track.share <= 0.2f);

  CHECK(auris_track_init(NULL, &c, COLUMNS, ROWS, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_init(&track, NULL, COLUMNS, ROWS, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_init(&track, &c, 1U, ROWS, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_init(&track, &c, 100U, 100U, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_init(&track, &c, COLUMNS, ROWS, 0.0f) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  auris_track_config_t bad = c;
  bad.kappa = 0.0f;
  CHECK(auris_track_init(&track, &bad, COLUMNS, ROWS, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  bad = c;
  bad.show = 0.1f; /* below hide */
  CHECK(auris_track_init(&track, &bad, COLUMNS, ROWS, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  bad = c;
  bad.display_cells = 9.0f;
  CHECK(auris_track_init(&track, &bad, COLUMNS, ROWS, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  bad = c;
  bad.none_prior = 1.0f;
  CHECK(auris_track_init(&track, &bad, COLUMNS, ROWS, HOP) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

static int test_prior_and_arguments(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  /* Nothing known: no evidence leaves the prior, and nothing is shown. */
  for (size_t n = 0U; n < 200U; ++n) {
    CHECK(auris_track_step(&track, NULL, 0.0f, 0.0f, &out) == AURIS_OK);
    CHECK(!out.shown && out.strength == 0.0f && out.confidence < 0.06f);
  }
  CHECK(fabs(total() - 1.0) < 1.0e-4);
  CHECK(fabsf(out.none - 0.5f) < 1.0e-3f);
  lobe(30.0f, 20.0f, 20.0f, 0.7f);
  CHECK(auris_track_step(NULL, scores, SCALE, 20.0f, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_step(&track, scores, SCALE, 20.0f, NULL) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_step(&track, scores, 0.0f, 20.0f, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_step(&track, scores, SCALE, -1.0f, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  scores[77] = NAN;
  CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

static int test_converges(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  size_t first_shown = 0U;
  lobe(40.3f, 20.7f, 20.0f, 0.7f);
  for (size_t n = 1U; n <= 40U; ++n) {
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
    CHECK(fabs(total() - 1.0) < 1.0e-4);
    if (out.shown && first_shown == 0U) {
      first_shown = n;
    }
  }
  printf("steady source: shown after %zu frames; estimate (%.2f, %.2f), "
         "confidence %.3f, none %.4f\n",
         first_shown, (double)out.column, (double)out.row,
         (double)out.confidence, (double)out.none);
  CHECK(first_shown >= 1U && first_shown <= 6U); /* within about 60 ms */
  CHECK(out.shown && out.strength == 1.0f && out.confidence > 0.95f);
  CHECK(out.none < 0.01f);
  CHECK(out.best == 21U * COLUMNS + 40U);
  /* The estimate lies between cells, close to the true position. */
  CHECK(fabsf(out.column - 40.3f) < 0.3f && fabsf(out.row - 20.7f) < 0.3f);

  /* Without evidence the estimate stays put and loses confidence slowly:
     0.6 s (56 hops) later it is still drawable. */
  float before = out.confidence;
  for (size_t n = 0U; n < 56U; ++n) {
    CHECK(auris_track_step(&track, NULL, 0.0f, 0.0f, &out) == AURIS_OK);
    CHECK(out.confidence <= before + 1.0e-5f);
    before = out.confidence;
  }
  printf("after 0.6 s without evidence: confidence %.3f at (%.2f, %.2f)\n",
         (double)out.confidence, (double)out.column, (double)out.row);
  CHECK(out.shown && out.best == 21U * COLUMNS + 40U);
  CHECK(out.confidence > 0.5f && out.confidence < 0.99f);
  CHECK(fabs(total() - 1.0) < 1.0e-4);

  auris_track_reset(&track);
  CHECK(auris_track_step(&track, NULL, 0.0f, 0.0f, &out) == AURIS_OK);
  CHECK(!out.shown && out.confidence < 0.06f);
  return EXIT_SUCCESS;
}

/* Frames whose direction is noisy: each frame's lobe is centred several
   cells away from the truth at random. The estimate must be much steadier
   than the frames it is made from. */
static int test_steadies_noisy_frames(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  double sum2 = 0.0, frame2 = 0.0;
  size_t counted = 0U, far = 0U;
  for (size_t n = 0U; n < 600U; ++n) {
    const float ex = 4.0f * normal(), ey = 4.0f * normal();
    lobe(30.0f + ex, 25.0f + ey, 20.0f, 0.65f);
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
    if (n >= 60U) {
      const double dx = (double)out.column - 30.0, dy = (double)out.row - 25.0;
      sum2 += dx * dx + dy * dy;
      frame2 += (double)(ex * ex + ey * ey);
      far += hypot(dx, dy) > 4.0 ? 1U : 0U;
      ++counted;
      CHECK(out.shown);
    }
  }
  const double rms = sqrt(sum2 / (double)counted);
  printf("noisy frames (rms %.2f cells): estimate rms %.2f cells, %zu of "
         "%zu beyond 4 cells\n",
         sqrt(frame2 / (double)counted), rms, far, counted);
  CHECK(rms < 1.6);
  CHECK(far == 0U);
  return EXIT_SUCCESS;
}

/* One frame in five points somewhere else entirely (a strong reflection).
   The estimate must not follow those frames. */
static int test_ignores_outliers(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  double worst = 0.0, sum2 = 0.0;
  size_t counted = 0U;
  for (size_t n = 0U; n < 500U; ++n) {
    if (n % 5U == 4U) {
      lobe(64.0f * uniform(), 48.0f * uniform(), 20.0f, 0.65f);
    } else {
      lobe(22.0f, 30.0f, 20.0f, 0.65f);
    }
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
    if (n >= 40U) {
      const double error = hypot((double)out.column - 22.0,
                                 (double)out.row - 30.0);
      worst = fmax(worst, error);
      sum2 += error * error;
      ++counted;
      CHECK(out.shown);
    }
  }
  /* The stray frames point up to 60 cells away. */
  printf("one frame in five elsewhere: rms error %.2f cells, worst %.2f\n",
         sqrt(sum2 / (double)counted), worst);
  CHECK(sqrt(sum2 / (double)counted) < 1.5 && worst < 4.0);
  return EXIT_SUCCESS;
}

/* Loud sound that is different on every microphone: plenty of weight, but
   each frame's best direction is no better than chance. Nothing may be
   shown, however long it lasts. */
static int test_rejects_incoherent(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  float highest = 0.0f;
  for (size_t n = 0U; n < 1500U; ++n) {
    lobe(64.0f * uniform(), 48.0f * uniform(), 65.0f, 0.04f);
    CHECK(auris_track_step(&track, scores, SCALE, 65.0f, &out) == AURIS_OK);
    highest = fmaxf(highest, out.confidence);
    CHECK(!out.shown);
  }
  printf("incoherent frames: highest confidence %.4f, none %.4f\n",
         (double)highest, (double)out.none);
  CHECK(highest < 0.05f && out.none > 0.95f);
  /* A real source afterwards is still found quickly. */
  size_t needed = 0U;
  lobe(50.0f, 12.0f, 20.0f, 0.7f);
  for (size_t n = 1U; n <= 30U && needed == 0U; ++n) {
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
    if (out.shown && out.best == 12U * COLUMNS + 50U) {
      needed = n;
    }
  }
  printf("source after noise: found in %zu frames\n", needed);
  CHECK(needed >= 1U && needed <= 10U);
  return EXIT_SUCCESS;
}

static int test_jump_and_motion(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  lobe(12.0f, 24.0f, 20.0f, 0.7f);
  for (size_t n = 0U; n < 100U; ++n) {
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
  }
  CHECK(out.best == 24U * COLUMNS + 12U && out.confidence > 0.95f);
  /* The source stops and another starts 40 cells away. */
  lobe(52.0f, 24.0f, 20.0f, 0.7f);
  size_t needed = 0U;
  for (size_t n = 1U; n <= 40U; ++n) {
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
    /* It switches; it does not slide through the places in between. */
    CHECK(out.column < 16.0f || out.column > 48.0f);
    if (needed == 0U && out.shown && fabsf(out.column - 52.0f) < 2.0f) {
      needed = n;
    }
  }
  printf("jump of 40 cells: followed in %zu frames (%.0f ms)\n", needed,
         1000.0 * (double)needed * (double)HOP);
  CHECK(needed >= 1U && needed <= 15U);

  /* A source moving 0.1 cells per hop (about 10 degrees a second). */
  CHECK(start() == EXIT_SUCCESS);
  double worst = 0.0;
  for (size_t n = 0U; n < 400U; ++n) {
    const float x = 10.0f + 0.1f * (float)n;
    lobe(x, 20.0f, 20.0f, 0.7f);
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
    if (n >= 50U) {
      CHECK(out.shown);
      CHECK(out.column <= x + 0.5f); /* behind, never ahead */
      worst = fmax(worst, (double)(x - out.column));
    }
  }
  printf("moving 0.1 cells per hop: largest lag %.2f cells\n", worst);
  CHECK(worst < 4.0);
  return EXIT_SUCCESS;
}

static int test_display(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  lobe(40.0f, 20.0f, 20.0f, 0.7f);
  for (size_t n = 0U; n < 60U; ++n) {
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
  }
  CHECK(auris_track_display(&track, map, CELLS) == AURIS_OK);
  size_t blob = 0U, top = 0U;
  for (size_t q = 0U; q < CELLS; ++q) {
    CHECK(map[q] >= 0.0f && map[q] <= 1.0f);
    blob += map[q] >= 0.55f ? 1U : 0U;
    top = map[q] > map[top] ? q : top;
  }
  printf("drawn blob of a settled estimate: %zu cells (%.1f%% of the grid)\n",
         blob, 100.0 * (double)blob / (double)CELLS);
  CHECK(top == 20U * COLUMNS + 40U && map[top] == 1.0f);
  CHECK(blob >= 12U && blob <= 60U); /* about 1% of the grid */
  CHECK(map[0] < 1.0e-3f);            /* nothing far from the estimate */
  /* At the edge of the grid the blob is as bright as anywhere. */
  CHECK(start() == EXIT_SUCCESS);
  lobe(0.0f, 0.0f, 20.0f, 0.7f);
  for (size_t n = 0U; n < 60U; ++n) {
    CHECK(auris_track_step(&track, scores, SCALE, 20.0f, &out) == AURIS_OK);
  }
  CHECK(out.best == 0U && out.shown);
  CHECK(auris_track_display(&track, map, CELLS) == AURIS_OK);
  CHECK(map[0] == 1.0f);
  CHECK(auris_track_display(&track, map, CELLS - 1U) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_display(&track, NULL, CELLS) ==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_track_display(NULL, map, CELLS) == AURIS_ERROR_INVALID_ARGUMENT);
  return EXIT_SUCCESS;
}

/* Weak evidence (a quiet voice far away): a few bins per frame. It takes
   longer to be sure, and the drawn blob is larger while it is unsure. */
static int test_weak_source(void) {
  CHECK(start() == EXIT_SUCCESS);
  auris_track_result_t out;
  size_t first = 0U;
  lobe(33.0f, 22.0f, 4.0f, 0.7f);
  for (size_t n = 1U; n <= 150U; ++n) {
    CHECK(auris_track_step(&track, scores, SCALE, 4.0f, &out) == AURIS_OK);
    if (out.shown && first == 0U) {
      first = n;
    }
  }
  CHECK(auris_track_display(&track, map, CELLS) == AURIS_OK);
  size_t weak = 0U;
  for (size_t q = 0U; q < CELLS; ++q) {
    weak += map[q] >= 0.55f ? 1U : 0U;
  }
  printf("weak source (4 bins per frame): shown after %zu frames, blob %zu "
         "cells, estimate (%.2f, %.2f)\n",
         first, weak, (double)out.column, (double)out.row);
  CHECK(first >= 3U && first <= 60U);
  CHECK(out.shown && fabsf(out.column - 33.0f) < 1.0f &&
        fabsf(out.row - 22.0f) < 1.0f);
  CHECK(weak >= 20U && weak <= 400U);
  return EXIT_SUCCESS;
}

int main(void) {
  if (test_setup() != EXIT_SUCCESS ||
      test_prior_and_arguments() != EXIT_SUCCESS ||
      test_converges() != EXIT_SUCCESS ||
      test_steadies_noisy_frames() != EXIT_SUCCESS ||
      test_ignores_outliers() != EXIT_SUCCESS ||
      test_rejects_incoherent() != EXIT_SUCCESS ||
      test_jump_and_motion() != EXIT_SUCCESS ||
      test_display() != EXIT_SUCCESS || test_weak_source() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("track tests: ok");
  return EXIT_SUCCESS;
}
