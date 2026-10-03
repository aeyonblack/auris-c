#ifndef AURIS_TRACK_H
#define AURIS_TRACK_H

#include "auris_status.h"
#include <stdbool.h>
#include <stddef.h>

/* Recursive Bayesian estimate of one source's direction on a grid of cells.

   Every hop the tracker holds the probability of each cell containing the
   source, plus the probability that there is no localizable source at all.
   Between hops the source may drift a little (the probabilities are blurred)
   or, rarely, reappear anywhere. A frame of SRP evidence then multiplies
   each cell by how well that frame fits a source there. Evidence that is
   no better than unrelated signals would reach by chance favours "no
   source", so loud incoherent noise never builds into a false direction.

   The grid is columns x rows cells, row-major, as auris_view_grid makes it. */

#define AURIS_TRACK_MAX_CELLS 4096U
#define AURIS_TRACK_MAX_KERNEL 33U

typedef struct {
  float kappa;        /* log-likelihood per bin of evidence */
  float drift_cells;  /* random-walk step in cells per square root second */
  float jump_per_s;   /* rate at which the state is redrawn from the prior */
  float cap;          /* largest log-likelihood drop one frame may apply */
  float none_prior;   /* prior probability of "no localizable source" */
  /* Evidence unrelated signals reach by chance, for a frame of weight W
     bins: chance_floor * W + chance_root * sqrt(W). */
  float chance_floor, chance_root;
  size_t confidence_cells; /* half-width of the box summed for confidence */
  size_t mean_cells;       /* half-width of the box averaged for position */
  float show, hide, full;  /* confidence to appear, vanish, be full strength */
  float display_cells;     /* Gaussian radius of the smallest drawn blob */
} auris_track_config_t;

typedef struct {
  size_t best;       /* most probable cell */
  float column, row; /* the estimate between cells: mean around `best` */
  float confidence;  /* probability that the source is near `best` */
  float none;        /* probability of no localizable source */
  float strength;    /* 0..1 from confidence, for drawing */
  bool shown;        /* confident enough to draw (with hysteresis) */
} auris_track_result_t;

typedef struct {
  auris_track_config_t config;
  size_t columns, rows, count;
  size_t passes;   /* diffusion passes per hop */
  float share;     /* each neighbour's share per pass */
  float jump;      /* per hop */
  float none;
  bool shown;
  size_t kernel_radius;
  float kernel[AURIS_TRACK_MAX_KERNEL];
  float posterior[AURIS_TRACK_MAX_CELLS];
  float scratch[AURIS_TRACK_MAX_CELLS];
} auris_track_t;

auris_track_config_t auris_track_defaults(void);
/* hop_seconds is the time between calls to auris_track_step. */
auris_status_t auris_track_init(auris_track_t *track,
                                const auris_track_config_t *config,
                                size_t columns, size_t rows, float hop_seconds);
/* Back to the prior: nothing known. */
void auris_track_reset(auris_track_t *track);
/* One hop. `scores` holds one SRP score per cell and `scale` turns a score
   into bins of evidence (frame size / 2 for auris_srp scores); `weight` is
   the frame's total weight in bins. Pass scores == NULL for a hop without
   evidence: the estimate then only drifts. */
auris_status_t auris_track_step(auris_track_t *track, const float *scores,
                                float scale, float weight,
                                auris_track_result_t *out);
/* The map to draw: the cell probabilities spread by display_cells and
   scaled so the peak is one. Writes columns * rows floats. */
auris_status_t auris_track_display(auris_track_t *track, float *map,
                                   size_t capacity);

#endif
