#include "auris_array.h"
#include "auris_array_configs.h"
#include "auris_audio.h"
#include "auris_camera_calibration.h"
#include "auris_srp.h"

#include <math.h>
#include <stdbool.h>
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

#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (double)(tol))

/* Source 1: fab pick-and-place, front-face view, millimetres (MIC1..MIC8).
   Rounded to 0.01 mm by the CAD export. */
static const double FRONT_MM[8][2] = {
    {-52.0, 0.0},     {-36.77, 36.77},  {0.0, 52.0},   {36.77, 36.77},
    {52.0, 0.0},      {36.77, -36.77},  {0.0, -52.0},  {-36.77, -36.77}};

/* Source 2: McASP slot order delivered by ALSA, from the wiring reference:
   ALSA channel n carries microphone ALSA_MIC[n] (1-based). */
static const size_t ALSA_MIC[8] = {1U, 4U, 5U, 8U, 2U, 3U, 6U, 7U};

int main(void) {
  const auris_array_t *cmb = auris_config_cmb_8();
  const auris_array_t *respeaker = auris_config_respeaker_usb_4();
  const double pi = 3.14159265358979323846;
  const double r = 0.052;

  /* Lookup by name. */
  CHECK(auris_config_find("cmb") == cmb);
  CHECK(auris_config_find("respeaker") == respeaker);
  CHECK(auris_config_find("CMB") == NULL);
  CHECK(auris_config_find("") == NULL);
  CHECK(auris_config_find(NULL) == NULL);

  /* Basic shape. */
  CHECK(auris_array_validate(cmb) == AURIS_OK);
  CHECK(cmb->microphone_count == 8U);
  CHECK(cmb->microphone_count <= AURIS_AUDIO_CHANNELS);
  CHECK(cmb->speed_of_sound_mps == 343.0F);

  for (size_t m = 0U; m < 8U; ++m) {
    const auris_vec3f_t p = cmb->positions[m];
    /* Mirror rule: viewed from behind, x flips and y is unchanged. */
    CHECK(NEAR(p.x_m, -FRONT_MM[m][0] / 1000.0, 1.0e-5));
    CHECK(NEAR(p.y_m, FRONT_MM[m][1] / 1000.0, 1.0e-5));
    CHECK(p.z_m == 0.0F);
    /* Exact ring: radius 52 mm, MICk at 45 * (k - 1) degrees. */
    const double angle = 2.0 * pi * (double)m / 8.0;
    CHECK(NEAR(p.x_m, r * cos(angle), 1.0e-6));
    CHECK(NEAR(p.y_m, r * sin(angle), 1.0e-6));
  }

  /* Orientation: MIC1 right, MIC3 up, MIC5 left, MIC7 down (from behind),
     and the numbering runs counter-clockwise (positive cross product). */
  CHECK(cmb->positions[0].x_m > 0.05F && cmb->positions[2].y_m > 0.05F);
  CHECK(cmb->positions[4].x_m < -0.05F && cmb->positions[6].y_m < -0.05F);
  for (size_t m = 0U; m < 8U; ++m) {
    const auris_vec3f_t a = cmb->positions[m];
    const auris_vec3f_t b = cmb->positions[(m + 1U) % 8U];
    CHECK(a.x_m * b.y_m - a.y_m * b.x_m > 0.0F);
  }

  /* Channel map: a permutation of 0..7 that agrees with the slot table. */
  bool seen[8] = {false};
  for (size_t m = 0U; m < 8U; ++m) {
    CHECK(cmb->capture_channels[m] < 8U);
    CHECK(!seen[cmb->capture_channels[m]]);
    seen[cmb->capture_channels[m]] = true;
  }
  for (size_t ch = 0U; ch < 8U; ++ch) {
    CHECK(cmb->capture_channels[ALSA_MIC[ch] - 1U] == ch);
  }

  /* Pairs: 28, in four chord classes 2 r sin(k pi / 8). */
  auris_mic_pair_t pairs[AURIS_MAX_PAIRS];
  size_t count = 0U;
  size_t per_class[5] = {0U};
  CHECK(auris_array_generate_pairs(cmb, pairs, AURIS_MAX_PAIRS, &count) ==
        AURIS_OK);
  CHECK(count == 28U);
  for (size_t p = 0U; p < count; ++p) {
    const size_t gap = pairs[p].second - pairs[p].first;
    const size_t steps = gap <= 4U ? gap : 8U - gap;
    ++per_class[steps];
    CHECK(NEAR(pairs[p].distance_m, 2.0 * r * sin((double)steps * pi / 8.0),
               2.0e-6));
  }
  CHECK(per_class[1] == 8U && per_class[2] == 8U && per_class[3] == 8U &&
        per_class[4] == 4U);

  float minimum = 0.0F, aperture = 0.0F, alias = 0.0F, lag = 0.0F;
  CHECK(auris_array_spacing(cmb, &minimum, &aperture) == AURIS_OK);
  CHECK(NEAR(minimum, 0.0397991, 2.0e-6));
  CHECK(NEAR(aperture, 0.104, 2.0e-6));
  CHECK(auris_array_aliasing_estimate_hz(cmb, &alias) == AURIS_OK);
  CHECK(NEAR(alias, 4309.145, 0.5));

  /* Pair (0,4) is MIC1-MIC5, a diameter. */
  CHECK(pairs[3].first == 0U && pairs[3].second == 4U);
  CHECK(auris_pair_max_tdoa_samples(&pairs[3], cmb->speed_of_sound_mps,
                                    48000.0F, &lag) == AURIS_OK);
  CHECK(NEAR(lag, 14.553936, 1.0e-3));
  CHECK(auris_pair_max_tdoa_samples(&pairs[3], cmb->speed_of_sound_mps,
                                    16000.0F, &lag) == AURIS_OK);
  CHECK(NEAR(lag, 4.851312, 1.0e-3));

  /* Calibration files are keyed by geometry and channel map. */
  const uint64_t cmb_id = auris_calibration_array_id(cmb);
  const uint64_t respeaker_id = auris_calibration_array_id(respeaker);
  CHECK(cmb_id != 0U && respeaker_id != 0U && cmb_id != respeaker_id);

  /* The existing SRP accepts the geometry at 48 kHz (lags < 255 samples). */
  auris_vec3f_t ring[360];
  for (size_t q = 0U; q < 360U; ++q) {
    const float a = (float)q * 0.017453292F;
    ring[q] = (auris_vec3f_t){cosf(a), sinf(a), 0.0F};
  }
  auris_srp_t *srp = auris_srp_create(cmb, 48000U, ring, 360U);
  CHECK(srp != NULL);
  auris_srp_destroy(srp);

  /* A deliberately broken copy must be rejected. */
  auris_array_t broken = *cmb;
  broken.capture_channels[7] = broken.capture_channels[0];
  CHECK(auris_array_validate(&broken) == AURIS_ERROR_INVALID_ARGUMENT);

  puts("CMB array tests: ok");
  return EXIT_SUCCESS;
}
