#ifndef CAMERA_CHAIN_H
#define CAMERA_CHAIN_H

#include "auris_camera.h"
#include "auris_exposure.h"
#include "auris_image.h"
#include "auris_vpac.h"

/* The imager's camera: the IMX219 on the BeagleBone's CSI port at
   1640 x 1232 (the whole field of view), 10 bits a sample, turned into an
   RGB picture, with the sensor's exposure steered from each picture. Both
   camera_scope and imager_scope use it, so the camera is tuned in one
   place.

   The picture is made by the chip's image processor (VPAC) with TI's
   tuning when it can be (TI's libraries, root, the tuning file in
   /opt/imaging/imx219); otherwise, or with AURIS_CAMERA_SOFTWARE=1, by the
   software processing in auris_image. Either way the exposure and white
   balance come from auris_image's measurements. */

typedef struct camera_chain camera_chain_t;

typedef struct {
  float exposure_ms, analogue, digital; /* the sensor's setting */
  float software;                       /* gain applied to the picture */
  float balance_red, balance_blue;      /* white balance, green = 1 */
  float mean, clipped;                  /* level seen, share at the limit */
  bool exposure_control, shading;       /* what is in use */
  bool hardware;                        /* the image processor makes it */
} camera_chain_status_t;

#define CAMERA_SHADING_MAX_NODES (64U * 64U)

/* Reads a lens-shading table: "AURISSHADE1 columns rows" and then
   columns x rows x 3 gains (R G B per node, row-major). Returns 0, -ENOENT
   when there is no file, or -EINVAL for anything malformed. `gains` must
   hold CAMERA_SHADING_MAX_NODES * 3 floats. */
int camera_chain_load_shading(const char *path, uint32_t *columns,
                              uint32_t *rows, float *gains);

/* Where the image processor's picture goes on the canvas: the software
   fit (auris_image_fit), with width and height made even for NV12. */
int camera_chain_hardware_rect(uint32_t source_width, uint32_t source_height,
                               uint32_t width, uint32_t height,
                               auris_image_rect_t *rect);

/* Whether to try the image processor, given AURIS_CAMERA_SOFTWARE. */
bool camera_chain_wants_hardware(const char *software);

/* width x height is the canvas the picture is fitted into. shading_path
   may be NULL; a missing file means no correction. */
int camera_chain_open(camera_chain_t **out, uint32_t width, uint32_t height,
                      const char *shading_path);
const auris_camera_format_t *camera_chain_format(const camera_chain_t *chain);
const auris_image_rect_t *camera_chain_content(const camera_chain_t *chain);
int camera_chain_start(camera_chain_t *chain);
int camera_chain_acquire(camera_chain_t *chain, auris_camera_frame_t *frame,
                         int timeout_ms);
/* Draws the held frame into the canvas and, from what it measured, sets
   the sensor's exposure for the frames to come. */
int camera_chain_render(camera_chain_t *chain,
                        const auris_camera_frame_t *frame,
                        auris_rgb24_t *canvas);
int camera_chain_release(camera_chain_t *chain);
void camera_chain_status(const camera_chain_t *chain,
                         camera_chain_status_t *status);
void camera_chain_close(camera_chain_t *chain);

#endif
