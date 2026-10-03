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
                                   .noise_down_s = 1.0f,
                                   .weighting = false,
                                   .noise_bias = 2.5f,
                                   .weight_open = 3.0f,
                                   .weight_close = 1.5f,
                                   .noise_bin_s = 2.0f};
}

static bool duration_valid(float seconds) {
  return isfinite(seconds) && seconds > 0.0f && seconds <= 60.0f;
}

auris_activity_config_t
auris_activity_defaults_for(const auris_capture_profile_t *profile) {
  auris_activity_config_t c = auris_activity_defaults();
  if (profile != NULL) {
    c.low_hz = profile->band_low_hz;
    c.high_hz = profile->band_high_hz;
    c.minimum_dbfs = profile->gate_minimum_dbfs;
    c.weighting = profile->snr_weighting;
    if (c.weighting) {
      c.low_hz = profile->weighted_low_hz;
    }
  }
  return c;
}

static size_t frames_for(float seconds, unsigned int rate, size_t hop) {
  /* Small tolerance prevents 32 ms rounding up to an extra hop. */
  return (size_t)fmax(
      1.0, ceil((double)seconds * (double)rate / (double)hop - 1.0e-6));
}

void auris_activity_reset(auris_activity_t *s) {
  if (s != NULL) {
    s->warm_seen = 0U;
    s->attack_seen = 0U;
    s->quiet_seen = 0U;
    s->noise_power = 1.0e-12f;
    for (size_t k = 0U; k < AURIS_FFT_MAX_BINS; ++k) {
      s->noise_bins[k] = 0.0f;
      s->weights[k] = 0.0f;
    }
    s->next_frame = 0U;
    s->started = false;
    s->active = false;
  }
}

auris_status_t auris_activity_init(auris_activity_t *s,
                                   const auris_activity_config_t *c,
                                   unsigned int rate, size_t channels,
                                   float window_energy) {
  return auris_activity_init_sized(s, c, rate, channels, AURIS_ANALYSIS_SIZE,
                                   AURIS_ANALYSIS_HOP, window_energy);
}

