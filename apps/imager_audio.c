#include "imager_audio.h"

#include <math.h>
#include <stdint.h>

auris_view_t imager_search_view(void) {
  /* 9.98 pixels a cell at a focal length of 480: 65 columns span 640
     pixels, 57 rows span 560. */
  return (auris_view_t){.width = 640U, .height = 560U,
                        .fx = 480.0f, .fy = 480.0f,
                        .cx = 319.5f, .cy = 279.5f,
                        .rotation = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

int imager_audio_plan(const imager_audio_options_t *o,
                      imager_audio_plan_t *plan) {
  if (o == NULL || plan == NULL ||
      !(o->step == 1U || o->step == 2U || o->step == 4U)) {
    return -EINVAL;
  }
  auris_config_t config;
  if (!auris_config_lookup(o->array, &config)) {
    return -EINVAL;
  }
  const auris_capture_profile_t *profile = config.capture;
  const float nyquist = (float)profile->sample_rate / 2.0f;
  const bool weighting = o->weighting > 0 ||
                         (o->weighting == 0 && profile->snr_weighting);
  float low = weighting ? profile->weighted_low_hz : profile->band_low_hz;
  float high = weighting ? profile->weighted_high_hz : profile->band_high_hz;
  if (o->noise_bias < 0.0f || o->weight_open < 0.0f || o->weight_close < 0.0f ||
      (o->coherence_given &&
       !(o->coherence_min >= 0.0f && o->coherence_min <= 1.0f)) ||
      (o->diffuse_given && !(o->diffuse >= 0.0f && o->diffuse <= 1.0f)) ||
      (o->height_given &&
       !(o->height_split_hz >= 0.0f && o->height_split_hz < nyquist &&
         o->height_keep > 0.0f && o->height_keep <= 1.0f &&
         o->height_share >= 0.0f && o->height_share <= 100.0f)) ||
      o->track_kappa < 0.0f || o->track_drift < 0.0f || o->track_jump < 0.0f ||
      o->track_display < 0.0f || o->hold_s < 0.0f || o->hold_s > 5.0f) {
    return -EINVAL;
  }
  float split = o->height_given ? o->height_split_hz
                                : profile->height_split_hz;
  float keep = o->height_given ? o->height_keep : profile->height_keep;
  float share = o->height_given ? o->height_share : profile->height_share;
  if (o->step != 1U || split == 0.0f || keep == 1.0f) {
    split = 0.0f;
    keep = 1.0f;
    share = 0.0f;
  }
  auris_track_config_t track = auris_track_defaults();
  if (o->track_kappa > 0.0f) track.kappa = o->track_kappa;
  if (o->track_drift > 0.0f) track.drift_cells = o->track_drift;
  if (o->track_jump > 0.0f) track.jump_per_s = o->track_jump;
  if (o->track_display > 0.0f) track.display_cells = o->track_display;
  if (o->band_given) {
    low = o->band_low_hz;
    high = o->band_high_hz;
    if (low == 0.0f && high == 0.0f) {
      high = nyquist;
    }
    if (!(isfinite(low) && isfinite(high) && low >= 0.0f && low < high &&
          high <= nyquist)) {
      return -EINVAL;
    }
  }
  const double history =
      ceil(IMAGER_HISTORY_S * (double)profile->sample_rate /
               (double)profile->frame_hop - 1.0e-6);
  *plan = (imager_audio_plan_t){
      .config = config,
      .device = o->device != NULL ? o->device : profile->device,
      .rate = profile->sample_rate,
      .frame_size = profile->frame_size,
      .frame_hop = profile->frame_hop,
      .upsample = profile->gcc_upsample,
      .step = o->step,
      .history = history > (double)IMAGER_HISTORY_MAX ? IMAGER_HISTORY_MAX
                                                       : (size_t)history,
      .band_low_hz = low,
      .band_high_hz = high,
      .weighting = weighting,
      .diffuse = o->diffuse_given ? o->diffuse : profile->diffuse_rejection,
      .height_split_hz = split,
      .height_keep = keep,
      .height_share = share,
      .coherence_min = o->coherence_given ? o->coherence_min
                                          : profile->coherence_min,
      .noise_bias = o->noise_bias,
      .weight_open = o->weight_open,
      .weight_close = o->weight_close,
      .tracking = o->tracking >= 0,
      .track = track,
      .hold_s = o->hold_s > 0.0f ? o->hold_s : 0.6f,
      .fade_from_s = 0.15f};
  return 0;
}

void imager_dsp_close(imager_dsp_t *dsp) {
  if (dsp != NULL) {
    auris_srp_destroy(dsp->srp);
    auris_fft_destroy(dsp->fft);
    dsp->srp = NULL;
    dsp->fft = NULL;
  }
}

int imager_dsp_open(imager_dsp_t *dsp, const imager_audio_plan_t *plan,
                    const auris_vec3f_t *directions, size_t columns,
                    size_t rows, bool gating, bool smoothing) {
  if (dsp == NULL || plan == NULL || directions == NULL) {
    return -EINVAL;
  }
  *dsp = (imager_dsp_t){.plan = *plan};
  const auris_array_t *array = plan->config.array;
  if (auris_preprocess_init_size(&dsp->prep, plan->frame_size) != AURIS_OK) {
    return -EINVAL;
  }
  float window_energy = 0.0f;
  for (size_t n = 0U; n < plan->frame_size; ++n) {
    window_energy += dsp->prep.window[n] * dsp->prep.window[n];
  }
  /* The gate measures the localization band and zeroes every bin outside
     it, so it is also the band filter. */
  auris_activity_config_t gate = auris_activity_defaults_for(plan->config.capture);
  gate.low_hz = plan->band_low_hz;
  gate.high_hz = plan->band_high_hz;
  gate.enabled = gating;
  gate.weighting = plan->weighting;
  if (plan->noise_bias > 0.0f) gate.noise_bias = plan->noise_bias;
  if (plan->weight_open > 0.0f) gate.weight_open = plan->weight_open;
  if (plan->weight_close > 0.0f) gate.weight_close = plan->weight_close;
  /* With the tracker, an estimate outlives the pauses inside speech: the
     gate holds for hold_s, and the view fades over that time. */
  if (plan->tracking) gate.hold_s = plan->hold_s;
  const float hop_s = (float)plan->frame_hop / (float)plan->rate;
  dsp->cells = columns * rows;
  dsp->srp_columns = columns;
  dsp->quiet_hops = SIZE_MAX / 2U;
  dsp->fade_from_hops = (size_t)lroundf(plan->fade_from_s / hop_s);
  dsp->hold_hops = (size_t)lroundf(plan->hold_s / hop_s);
  if (plan->tracking &&
      (columns < 2U || rows < 2U || columns > AURIS_TRACK_MAX_CELLS / rows ||
       dsp->hold_hops <= dsp->fade_from_hops ||
       auris_track_init(&dsp->track, &plan->track, columns, rows, hop_s) !=
           AURIS_OK)) {
    return -EINVAL;
  }
  if (auris_activity_init_sized(&dsp->activity, &gate, plan->rate,
                                array->microphone_count, plan->frame_size,
                                plan->frame_hop, window_energy) != AURIS_OK) {
    return -EINVAL;
  }
  dsp->fft = auris_fft_create_size(plan->frame_size, plan->upsample);
  dsp->srp = auris_srp_create_sized(array, plan->rate, plan->frame_size,
                                    plan->frame_hop, plan->upsample,
                                    directions, columns * rows);
  if (dsp->fft == NULL || dsp->srp == NULL) {
    imager_dsp_close(dsp);
    return -ENOMEM;
  }
  if (auris_srp_set_grid(dsp->srp, columns, rows, plan->step) != AURIS_OK ||
      auris_srp_set_diffuse_rejection(dsp->srp, plan->diffuse) != AURIS_OK ||
      auris_srp_set_height_band(dsp->srp, plan->height_split_hz,
                                plan->height_keep, plan->height_share) !=
          AURIS_OK ||
      imager_dsp_set_smoothing(dsp, smoothing) != AURIS_OK) {
    imager_dsp_close(dsp);
    return -EINVAL;
  }
  return 0;
}

auris_status_t imager_dsp_set_smoothing(imager_dsp_t *dsp, bool smoothing) {
  if (dsp == NULL || dsp->srp == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  dsp->tracking = smoothing && dsp->plan.tracking;
  dsp->map_ok = false;
  dsp->quiet_hops = SIZE_MAX / 2U;
  if (dsp->plan.tracking) {
    auris_track_reset(&dsp->track);
  }
  /* The tracker integrates over time itself, so it is fed unsmoothed maps. */
  return auris_srp_set_smoothing(
      dsp->srp, smoothing && !dsp->plan.tracking ? 0.100f : 0.0f);
}

auris_status_t imager_dsp_recalibrate(imager_dsp_t *dsp, bool smoothing) {
  if (dsp == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  auris_activity_reset(&dsp->activity);
  return imager_dsp_set_smoothing(dsp, smoothing);
}

/* The tracker's part of one frame: update, then describe what to draw. */
static auris_status_t track_frame(imager_dsp_t *dsp, bool fresh,
                                  const auris_activity_result_t *gate,
                                  const auris_srp_result_t *result,
                                  imager_view_t *view) {
  if (!gate->active) {
    auris_track_reset(&dsp->track);
    dsp->quiet_hops = SIZE_MAX / 2U;
    return AURIS_OK;
  }
  auris_track_result_t estimate;
  const auris_status_t status = auris_track_step(
      &dsp->track, fresh ? result->scores : NULL,
      (float)dsp->plan.frame_size / 2.0f, fresh ? result->weight : 0.0f,
      &estimate);
  if (status != AURIS_OK) {
    return status;
  }
  dsp->quiet_hops = fresh ? 0U
                          : (dsp->quiet_hops < SIZE_MAX / 2U
                                 ? dsp->quiet_hops + 1U
                                 : dsp->quiet_hops);
  float fade = 1.0f;
  if (dsp->quiet_hops >= dsp->hold_hops) {
    fade = 0.0f;
  } else if (dsp->quiet_hops > dsp->fade_from_hops) {
    fade = (float)(dsp->hold_hops - dsp->quiet_hops) /
           (float)(dsp->hold_hops - dsp->fade_from_hops);
  }
  const float strength = estimate.strength * fade;
  view->best = estimate.best;
  view->column = estimate.column;
  view->row = estimate.row;
  view->confidence = estimate.confidence;
  view->strength = strength;
  view->fresh = fresh;
  view->valid = estimate.shown && strength > 0.02f;
  if (view->valid) {
    if (auris_track_display(&dsp->track, dsp->display, dsp->cells) !=
        AURIS_OK) {
      view->valid = false;
    }
    view->map = dsp->display;
  }
  return AURIS_OK;
}

auris_status_t imager_dsp_process(imager_dsp_t *dsp,
                                  auris_analysis_frame_t *frame,
                                  auris_activity_result_t *gate,
                                  auris_srp_result_t *result,
                                  imager_view_t *view) {
  if (dsp == NULL || dsp->srp == NULL || frame == NULL || gate == NULL ||
      result == NULL || view == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *view = (imager_view_t){0};
  auris_status_t status = auris_preprocess_apply(&dsp->prep, frame);
  if (status == AURIS_OK) {
    status = auris_spectrum_apply(dsp->fft, frame, &dsp->spectrum);
  }
  if (status == AURIS_OK) {
    status = auris_activity_apply(&dsp->activity, &dsp->spectrum, gate);
  }
  bool applied = false;
  if (status == AURIS_OK && gate->active && gate->evidence) {
    status = auris_srp_apply(dsp->srp, dsp->fft, &dsp->spectrum, result);
    dsp->map_ok = status == AURIS_OK && result->valid;
    applied = dsp->map_ok;
  }
  if (status != AURIS_OK) {
    return status;
  }
  if (dsp->tracking) {
    return track_frame(dsp, applied, gate, result, view);
  }
  if (!gate->active || !dsp->map_ok) {
    result->valid = false;
    dsp->map_ok = false;
    auris_srp_reset(dsp->srp);
  } else {
    /* A weak map is not shown, but its evidence keeps building. */
    result->valid = result->coherence >= dsp->plan.coherence_min;
  }
  if (result->valid) {
    *view = (imager_view_t){.valid = true,
                            .fresh = gate->evidence,
                            .best = result->best,
                            .column = (float)(result->best % dsp->srp_columns),
                            .row = (float)(result->best / dsp->srp_columns),
                            .strength = 1.0f,
                            .confidence = result->coherence,
                            .map = result->scores};
  }
  return AURIS_OK;
}
