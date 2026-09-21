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
  auris_outputs_config_t c={0};
  char *args[]={"test","--output","display","--output","stream","--rotation","90"};
  for (int i=1;i<7;++i) CHECK(auris_outputs_option(&c,7,args,&i)==1);
  CHECK(c.stream && c.display && c.rotation==90U);
  puts("display geometry and options passed");
  return 0;
}
