#ifndef AURIS_VPAC_H
#define AURIS_VPAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auris_nv12.h"

/* The TDA4VM's image processor (VPAC): raw sensor frames in, a scaled NV12
   picture out. The ARM cores only copy the frame in; the colour processing
   and scaling are done by the hardware. Needs TI's vision libraries and root
   (they map /dev/mem).

   With `statistics`, the hardware also gathers H3A statistics and TI's
   auto exposure and white balance (2A) run on them. TI's IMX219 tuning sets
   up H3A for 1920-wide frames: any narrower frame (the 1640 x 1232 full
   view included) makes the hardware stop without finishing the frame, and
   the image processor's cores then need a reboot. Without statistics, the
   white balance and exposure come from auris_vpac_set_balance. */
typedef struct auris_vpac auris_vpac_t;

typedef struct {
  uint32_t source_width, source_height;
  size_t source_stride; /* bytes per raw row */
  unsigned int bits;    /* 8, or 10 in little-endian 16-bit words */
  uint32_t width, height; /* the scaled picture; both even */
  /* TI's tuning (DCC) files for the sensor: colour processing, and 2A
     (needed only with statistics). */
  const char *isp_tuning, *aewb_tuning;
  bool statistics;
  /* What TI's auto exposure may ask of the sensor (with statistics). */
  uint32_t min_exposure_us, max_exposure_us;
  float max_analogue;
} auris_vpac_config_t;

/* What 2A made of the frame (with statistics; zero without): the exposure
   it wants the sensor to use next, and the white balance applied. */
typedef struct {
  uint32_t exposure_us;
  uint32_t analogue_q10; /* 1024 = x1 */
  bool exposure_valid;
  float balance_red, balance_blue; /* green = 1 */
  uint32_t colour_temperature;
  uint32_t wb_gains[4]; /* as VISS takes them: R, Gr, Gb, B */
} auris_vpac_result_t;

/* -ENOTSUP when built without TI's libraries, -EPERM when not root. */
int auris_vpac_open(auris_vpac_t **out, const auris_vpac_config_t *config);

/* Processes one raw frame. `picture` points into the processor's memory and
   stays valid until the next call or close. */
int auris_vpac_process(auris_vpac_t *vpac, const void *raw, size_t bytes,
                       auris_nv12_t *picture, auris_vpac_result_t *result);

/* Without statistics: the white balance VISS applies (green = 1), and the
   sensor's exposure and the light's colour temperature, which pick the
   tuning used (noise filtering, colour matrix). Used from the next frame. */
int auris_vpac_set_balance(auris_vpac_t *vpac, float red, float blue,
                           uint32_t exposure_us, float analogue,
                           uint32_t colour_temperature);

void auris_vpac_close(auris_vpac_t *vpac);

#endif