auris_status_t auris_activity_init_sized(auris_activity_t *s,
                                         const auris_activity_config_t *c,
                                         unsigned int rate, size_t channels,
                                         size_t size, size_t hop,
                                         float window_energy) {
  if (s == NULL || c == NULL || rate == 0U || channels == 0U ||
      channels > AURIS_AUDIO_CHANNELS || !auris_analysis_size_valid(size, hop) ||
      !isfinite(window_energy) || window_energy <= 0.0f ||
      window_energy > (float)size ||
      !isfinite(c->low_hz) || !isfinite(c->high_hz) || c->low_hz < 0.0f ||
      c->low_hz >= c->high_hz || c->high_hz > (float)rate / 2.0f ||
      !isfinite(c->open_db) || !isfinite(c->close_db) || c->close_db <= 0.0f ||
      c->open_db <= c->close_db || c->open_db > 60.0f ||
      !isfinite(c->minimum_dbfs) || c->minimum_dbfs < -120.0f ||
      c->minimum_dbfs > 0.0f || !duration_valid(c->calibration_s) ||
      !duration_valid(c->attack_s) || !duration_valid(c->hold_s) ||
      !duration_valid(c->noise_up_s) || !duration_valid(c->noise_down_s) ||
      (c->weighting &&
       (!isfinite(c->noise_bias) || c->noise_bias < 1.0f ||
        c->noise_bias > 100.0f || !isfinite(c->weight_open) ||
        !isfinite(c->weight_close) || c->weight_close <= 0.0f ||
        c->weight_open < c->weight_close || !duration_valid(c->noise_bin_s)))) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float bin_hz = (float)rate / (float)size;
  const float first = fmaxf(1.0f, ceilf(c->low_hz / bin_hz));
  const float last =
      fminf((float)(size / 2U - 1U), floorf(c->high_hz / bin_hz));
  if (first > last) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float dt = (float)hop / (float)rate;
  *s = (auris_activity_t){.rate = rate,
                          .channels = channels,
                          .size = size,
                          .hop = hop,
                          .enabled = c->enabled,
                          .first_bin = (size_t)first,
                          .last_bin = (size_t)last,
                          .warm_frames = frames_for(c->calibration_s, rate, hop),
                          .attack_frames = frames_for(c->attack_s, rate, hop),
                          .hold_frames = frames_for(c->hold_s, rate, hop),
                          .denominator =
                              (float)channels * (float)size * window_energy,
                          .open_ratio = powf(10.0f, c->open_db / 10.0f),
                          .close_ratio = powf(10.0f, c->close_db / 10.0f),
                          .minimum_power = powf(10.0f, c->minimum_dbfs / 10.0f),
                          .noise_up = -expm1f(-dt / c->noise_up_s),
                          .noise_down = -expm1f(-dt / c->noise_down_s),
                          .weighting = c->weighting,
                          .noise_bias = c->noise_bias,
                          .weight_open = c->weight_open,
                          .weight_close = c->weight_close,
                          .bin_alpha = c->weighting
                                           ? -expm1f(-dt / c->noise_bin_s)
                                           : 0.0f};
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
      spectrum->sample_rate != s->rate || spectrum->channels != s->channels ||
      spectrum->size != s->size) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *out = (auris_activity_result_t){0};
  if (s->started && spectrum->first_frame != s->next_frame) {
    auris_activity_reset(s);
  }
  s->started = true;
  s->next_frame = spectrum->first_frame + (uint64_t)s->hop;
  /* Per-bin power, averaged over the microphones (raw FFT units: only its
     ratio to the learned noise is used). Reuses the weights array until
     the weights themselves are computed below. */
  float *bin_power = s->weights;
  double energy = 0.0;
  for (size_t k = 0U; k <= s->size / 2U; ++k) {
    double bin = 0.0;
    for (size_t m = 0U; m < s->channels; ++m) {
      if (k < s->first_bin || k > s->last_bin) {
        spectrum->bins[m][k][0] = 0.0f;
        spectrum->bins[m][k][1] = 0.0f;
      } else {
        const double re = (double)spectrum->bins[m][k][0];
        const double im = (double)spectrum->bins[m][k][1];
        bin += re * re + im * im;
      }
    }
    energy += bin;
    bin_power[k] = (float)(bin / (double)s->channels);
  }
  const float power = (float)(2.0 * energy / (double)s->denominator);
  if (!isfinite(power)) {
    auris_activity_reset(s);
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  out->level_dbfs = power_db(power);
  if (!s->enabled && !s->weighting) {
    out->noise_dbfs = -120.0f;
    out->open_dbfs = -120.0f;
    out->close_dbfs = -120.0f;
    out->ready = true;
    out->active = true;
    out->evidence = true;
    s->active = true;
    return AURIS_OK;
  }
  if (s->weighting) {
    spectrum->weights = s->weights;
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
    for (size_t k = s->first_bin; s->weighting && k <= s->last_bin; ++k) {
      s->noise_bins[k] += (bin_power[k] - s->noise_bins[k]) /
                          (float)s->warm_seen;
    }
    for (size_t k = 0U; k <= s->size / 2U; ++k) {
      s->weights[k] = 0.0f; /* nothing is known about the noise yet */
    }
    out->noise_dbfs = power_db(s->noise_power);
    out->open_dbfs =
        power_db(fmaxf(s->minimum_power, s->noise_power * s->open_ratio));
    out->close_dbfs =
        power_db(fmaxf(s->minimum_power, s->noise_power * s->close_ratio));
    if (!s->enabled) { /* gate off: every frame passes, unweighted so far */
      out->ready = out->active = out->evidence = true;
      spectrum->weights = NULL;
    }
    return AURIS_OK;
  }
  out->ready = true;
  bool above_open = power >= open;
  out->evidence = power >= close;
  if (s->weighting) {
    /* The noise update below needs this frame's powers, so it runs before
       they are replaced by the weights. Decide first, on a first pass. */
    float sum = 0.0f;
    for (size_t k = s->first_bin; k <= s->last_bin; ++k) {
      const float floor = s->noise_bias * fmaxf(s->noise_bins[k], 1.0e-30f);
      const float xi = fmaxf(bin_power[k] / floor - 1.0f, 0.0f);
      sum += xi / (1.0f + xi);
    }
    out->weight_sum = sum;
    above_open = sum >= s->weight_open;
    out->evidence = sum >= s->weight_close;
  }
  bool learn = false;
  if (s->active) {
    s->quiet_seen = out->evidence ? 0U : s->quiet_seen + 1U;
    if (s->quiet_seen >= s->hold_frames) {
      s->active = false;
      s->attack_seen = 0U;
    }
  } else {
    s->attack_seen = above_open ? s->attack_seen + 1U : 0U;
    if (s->attack_seen >= s->attack_frames) {
      s->active = true;
      s->quiet_seen = 0U;
    } else if (!out->evidence) {
      const float alpha = power < s->noise_power ? s->noise_down : s->noise_up;
      s->noise_power += alpha * (power - s->noise_power);
      s->noise_power = fmaxf(s->noise_power, 1.0e-12f);
      learn = true;
    }
  }
  if (s->weighting) {
    for (size_t k = 0U; k <= s->size / 2U; ++k) {
      if (k < s->first_bin || k > s->last_bin) {
        s->weights[k] = 0.0f;
        continue;
      }
      const float p = bin_power[k];
      const float floor = s->noise_bias * fmaxf(s->noise_bins[k], 1.0e-30f);
      const float xi = fmaxf(p / floor - 1.0f, 0.0f);
      if (learn) {
        s->noise_bins[k] += s->bin_alpha * (p - s->noise_bins[k]);
      }
      s->weights[k] = xi / (1.0f + xi); /* overwrites bin_power[k] */
    }
  }
  out->active = s->active;
  if (!s->enabled) { /* gate off: every frame passes, still weighted */
    out->active = true;
    out->evidence = true;
  }
  return AURIS_OK;
}
