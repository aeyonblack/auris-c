#ifndef AURIS_IMAGE_H
#define AURIS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct auris_image auris_image_t;

typedef struct {
  uint32_t source_width, source_height, width, height;
  float black, gain[3], exposure_ev, gamma;
  /* Linear-sRGB correction, applied after white balance and before gamma.
     An all-zero matrix selects identity for backwards compatibility. */
  float colour_matrix[3][3];
  bool auto_white_balance, auto_exposure;
} auris_image_config_t;

typedef struct {
  float gain[3], exposure;
} auris_image_adjustment_t;

typedef struct {
  uint32_t x, y, width, height;
} auris_image_rect_t;

typedef struct {
  const uint8_t *data;
  uint32_t width, height;
  size_t stride, bytes;
} auris_bayer8_t;

typedef struct {
  uint8_t *data;
  uint32_t width, height;
  size_t stride, bytes;
} auris_rgb24_t;

int auris_image_open(auris_image_t **out, const auris_image_config_t *config);
const auris_image_rect_t *auris_image_content(const auris_image_t *image);
int auris_image_get_adjustment(const auris_image_t *image,
                               auris_image_adjustment_t *adjustment);
int auris_image_render(auris_image_t *image, const auris_bayer8_t *input,
                       auris_rgb24_t *output);
void auris_image_close(auris_image_t *image);
#endif
