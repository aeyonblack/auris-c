#include "auris_display.h"
#include "auris_outputs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
  uint8_t pixels[]={1,0,0, 2,0,0, 3,0,0, 4,0,0, 5,0,0, 6,0,0};
  auris_rgb24_t rgb={pixels,3U,2U,9U,sizeof(pixels)};
  uint32_t dst[8];
  const unsigned int rotations[]={0,90,180,270};
  const uint32_t expected[4][6]={{1,2,3,4,5,6},{4,1,5,2,6,3},
                                {6,5,4,3,2,1},{3,6,2,5,1,4}};
  for (size_t r=0;r<4U;++r) {
    uint32_t w=(r&1U)?2U:3U,h=(r&1U)?3U:2U;
    memset(dst,0,sizeof(dst));
    CHECK(!auris_display_pack(&rgb,dst,w,h,(size_t)w*4U,sizeof(dst),rotations[r]));
    for (size_t i=0;i<6U;++i) CHECK(dst[i]==expected[r][i]<<16U);
  }
  uint32_t padded[25];
  for (size_t i=0;i<25U;++i) padded[i]=0xDEADBEEFU;
  CHECK(!auris_display_pack(&rgb,padded,3U,4U,20U,sizeof(padded),0U));
  CHECK(padded[0]==0U && padded[15]==0U);
  CHECK(padded[5]==1U<<16U && padded[10]==4U<<16U);
  CHECK(padded[3]==0xDEADBEEFU && padded[4]==0xDEADBEEFU);
  CHECK(auris_display_pack(&rgb,dst,3U,2U,12U,23U,0U)==-EINVAL);
  CHECK(auris_display_pack(&rgb,dst,3U,2U,12U,sizeof(dst),45U)==-EINVAL);
  rgb.stride=SIZE_MAX;
  CHECK(auris_display_pack(&rgb,dst,3U,2U,12U,sizeof(dst),0U)==-EINVAL);
  /* Exact fit (no scaling, the display plane path): a 37x23 picture packed
     into its own rotated size, with a padded pitch, for every rotation. */
  {
    enum { W=37, H=23, PAD=5 };
    static uint8_t big[W*H*3];
    static uint32_t out[(W+PAD)*(W+PAD)];
    for (size_t i=0;i<sizeof(big);++i) big[i]=(uint8_t)(i*7U+3U);
    auris_rgb24_t pic={big,W,H,W*3U,sizeof(big)};
    for (size_t r=0;r<4U;++r) {
      const bool turn=(r&1U)!=0U;
      const uint32_t w=turn?H:W, h=turn?W:H, pitch=(w+PAD)*4U;
      for (size_t i=0;i<sizeof(out)/4U;++i) out[i]=0xDEADBEEFU;
      CHECK(!auris_display_pack(&pic,out,w,h,pitch,sizeof(out),rotations[r]));
      for (uint32_t y=0;y<h;++y) {
        for (uint32_t x=0;x<w;++x) {
          uint32_t sx=x, sy=y;
          if (rotations[r]==90U) { sx=y; sy=H-1U-x; }
          else if (rotations[r]==180U) { sx=W-1U-x; sy=H-1U-y; }
          else if (rotations[r]==270U) { sx=W-1U-y; sy=x; }
          const uint8_t *p=big+(size_t)sy*W*3U+(size_t)sx*3U;
          CHECK(out[(size_t)y*(pitch/4U)+x]==(((uint32_t)p[0]<<16U)|((uint32_t)p[1]<<8U)|p[2]));
        }
        /* The pitch padding beyond each row is left alone. */
        CHECK(out[(size_t)y*(pitch/4U)+w]==0xDEADBEEFU);
      }
    }
  }
  /* Where a picture of this size goes on a larger panel, keeping its shape:
     a 640x360 canvas turned a quarter fills a 720x1280 panel exactly. */
  {
    uint32_t x=1U, y=1U, w=0U, h=0U;
    CHECK(!auris_display_fit(360U,640U,720U,1280U,&x,&y,&w,&h));
    CHECK(x==0U && y==0U && w==720U && h==1280U);
    CHECK(!auris_display_fit(640U,360U,720U,1280U,&x,&y,&w,&h));
    CHECK(x==0U && w==720U && h==405U && y==(1280U-405U)/2U);
    CHECK(!auris_display_fit(100U,100U,300U,200U,&x,&y,&w,&h));
    CHECK(w==200U && h==200U && x==50U && y==0U);
    CHECK(auris_display_fit(0U,100U,300U,200U,&x,&y,&w,&h)==-EINVAL);
    CHECK(auris_display_fit(10U,10U,300U,200U,NULL,&y,&w,&h)==-EINVAL);
  }
  auris_outputs_config_t c={0};
  char *args[]={"test","--output","display","--output","stream","--rotation","90"};
  for (int i=1;i<7;++i) CHECK(auris_outputs_option(&c,7,args,&i)==1);
  CHECK(c.stream && c.display && c.rotation==90U);
  puts("display geometry and options passed");
  return 0;
}
