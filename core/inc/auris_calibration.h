#ifndef AURIS_CALIBRATION_H
#define AURIS_CALIBRATION_H
#include "auris_view.h"

#define AURIS_CALIBRATION_MAX_POINTS 32U

typedef struct {
  auris_vec3f_t camera, array;
} auris_calibration_pair_t;

typedef struct {
  float rotation[3][3]; /* camera x-right/y-up/z-forward -> array */
  size_t inliers;
  bool used[AURIS_CALIBRATION_MAX_POINTS];
  double rms_deg, max_deg;
} auris_calibration_result_t;

/* Robust rotation-only fit. Requires >=6 pairs, >=75% consensus, and
   2-D angular spread. Outputs are published only on success. */
int auris_calibration_solve(const auris_calibration_pair_t *pairs, size_t count,
                            double threshold_deg, auris_calibration_result_t *out);
bool auris_calibration_rotation_valid(const float rotation[3][3]);
/* Errors for one correspondence, including points beyond image edges.
   Returns -ERANGE if the prediction is behind the camera. */
int auris_calibration_error(const auris_view_t *camera,
                            auris_calibration_pair_t pair,
                            double *degrees, double *pixels);
#endif
