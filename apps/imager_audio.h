#ifndef IMAGER_AUDIO_H
#define IMAGER_AUDIO_H

#include "auris_activity.h"
#include "auris_array_configs.h"
#include "auris_srp.h"
#include "auris_track.h"
#include "auris_view.h"
#include <errno.h>

/* The imager's search grid, row-major from the top left. Sideways it spans
   the camera picture plus 2.6 degrees each side. Above and below it reaches
   about 6 degrees past the picture: a sound or an echo just outside the
   picture is then placed outside it and not drawn, where a grid that ended
   at the picture would pin it to the edge. Reaching further was tried and
   made voices drop out: their low bins place height too loosely. */
#define IMAGER_COLUMNS 65U
#define IMAGER_ROWS 57U
#define IMAGER_DIRECTIONS (IMAGER_COLUMNS * IMAGER_ROWS)

/* Acoustic maps the imager keeps for matching against camera frames: the
   last 512 ms (32 maps at the ReSpeaker's 16 ms hops, 48 at the CMB's). */
#define IMAGER_HISTORY_S 0.512
#define IMAGER_HISTORY_MAX 64U

typedef struct {
  const char *array;  /* registry name, e.g. "cmb" */
  const char *device; /* NULL: the profile's device */
  bool band_given;    /* false: the profile's band; 0 0: every bin */
  float band_low_hz, band_high_hz;
  size_t step; /* coarse-to-fine search step: 1 (exhaustive), 2 or 4 */
  /* Tuning overrides; zero keeps the profile's or the gate's default. */
  int weighting;        /* 1 forces SNR weighting on, -1 off */
  bool diffuse_given;   /* false: the profile's diffuse_rejection */
  float diffuse;        /* 0 (off) to 1 */
  bool coherence_given; /* false: the profile's coherence_min */
  float coherence_min;
  float noise_bias, weight_open, weight_close;
  bool height_given; /* false: the profile's height band; 0 Hz: off */
  float height_split_hz, height_keep, height_share;
  /* Tracking (Module 28). -1 turns the tracker off and goes back to the
     0.1 s smoothing of the SRP curves. Zero keeps a value's default. */
  int tracking;
  float track_kappa, track_drift, track_jump, track_display, hold_s;
} imager_audio_options_t;

/* Everything the audio side needs, resolved from the array's profile. */
typedef struct {
  auris_config_t config;
  const char *device;
  unsigned int rate;
  size_t frame_size, frame_hop, upsample, step, history;
  float band_low_hz, band_high_hz;
  bool weighting;      /* per-bin SNR weights; the gate decides on them */
  float diffuse;       /* 0..1: pairs discount bins the room dominates */
  /* Bins below height_split_hz place the column; their say between rows
     falls to height_keep as the bins above reach height_share of their
     weight. 0 Hz: off. Always off for a coarse search. */
  float height_split_hz, height_keep, height_share;
  float coherence_min; /* maps below this SRP coherence are not shown */
  float noise_bias, weight_open, weight_close; /* 0: the gate's defaults */
  bool tracking;             /* Bayesian tracker instead of curve smoothing */
  auris_track_config_t track;
  float hold_s;      /* how long an estimate is kept without new evidence */
  float fade_from_s; /* it is drawn at full strength this long, then fades */
} imager_audio_plan_t;

/* What to draw for one analysis frame. */
typedef struct {
  bool valid;        /* there is a map to draw */
  bool fresh;        /* this frame added evidence to it */
  size_t best;       /* cell of the estimate */
  float column, row; /* the estimate in grid coordinates, between cells */
  float strength;    /* 0..1: confidence, fading while the estimate is held */
  float confidence;  /* probability that the source is near the estimate */
  const float *map;  /* one value per cell; valid until the next frame */
} imager_view_t;

/* The DSP chain: window, FFT, activity gate (which also applies the band)
   and SRP over the imager's search grid. Owned by the audio worker. */
typedef struct {
  imager_audio_plan_t plan;
  auris_fft_t *fft;
  auris_srp_t *srp;
  auris_preprocess_t prep;
  auris_activity_t activity;
  auris_spectrum_t spectrum;
  bool map_ok; /* the last SRP map was usable, whether or not it is shown */
  bool tracking;         /* the tracker is in use (smoothing on) */
  auris_track_t track;
  size_t cells;          /* columns x rows */
  size_t srp_columns;
  size_t quiet_hops;     /* hops since the tracker last had evidence */
  size_t fade_from_hops, hold_hops;
  float display[AURIS_TRACK_MAX_CELLS]; /* the tracker's map to draw */
} imager_dsp_t;

/* The view whose IMAGER_COLUMNS x IMAGER_ROWS grid (auris_view_grid) the
   imager searches: cells 1.19 degrees apart at the centre, array axes. */
auris_view_t imager_search_view(void);
/* Returns 0, or -EINVAL for an unknown array, bad band or bad step. */
int imager_audio_plan(const imager_audio_options_t *options,
                      imager_audio_plan_t *plan);
/* Builds the chain for `columns` x `rows` directions. Returns 0, -EINVAL
   or -ENOMEM; on failure nothing is left allocated. */
int imager_dsp_open(imager_dsp_t *dsp, const imager_audio_plan_t *plan,
                    const auris_vec3f_t *directions, size_t columns,
                    size_t rows, bool gating, bool smoothing);
/* One analysis frame: gate it and, while there is evidence, localize it.
   `view` says what to draw. With tracking, each frame's SRP map updates
   the tracker and the view is the tracker's estimate, kept and faded for
   hold_s after the last evidence. Without it, the view is the SRP map
   itself (smoothed over 0.1 s when smoothing is on).
   `result` is the SRP result of the last frame that had evidence. */
auris_status_t imager_dsp_process(imager_dsp_t *dsp,
                                  auris_analysis_frame_t *frame,
                                  auris_activity_result_t *gate,
                                  auris_srp_result_t *result,
                                  imager_view_t *view);
/* Turns smoothing (the tracker, when the plan has one) on or off and
   forgets what was accumulated. */
auris_status_t imager_dsp_set_smoothing(imager_dsp_t *dsp, bool smoothing);
void imager_dsp_close(imager_dsp_t *dsp);

#endif
