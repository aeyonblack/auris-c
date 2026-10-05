#include "auris_exposure.h"

#include <math.h>
#include <stddef.h>

auris_exposure_config_t auris_exposure_defaults(float line_us,
                                                uint32_t min_lines,
                                                uint32_t max_lines,
                                                float max_analogue,
                                                float max_digital) {
  return (auris_exposure_config_t){.line_us = line_us,
                                   .min_lines = min_lines,
                                   .max_lines = max_lines,
                                   .max_analogue = max_analogue,
                                   .max_digital = max_digital,
                                   .target = 0.16f,
                                   .highlight = 0.9f,
                                   .tolerance = 0.12f,
                                   .step = 0.85f,
                                   .flicker_us = 10000.0f,
                                   .settle_us = 120000U};
}

static bool in_range(float value, float low, float high) {
  return isfinite(value) && value >= low && value <= high;
}

static bool valid(const auris_exposure_config_t *c) {
  return c != NULL && in_range(c->line_us, 0.1f, 1.0e5f) &&
         c->min_lines >= 1U && c->max_lines > c->min_lines &&
         in_range(c->max_analogue, 1.0f, 1000.0f) &&
         in_range(c->max_digital, 1.0f, 1000.0f) &&
         in_range(c->target, 0.01f, 0.9f) &&
         in_range(c->highlight, c->target, 1.0f) &&
         in_range(c->tolerance, 0.0f, 1.0f) && in_range(c->step, 0.05f, 1.0f) &&
         in_range(c->flicker_us, 0.0f, 1.0e5f);
}

float auris_exposure_value(const auris_exposure_config_t *c,
                           auris_exposure_setting_t s) {
  return (float)s.lines * c->line_us * s.analogue * s.digital;
}

auris_exposure_setting_t
auris_exposure_split(const auris_exposure_config_t *c, float value) {
  if (!isfinite(value) || value < 0.0f) {
    value = 0.0f;
  }
  float lines = floorf(value / c->line_us + 0.5f);
  uint32_t most = c->max_lines;
  if (c->flicker_us > 0.0f) {
    const float cycle = c->flicker_us / c->line_us;
    if (lines >= cycle) {
      lines = floorf(floorf(lines / cycle + 1.0e-3f) * cycle + 0.5f);
    }
    /* The longest time that is still whole cycles. */
    const float whole = floorf((float)c->max_lines / cycle);
    if (whole >= 1.0f) {
      most = (uint32_t)floorf(whole * cycle + 0.5f);
    }
  }
  auris_exposure_setting_t s = {.analogue = 1.0f, .digital = 1.0f};
  s.lines = lines < (float)c->min_lines
                ? c->min_lines
                : (lines > (float)most ? most : (uint32_t)lines);
  float rest = value / ((float)s.lines * c->line_us);
  if (rest > 1.0f) {
    s.analogue = rest > c->max_analogue ? c->max_analogue : rest;
    rest /= s.analogue;
    if (rest > 1.0f) {
      s.digital = rest > c->max_digital ? c->max_digital : rest;
    }
  }
  return s;
}

int auris_exposure_init(auris_exposure_t *state,
                        const auris_exposure_config_t *config,
                        const auris_exposure_setting_t *start) {
  if (state == NULL || !valid(config) || start == NULL ||
      start->lines < config->min_lines || start->lines > config->max_lines ||
      !in_range(start->analogue, 1.0f, config->max_analogue + 1.0e-3f) ||
      !in_range(start->digital, 1.0f, config->max_digital + 1.0e-3f)) {
    return -1;
  }
  *state = (auris_exposure_t){.config = *config, .setting = *start};
  return 0;
}

void auris_exposure_applied(auris_exposure_t *state,
                            auris_exposure_setting_t actual, uint64_t now_us) {
  if (state != NULL) {
    state->setting = actual;
    state->ready_us = now_us + state->config.settle_us;
    state->waiting = true;
  }
}

bool auris_exposure_step(auris_exposure_t *state, uint64_t now_us, float mean,
                         float bright, auris_exposure_setting_t *next) {
  if (state == NULL || next == NULL || !isfinite(mean) || !isfinite(bright)) {
    return false;
  }
  const auris_exposure_config_t *c = &state->config;
  if (state->waiting) {
    if (now_us < state->ready_us) {
      return false;
    }
    state->waiting = false;
  }
  /* How much more (or less) light the picture wants. A picture at the
     sensor's limit says only "at least this much less". */
  float ratio = c->target / fmaxf(mean, 1.0e-4f);
  float room = c->highlight / fmaxf(bright, 1.0e-4f);
  if (bright >= 0.97f) {
    room = 0.7f; /* at the limit: how far over is not known */
  }
  if (ratio > room) {
    ratio = room;
  }
  if (fabsf(ratio - 1.0f) <= c->tolerance) {
    return false;
  }
  const float value = auris_exposure_value(c, state->setting);
  const auris_exposure_setting_t wanted =
      auris_exposure_split(c, value * powf(ratio, c->step));
  /* Rounding can leave the same setting, at either limit above all. */
  if (wanted.lines == state->setting.lines &&
      fabsf(wanted.analogue / state->setting.analogue - 1.0f) < 0.02f &&
      fabsf(wanted.digital / state->setting.digital - 1.0f) < 0.02f) {
    return false;
  }
  *next = wanted;
  auris_exposure_applied(state, wanted, now_us);
  return true;
}
