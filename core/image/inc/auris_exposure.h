#ifndef AURIS_EXPOSURE_H
#define AURIS_EXPOSURE_H

#include <stdbool.h>
#include <stdint.h>

/* Automatic exposure for a sensor with an exposure time in lines, an
   analogue gain and a digital gain. It is given the brightness of each
   processed frame and answers with the next sensor setting, when one is
   needed. It does not touch the sensor itself. */

typedef struct {
  float line_us;       /* one exposure line, in microseconds */
  uint32_t min_lines, max_lines;
  float max_analogue;  /* largest analogue gain, as a multiplier */
  float max_digital;   /* largest digital gain used */
  float target;        /* mean level to aim for, 0..1 of the sensor's range */
  float highlight;     /* 90 % of the picture is kept below this level */
  float tolerance;     /* no change while within this share of the aim */
  float step;          /* share of a correction taken at once, 0..1 */
  float flicker_us;    /* half a mains cycle (10000 at 50 Hz); 0: ignore */
  uint64_t settle_us;  /* a new setting shows in the picture after this */
} auris_exposure_config_t;

typedef struct {
  uint32_t lines;
  float analogue, digital;
} auris_exposure_setting_t;

typedef struct {
  auris_exposure_config_t config;
  auris_exposure_setting_t setting;
  uint64_t ready_us;
  bool waiting;
} auris_exposure_t;

/* Aim 0.16, highlights below 0.9, 12 % tolerance, 85 % steps, 50 Hz mains,
   120 ms for a change to show. */
auris_exposure_config_t auris_exposure_defaults(float line_us,
                                                uint32_t min_lines,
                                                uint32_t max_lines,
                                                float max_analogue,
                                                float max_digital);
/* Returns 0, or -1 for a bad configuration. */
int auris_exposure_init(auris_exposure_t *state,
                        const auris_exposure_config_t *config,
                        const auris_exposure_setting_t *start);
/* Exposure as one number: microseconds times both gains. */
float auris_exposure_value(const auris_exposure_config_t *config,
                           auris_exposure_setting_t setting);
/* The setting for a value: time first, then analogue gain, then digital.
   From flicker_us up, the time is a whole number of half mains cycles, so
   lamps do not put bands across the picture. */
auris_exposure_setting_t
auris_exposure_split(const auris_exposure_config_t *config, float value);
/* One frame taken at now_us: `mean` is its mean level and `bright` the
   level 90 % of it is below, both 0..1 of the sensor's range before any
   software gain. Returns true when `next` should be sent to the sensor. */
bool auris_exposure_step(auris_exposure_t *state, uint64_t now_us, float mean,
                         float bright, auris_exposure_setting_t *next);
/* What the sensor took (it rounds gains), and when it was sent. */
void auris_exposure_applied(auris_exposure_t *state,
                            auris_exposure_setting_t actual, uint64_t now_us);

#endif
