/* Saves one picture from the imager's camera chain as a PPM, after letting
   the exposure and white balance settle, to compare the image processor
   with the software processing (AURIS_CAMERA_SOFTWARE=1):
     camera_snapshot out.ppm [frames] */
#include "camera_chain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s out.ppm [frames]\n", argv[0]);
    return 2;
  }
  const unsigned long frames = argc > 2 ? strtoul(argv[2], NULL, 10) : 60U;
  enum { W = 640, H = 360 };
  static uint8_t rgb[W * H * 3];
  auris_rgb24_t canvas = {rgb, W, H, W * 3U, sizeof(rgb)};
  camera_chain_t *chain = NULL;
  int err = camera_chain_open(&chain, W, H, "configs/lens_shading.txt");
  if (err == 0)
    err = camera_chain_start(chain);
  for (unsigned long n = 0U; err == 0 && n < frames; ++n) {
    auris_camera_frame_t frame;
    err = camera_chain_acquire(chain, &frame, 1000);
    if (err == -EBADMSG || err == -EAGAIN || err == -EINTR) {
      err = 0;
      continue;
    }
    if (err < 0)
      break;
    err = camera_chain_render(chain, &frame, &canvas);
    const int released = camera_chain_release(chain);
    if (err == 0)
      err = released;
  }
  if (err == 0) {
    camera_chain_status_t seen;
    camera_chain_status(chain, &seen);
    printf("snapshot: %s, exposure %.1f ms x%.2f x%.2f, balance R %.2f B "
           "%.2f, level %.3f\n",
           seen.hardware ? "image processor" : "software",
           (double)seen.exposure_ms, (double)seen.analogue,
           (double)seen.digital, (double)seen.balance_red,
           (double)seen.balance_blue, (double)seen.mean);
    FILE *out = fopen(argv[1], "wb");
    if (out == NULL ||
        fprintf(out, "P6\n%d %d\n255\n", W, H) < 0 ||
        fwrite(rgb, 1U, sizeof(rgb), out) != sizeof(rgb))
      err = -EIO;
    if (out != NULL && fclose(out) != 0)
      err = -EIO;
  }
  camera_chain_close(chain);
  if (err < 0) {
    fprintf(stderr, "snapshot: %s\n", strerror(-err));
    return 1;
  }
  return 0;
}
