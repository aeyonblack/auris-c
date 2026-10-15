/* Built when TI's vision libraries are not installed (any machine but the
   BeagleBone): the image processor is never available, so the camera chain
   uses its software processing. */
#include "auris_vpac.h"

#include <errno.h>

int auris_vpac_open(auris_vpac_t **out, const auris_vpac_config_t *config) {
  if (out == NULL || *out != NULL || config == NULL)
    return -EINVAL;
  return -ENOTSUP;
}

int auris_vpac_process(auris_vpac_t *vpac, const void *raw, size_t bytes,
                       auris_nv12_t *picture, auris_vpac_result_t *result) {
  (void)vpac;
  (void)raw;
  (void)bytes;
  (void)picture;
  (void)result;
  return -ENOTSUP;
}

void auris_vpac_close(auris_vpac_t *vpac) { (void)vpac; }
