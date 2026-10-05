#include "auris_exposure.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

/* The IMX219 at 1640 x 1232: 18.9 us lines, 4..1759 of them, analogue
   gain to 10.67, digital to 4. */
static auris_exposure_config_t imx219(void) {
  return auris_exposure_defaults(18.904f, 4U, 1759U, 10.6667f, 4.0f);
}

static int test_split(void) {
  const auris_exposure_config_t c = imx219();
  CHECK(c.target > 0.1f && c.target < 0.25f && c.flicker_us == 10000.0f);
  /* Time first. Below 10 ms any length will do. */
  auris_exposure_setting_t s = auris_exposure_split(&c, 2000.0f);
  CHECK(s.lines == 106U && s.analogue == 1.0f && s.digital == 1.0f);
  CHECK(fabsf(auris_exposure_value(&c, s) - 2000.0f) < 20.0f);
  /* From 10 ms the time is whole half-cycles of 50 Hz mains (529 lines
     each), and the analogue gain makes up the rest. */
  s = auris_exposure_split(&c, 15000.0f);
  CHECK(s.lines == 529U && fabsf(s.analogue - 1.5f) < 0.01f &&
        s.digital == 1.0f);
  s = auris_exposure_split(&c, 30000.0f);
  CHECK(s.lines == 1587U && s.analogue == 1.0f);
  /* Then analogue gain to its limit, then digital. */
  s = auris_exposure_split(&c, 30000.0f * 8.0f);
  CHECK(s.lines == 1587U && fabsf(s.analogue - 8.0f) < 0.01f &&
        s.digital == 1.0f);
  s = auris_exposure_split(&c, 30000.0f * 10.6667f * 2.0f);
  CHECK(s.lines == 1587U && fabsf(s.analogue - 10.6667f) < 1.0e-3f &&
        fabsf(s.digital - 2.0f) < 0.01f);
  /* Out of range: the limits. */
  s = auris_exposure_split(&c, 1.0e9f);
  CHECK(s.lines == 1587U && s.analogue == c.max_analogue &&
        s.digital == c.max_digital);
  s = auris_exposure_split(&c, 0.0f);
  CHECK(s.lines == 4U && s.analogue == 1.0f && s.digital == 1.0f);
  /* Without flicker avoidance the whole frame time is used. */
  auris_exposure_config_t free_running = c;
  free_running.flicker_us = 0.0f;
  s = auris_exposure_split(&free_running, 1.0e9f);
  CHECK(s.lines == 1759U);
  s = auris_exposure_split(&free_running, 15000.0f);
  CHECK(s.lines == 793U && fabsf(s.analogue - 1.0f) < 0.01f);
  return EXIT_SUCCESS;
}

static int test_step(void) {
  const auris_exposure_config_t c = imx219();
  auris_exposure_t ae;
  const auris_exposure_setting_t start = {.lines = 1587U, .analogue = 1.0f,
                                          .digital = 1.0f};
  CHECK(auris_exposure_init(&ae, &c, &start) == 0);
  auris_exposure_setting_t next = {0};
  /* Within the tolerance: leave the sensor alone. */
  CHECK(!auris_exposure_step(&ae, 0U, c.target * 1.08f, 0.4f, &next));
  CHECK(!auris_exposure_step(&ae, 1000U, c.target * 0.93f, 0.4f, &next));
  /* Four times too dark: more exposure, most of the way at once. */
  CHECK(auris_exposure_step(&ae, 2000U, c.target / 4.0f, 0.1f, &next));
  const float gained = auris_exposure_value(&c, next) /
                       auris_exposure_value(&c, start);
  CHECK(gained > 2.5f && gained <= 4.01f);
  CHECK(next.lines == 1587U && next.analogue > 2.5f);
  /* The sensor takes a few frames to show it: nothing new until then,
     whatever the picture says. */
  CHECK(!auris_exposure_step(&ae, 2000U + c.settle_us - 1U, 0.01f, 0.02f,
                             &next));
  CHECK(auris_exposure_step(&ae, 2000U + c.settle_us, 0.01f, 0.02f, &next));
  /* What the sensor really took is what the next step starts from. */
  auris_exposure_setting_t actual = next;
  actual.analogue = 3.0f;
  auris_exposure_applied(&ae, actual, 500000U);
  CHECK(!auris_exposure_step(&ae, 500000U + c.settle_us - 1U, 0.9f, 1.0f,
                             &next));
  CHECK(auris_exposure_step(&ae, 500000U + c.settle_us, 0.9f, 1.0f, &next));
  CHECK(auris_exposure_value(&c, next) < auris_exposure_value(&c, actual));

  /* A bright window: the mean is on target but a tenth of the picture is
     at the limit, so the exposure comes down. */
  CHECK(auris_exposure_init(&ae, &c, &start) == 0);
  CHECK(auris_exposure_step(&ae, 0U, c.target, 1.0f, &next));
  CHECK(auris_exposure_value(&c, next) < auris_exposure_value(&c, start));
  /* At a limit there is nothing to change. */
  const auris_exposure_setting_t top = auris_exposure_split(&c, 1.0e9f);
  CHECK(auris_exposure_init(&ae, &c, &top) == 0);
  CHECK(!auris_exposure_step(&ae, 0U, 0.001f, 0.002f, &next));

  CHECK(auris_exposure_init(NULL, &c, &start) == -1);
  auris_exposure_config_t bad = c;
  bad.target = 0.0f;
  CHECK(auris_exposure_init(&ae, &bad, &start) == -1);
  bad = c;
  bad.max_lines = 2U;
  CHECK(auris_exposure_init(&ae, &bad, &start) == -1);
  bad = c;
  bad.step = 1.5f;
  CHECK(auris_exposure_init(&ae, &bad, &start) == -1);
  return EXIT_SUCCESS;
}

