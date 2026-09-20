#ifdef AURIS_CAMERA_AVAILABLE
#include "auris_camera.h"
#include <errno.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); goto done; } } while (0)

int main(int argc,char **argv) {
  int result=EXIT_FAILURE;
  auris_camera_t *camera=NULL;
  auris_camera_frame_t frame;
  auris_camera_config_t config={.sensor="/dev/v4l-subdev2",.bridge="/dev/v4l-subdev1",
    .device="/dev/null",.width=1640U,.height=1232U,.ti_5_10_packed=true};
  CHECK(auris_camera_open(NULL,&config)==-EINVAL);
  CHECK(auris_camera_open(&camera,NULL)==-EINVAL);
  CHECK(auris_camera_open(&camera,&config)==-ENOTTY && camera==NULL);
  CHECK(auris_camera_start(NULL)==-EINVAL);
  CHECK(auris_camera_release(NULL)==-EINVAL);
  CHECK(auris_camera_acquire(NULL,&frame,100)==-EINVAL);
  CHECK(auris_camera_format(NULL)==NULL);
  auris_camera_close(NULL);
  config.width=0U;
  CHECK(auris_camera_open(&camera,&config)==-EINVAL);
  if (argc == 2 && strcmp(argv[1],"--hardware")==0) {
    config.width=1640U;
    config.device="/dev/v4l/by-path/platform-4510000.ticsi2rx-video-index0";
    config.ti_5_10_packed=false;
    CHECK(auris_camera_open(&camera,&config)==-EPROTO && camera==NULL);
    config.ti_5_10_packed=true;
    /* End on the chosen full-view mode. */
    const uint32_t widths[]={1920U,1640U};
    const uint32_t heights[]={1080U,1232U};
    for (size_t mode=0U;mode<2U;++mode) {
      config.width=widths[mode]; config.height=heights[mode];
      CHECK(auris_camera_open(&camera,&config)==0);
      CHECK(auris_camera_open(&camera,&config)==-EINVAL);
      CHECK(auris_camera_release(camera)==-EINVAL);
      CHECK(auris_camera_acquire(camera,&frame,10)==-EPIPE);
      CHECK(auris_camera_start(camera)==0);
      CHECK(auris_camera_start(camera)==-EBUSY);
      CHECK(auris_camera_acquire(camera,&frame,-1)==-EINVAL);
      uint64_t first=0U,last=0U;
      uint32_t previous=0U;
      for (size_t n=0U;n<120U;++n) {
        CHECK(auris_camera_acquire(camera,&frame,2000)==0);
        CHECK(frame.data!=NULL && frame.bytes==(size_t)config.width*config.height);
        CHECK((frame.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK)==V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);
        CHECK((frame.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK)==V4L2_BUF_FLAG_TSTAMP_SRC_EOF);
        if(n==0U) first=frame.timestamp_us;
        else CHECK(frame.timestamp_us>last && frame.sequence==previous+1U);
        last=frame.timestamp_us; previous=frame.sequence;
        auris_camera_frame_t second;
        CHECK(auris_camera_acquire(camera,&second,10)==-EBUSY && second.data==NULL);
        CHECK(auris_camera_release(camera)==0);
        CHECK(auris_camera_release(camera)==-EINVAL);
      }
      printf("%ux%u: 120 consecutive frames, %.2f fps\n",config.width,config.height,
             119000000.0/(double)(last-first));
      CHECK(auris_camera_acquire(camera,&frame,2000)==0);
      auris_camera_close(camera); camera=NULL; /* Close while holding a frame. */
    }
    CHECK(auris_camera_open(&camera,&config)==0);
  }
  puts("Camera checks passed"); result=EXIT_SUCCESS;
done:
  auris_camera_close(camera);
  return result;
}
#else
#include <stdio.h>
int main(void) { puts("Module 10B camera library not implemented yet"); return 77; }
#endif
