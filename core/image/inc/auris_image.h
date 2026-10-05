#ifndef AURIS_IMAGE_H
#define AURIS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Raw RGGB sensor frames to an RGB picture.

   Each 2 x 2 cell of the sensor (one red, two green, one blue sample)
   becomes one colour sample: black level off, lens shading undone, white
   balance applied, then clipped at white so a lamp stays white. Those
   samples are averaged over the area of each output pixel, so every sensor
   pixel counts toward lowering the noise. The averaged picture is gained to
   the target brightness, colour-corrected and coded with the sRGB curve. */

typedef struct auris_image auris_image_t;

/* Gains that undo the lens's fall-off, on a grid of columns x rows nodes
   that spans the whole sensor: three values (R, G, B) per node, row-major.
   Only their ratios matter. columns 0: no correction. */
typedef struct {
  uint32_t columns, rows;
  const float *gains;
} auris_shading_t;

typedef struct {
  uint32_t source_width, source_height; /* sensor size; both even */
  uint32_t width, height;               /* canvas; the picture is fitted in */
  unsigned int bits;                    /* 8 or 10 bits per sample */
  float black;                          /* black level, in sensor counts */
  /* White balance: used as given, or as the start when it is automatic. */
  float gain[3];
  /* Linear correction to sRGB primaries, after the white balance. An
     all-zero matrix means none. */
  float colour_matrix[3][3];
  float target;      /* mean linear brightness the software gain aims for */
  float max_gain;    /* most the software gain may lift a dark frame */
  float shadow_knee; /* colour fades toward grey below this level; 0: off */
  float contrast;    /* 0..1: how far an S-curve bends the coded levels */
  bool auto_white_balance, auto_exposure;
  auris_shading_t shading;
} auris_image_config_t;

/* What the last frame looked like to the sensor, for exposure control, and
   what was applied to it. Levels are 0..1 of the sensor's range above
   black, of the green samples, before any software gain. */
typedef struct {
  float mean;    /* mean level */
  float bright;  /* 90 % of the picture is below this level */
  float clipped; /* share of the picture at the sensor's limit */
  float gain[3]; /* white balance, green = 1 */
  float exposure; /* software gain */
} auris_image_stats_t;

typedef struct {
  uint32_t x, y, width, height;
} auris_image_rect_t;

/* RGGB samples: uint8_t at 8 bits, little-endian uint16_t at 10. `stride`
   and `bytes` are in bytes. */
typedef struct {
  const void *data;
  uint32_t width, height;
  size_t stride, bytes;
  unsigned int bits;
} auris_bayer_t;

typedef struct {
  uint8_t *data;
  uint32_t width, height;
  size_t stride, bytes;
} auris_rgb24_t;

int auris_image_open(auris_image_t **out, const auris_image_config_t *config);
/* Where the picture sits in the canvas; the rest is black. */
const auris_image_rect_t *auris_image_content(const auris_image_t *image);
int auris_image_stats(const auris_image_t *image, auris_image_stats_t *stats);
/* Looks at a frame without drawing it: the statistics, and the automatic
   white balance and gain, are updated as auris_image_render would. */
int auris_image_measure(auris_image_t *image, const auris_bayer_t *input);
int auris_image_render(auris_image_t *image, const auris_bayer_t *input,
                       auris_rgb24_t *output);
void auris_image_close(auris_image_t *image);
#endif