/* A scene of fixed brightness watched through a sensor that shows a new
   setting three frames late. `scene` is the level one microsecond of
   exposure at gain one gives. Returns the frames taken to get within 15 %
   of the target and stay there, or -1. */
static int settle(float scene, auris_exposure_setting_t start, float *worst) {
  const auris_exposure_config_t c = imx219();
  auris_exposure_t ae;
  if (auris_exposure_init(&ae, &c, &start) != 0) {
    return -1;
  }
  auris_exposure_setting_t pipe[3] = {start, start, start}, wanted = start;
  int settled = -1, changes = 0;
  *worst = 0.0f;
  for (int frame = 0; frame < 150; ++frame) {
    const uint64_t now = (uint64_t)frame * 33333U;
    const float raw = scene * auris_exposure_value(&c, pipe[0]);
    const float mean = raw > 1.0f ? 1.0f : raw;
    const float bright = 2.5f * raw > 1.0f ? 1.0f : 2.5f * raw;
    const bool good = fabsf(mean / c.target - 1.0f) < 0.15f;
    if (good && settled < 0) settled = frame;
    if (!good && settled >= 0) {
      *worst = fmaxf(*worst, fabsf(mean / c.target - 1.0f));
    }
    pipe[0] = pipe[1];
    pipe[1] = pipe[2];
    pipe[2] = wanted;
    auris_exposure_setting_t next;
    if (auris_exposure_step(&ae, now, mean, bright, &next)) {
      wanted = next;
      auris_exposure_applied(&ae, next, now);
      ++changes;
    }
  }
  printf("scene %.2e: within 15 %% after %d frames, %d changes\n",
         (double)scene, settled, changes);
  return changes > 12 ? -1 : settled;
}

static int test_loop(void) {
  const auris_exposure_config_t c = imx219();
  const auris_exposure_setting_t dim = {.lines = 1587U, .analogue = 1.0f,
                                        .digital = 1.0f};
  float worst = 0.0f;
  /* A lamp-lit room from the power-on setting: about 12 times too dark. */
  int frames = settle(c.target / (30000.0f * 12.0f), dim, &worst);
  CHECK(frames >= 0 && frames <= 30 && worst < 0.2f);
  /* Daylight from a long, high-gain exposure: far too bright. */
  const auris_exposure_setting_t high = auris_exposure_split(&c, 2.0e5f);
  frames = settle(c.target / 500.0f, high, &worst);
  CHECK(frames >= 0 && frames <= 30 && worst < 0.2f);
  /* Already right: nothing happens. */
  frames = settle(c.target / auris_exposure_value(&c, dim), dim, &worst);
  CHECK(frames == 0 && worst == 0.0f);
  return EXIT_SUCCESS;
}

int main(void) {
  int status = test_split();
  if (status == EXIT_SUCCESS) status = test_step();
  if (status == EXIT_SUCCESS) status = test_loop();
  if (status == EXIT_SUCCESS) puts("exposure: ok");
  return status;
}
