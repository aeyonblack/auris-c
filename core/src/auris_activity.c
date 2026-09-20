#include "auris_activity.h"

#include <math.h>

auris_activity_config_t auris_activity_defaults(void) {
  return (auris_activity_config_t){.enabled = true,
                                   .low_hz = 300.0f,
                                   .high_hz = 4000.0f,
                                   .open_db = 10.0f,
                                   .close_db = 6.0f,
                                   .minimum_dbfs = -65.0f,
                                   .calibration_s = 1.0f,
                                   .attack_s = 0.032f,
                                   .hold_s = 0.192f,
                                   .noise_up_s = 10.0f,
                                   .noise_down_s = 1.0f};
}

static bool duration_valid(float seconds) {
  return isfinite(seconds) && seconds > 0.0f && seconds <= 60.0f;
}

static size_t frames_for(float seconds, unsigned int rate) {
  /* Small tolerance prevents 32 ms rounding up to three 16 ms hops. */
  return (size_t)fmax(
      1.0, ceil((double)seconds * (double)rate / (double)AURIS_ANALYSIS_HOP -
                1.0e-6));
}

void auris_activity_reset(auris_activity_t *s) {
  if (s != NULL) {
    s->warm_seen = 0U;
    s->attack_seen = 0U;
    s->quiet_seen = 0U;
    s->noise_power = 1.0e-12f;
    s->next_frame = 0U;
    s->started = false;
    s->active = false;
  }
}

auris_status_t auris_activity_init(auris_activity_t *s,
                                   const auris_activity_config_t *c,
                                   unsigned int rate, size_t channels,
                                   float window_energy) {
  if (s == NULL || c == NULL || rate == 0U || channels == 0U ||
      channels > AURIS_AUDIO_CHANNELS || !isfinite(window_energy) ||
      window_energy <= 0.0f || window_energy > (float)AURIS_ANALYSIS_SIZE ||
      !isfinite(c->low_hz) || !isfinite(c->high_hz) || c->low_hz < 0.0f ||
      c->low_hz >= c->high_hz || c->high_hz > (float)rate / 2.0f ||
      !isfinite(c->open_db) || !isfinite(c->close_db) || c->close_db <= 0.0f ||
      c->open_db <= c->close_db || c->open_db > 60.0f ||
      !isfinite(c->minimum_dbfs) || c->minimum_dbfs < -120.0f ||
      c->minimum_dbfs > 0.0f || !duration_valid(c->calibration_s) ||
      !duration_valid(c->attack_s) || !duration_valid(c->hold_s) ||
      !duration_valid(c->noise_up_s) || !duration_valid(c->noise_down_s)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float bin_hz = (float)rate / (float)AURIS_ANALYSIS_SIZE;
  const size_t first = (size_t)fmaxf(1.0f, ceilf(c->low_hz / bin_hz));
  const size_t last =
      (size_t)fminf((float)(AURIS_FFT_BINS - 2U), floorf(c->high_hz / bin_hz));
  if (first > last) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float dt = (float)AURIS_ANALYSIS_HOP / (float)rate;
  *s = (auris_activity_t){.rate = rate,
                          .channels = channels,
                          .enabled = c->enabled,
                          .first_bin = first,
                          .last_bin = last,
                          .warm_frames = frames_for(c->calibration_s, rate),
                          .attack_frames = frames_for(c->attack_s, rate),
                          .hold_frames = frames_for(c->hold_s, rate),
                          .denominator = (float)channels *
                                         (float)AURIS_ANALYSIS_SIZE *
                                         window_energy,
                          .open_ratio = powf(10.0f, c->open_db / 10.0f),
                          .close_ratio = powf(10.0f, c->close_db / 10.0f),
                          .minimum_power = powf(10.0f, c->minimum_dbfs / 10.0f),
                          .noise_up = -expm1f(-dt / c->noise_up_s),
                          .noise_down = -expm1f(-dt / c->noise_down_s)};
  auris_activity_reset(s);
  return AURIS_OK;
}

static float power_db(float power) {
  return 10.0f * log10f(fmaxf(power, 1.0e-12f));
}

auris_status_t auris_activity_apply(auris_activity_t *s,
                                    auris_spectrum_t *spectrum,
                                    auris_activity_result_t *out) {
  if (s == NULL || spectrum == NULL || out == NULL || s->rate == 0U ||
      spectrum->sample_rate != s->rate || spectrum->channels != s->channels) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *out = (auris_activity_result_t){0};
  if (s->started && spectrum->first_frame != s->next_frame) {
    auris_activity_reset(s);
  }
  s->started = true;
  s->next_frame = spectrum->first_frame + (uint64_t)AURIS_ANALYSIS_HOP;
  double energy = 0.0;
  for (size_t m = 0U; m < s->channels; ++m) {
    for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
      if (k < s->first_bin || k > s->last_bin) {
        spectrum->bins[m][k][0] = 0.0f;
        spectrum->bins[m][k][1] = 0.0f;
      } else {
        const double re = (double)spectrum->bins[m][k][0];
        const double im = (double)spectrum->bins[m][k][1];
        energy += re * re + im * im;
      }
    }
  }
  const float power = (float)(2.0 * energy / (double)s->denominator);
  if (!isfinite(power)) {
    auris_activity_reset(s);
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  out->level_dbfs = power_db(power);
  if (!s->enabled) {
    out->noise_dbfs = -120.0f;
    out->open_dbfs = -120.0f;
    out->close_dbfs = -120.0f;
    out->ready = true;
    out->active = true;
    out->evidence = true;
    s->active = true;
    return AURIS_OK;
  }
  const float open = fmaxf(s->minimum_power, s->noise_power * s->open_ratio);
  const float close = fmaxf(s->minimum_power, s->noise_power * s->close_ratio);
  out->noise_dbfs = power_db(s->noise_power);
  out->open_dbfs = power_db(open);
  out->close_dbfs = power_db(close);
  if (s->warm_seen < s->warm_frames) {
    ++s->warm_seen;
    s->noise_power += (power - s->noise_power) / (float)s->warm_seen;
    s->noise_power = fmaxf(s->noise_power, 1.0e-12f);
    out->noise_dbfs = power_db(s->noise_power);
    out->open_dbfs =
        power_db(fmaxf(s->minimum_power, s->noise_power * s->open_ratio));
    out->close_dbfs =
        power_db(fmaxf(s->minimum_power, s->noise_power * s->close_ratio));
    return AURIS_OK;
  }
  out->ready = true;
  out->evidence = power >= close;
  if (s->active) {
    s->quiet_seen = out->evidence ? 0U : s->quiet_seen + 1U;
    if (s->quiet_seen >= s->hold_frames) {
      s->active = false;
      s->attack_seen = 0U;
    }
  } else {
    s->attack_seen = power >= open ? s->attack_seen + 1U : 0U;
    if (s->attack_seen >= s->attack_frames) {
      s->active = true;
      s->quiet_seen = 0U;
    } else if (!out->evidence) {
      const float alpha = power < s->noise_power ? s->noise_down : s->noise_up;
      s->noise_power += alpha * (power - s->noise_power);
      s->noise_power = fmaxf(s->noise_power, 1.0e-12f);
    }
  }
  out->active = s->active;
  return AURIS_OK;
}
