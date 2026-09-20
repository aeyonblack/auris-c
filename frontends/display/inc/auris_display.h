#ifndef AURIS_DISPLAY_H
#define AURIS_DISPLAY_H
#include "auris_output.h"
typedef struct {
  const char *device; /* NULL: discover /dev/dri/card*. */
  uint32_t connector; /* 0: first connected connector. */
  unsigned int rotation; /* clockwise degrees: 0, 90, 180, 270 */
} auris_display_config_t;
typedef struct { uint32_t width,height; unsigned int rotation; } auris_display_info_t;
int auris_output_open_display_info(auris_output_t **out,uint32_t width,uint32_t height,
                                   const auris_display_config_t *config,
                                   auris_display_info_t *info);
int auris_output_open_display(auris_output_t **out, uint32_t width,
                              uint32_t height,
                              const auris_display_config_t *config);
/* Pure conversion shared with regression checks. XRGB8888 values are
   0x00RRGGBB. Fits the rotated image with letterboxing, honours row pitch. */
int auris_display_pack(const auris_rgb24_t *source, uint32_t *destination,
                       uint32_t width, uint32_t height, size_t pitch,
                       size_t capacity, unsigned int rotation);
#endif
