#ifdef AURIS_IMAGE_AVAILABLE
#include "auris_image.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(EXIT_FAILURE); } } while (0)

static auris_image_config_t config(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
  return (auris_image_config_t){sw, sh, w, h, 0.0f, {1.0f,1.0f,1.0f}, 0.0f, 1.0f};
}

static void flat(void) {
  /* Odd dimensions, padded rows, all Bayer parities and mirrored borders. */
  uint8_t raw[7U*5U], rgb[23U*5U];
  memset(raw, 253, sizeof(raw)); memset(rgb, 177, sizeof(rgb));
  for (size_t y=0U; y<5U; ++y)
    for (size_t x=0U; x<5U; ++x)
      raw[y*7U+x] = (uint8_t)((y&1U) ? ((x&1U) ? 200U : 80U) : ((x&1U) ? 80U : 20U));
  auris_image_config_t c=config(5U,5U,7U,5U);
  auris_image_t *s=NULL;
  CHECK(auris_image_open(&s,&c)==0);
  const auris_image_rect_t *r=auris_image_content(s);
  CHECK(r->x==1U && r->y==0U && r->width==5U && r->height==5U);
  auris_bayer8_t in={raw,5U,5U,7U,33U}; /* No last-row padding needed. */
  auris_rgb24_t out={rgb,7U,5U,23U,113U};
  CHECK(auris_image_render(s,&in,&out)==0);
  for(size_t y=0U;y<5U;++y) {
    for(size_t x=0U;x<7U;++x) {
      const uint8_t expected[3]={20,80,200};
      for(size_t k=0U;k<3U;++k)
        CHECK(rgb[y*23U+x*3U+k]==((x==0U || x==6U) ? 0U : expected[k]));
    }
    CHECK(rgb[y*23U+21U]==177U && rgb[y*23U+22U]==177U);
  }
  CHECK(raw[5]==253U && raw[6]==253U);
  out.bytes=112U;
  CHECK(auris_image_render(s,&in,&out)==-EINVAL);
  out.bytes=113U; in.bytes=32U;
  CHECK(auris_image_render(s,&in,&out)==-EINVAL);
  in.bytes=33U; out.stride=20U;
  CHECK(auris_image_render(s,&in,&out)==-EINVAL);
  auris_image_close(s);
}

static void interpolation(void) {
  uint8_t raw[8U*6U], rgb[16U*12U*3U];
  /* Each colour is an affine plane; bilinear reconstruction must preserve it
     away from reflected boundaries. Expected values are analytic. */
  for(size_t y=0U;y<6U;++y)
    for(size_t x=0U;x<8U;++x) {
      const unsigned int base=(y&1U) ? ((x&1U) ? 60U : 40U) : ((x&1U) ? 40U : 20U);
      raw[y*8U+x]=(uint8_t)(base+2U*x+4U*y);
    }
  auris_image_config_t c=config(8U,6U,16U,12U);
  auris_image_t *s=NULL;
  CHECK(auris_image_open(&s,&c)==0);
  auris_bayer8_t in={raw,8U,6U,8U,sizeof(raw)};
  auris_rgb24_t out={rgb,16U,12U,48U,sizeof(rgb)};
  CHECK(auris_image_render(s,&in,&out)==0);
  for(size_t y=3U;y<9U;++y)
    for(size_t x=3U;x<13U;++x)
      for(size_t k=0U;k<3U;++k) {
        const float sx=((float)x+0.5f)/2.0f-0.5f;
        const float sy=((float)y+0.5f)/2.0f-0.5f;
        const float expected=20.0f*(float)(k+1U)+2.0f*sx+4.0f*sy;
        CHECK(fabsf((float)rgb[(y*16U+x)*3U+k]-expected)<=1.0f);
      }
  auris_image_close(s);
}

static void tone_and_validation(void) {
  uint8_t raw[4]={64,64,64,64}, rgb[12];
  auris_image_config_t c=config(2U,2U,2U,2U);
  c.black=16.0f; c.exposure_ev=1.0f; c.gain[0]=2.0f; c.gamma=2.0f;
  auris_image_t *s=NULL;
  CHECK(auris_image_open(&s,&c)==0);
  CHECK(auris_image_open(&s,&c)==-EINVAL);
  auris_bayer8_t in={raw,2U,2U,2U,sizeof(raw)};
  auris_rgb24_t out={rgb,2U,2U,6U,sizeof(rgb)};
  CHECK(auris_image_render(s,&in,&out)==0);
  CHECK(abs((int)rgb[0]-229)<=1 && abs((int)rgb[1]-162)<=1 && rgb[1]==rgb[2]);
  memset(raw,0,sizeof(raw)); CHECK(auris_image_render(s,&in,&out)==0);
  for(size_t k=0;k<12U;++k) CHECK(rgb[k]==0U);
  memset(raw,255,sizeof(raw)); CHECK(auris_image_render(s,&in,&out)==0);
  for(size_t k=0;k<12U;++k) CHECK(rgb[k]==255U);
  in.stride=SIZE_MAX; CHECK(auris_image_render(s,&in,&out)==-EINVAL);
  auris_image_close(s); s=NULL;
  c.gamma=NAN; CHECK(auris_image_open(&s,&c)==-EINVAL && s==NULL);
  c.gamma=1.0f; c.gain[1]=INFINITY; CHECK(auris_image_open(&s,&c)==-EINVAL);
  c.gain[1]=1.0f; c.width=0U; CHECK(auris_image_open(&s,&c)==-EINVAL);
  CHECK(auris_image_open(NULL,&c)==-EINVAL);
  CHECK(auris_image_content(NULL)==NULL);
  CHECK(auris_image_render(NULL,&in,&out)==-EINVAL);
  auris_image_close(NULL);
}

static void layouts(void) {
  const uint32_t sizes[][6]={{640,360,80,0,479,360},{1280,720,161,0,958,720},
                           {360,640,0,185,360,270},{1,1,0,0,1,1}};
  for(size_t i=0U;i<4U;++i) {
    auris_image_config_t c=config(1640,1232,sizes[i][0],sizes[i][1]);
    auris_image_t *s=NULL;
    CHECK(auris_image_open(&s,&c)==0);
    const auris_image_rect_t *r=auris_image_content(s);
    CHECK(r->x==sizes[i][2] && r->y==sizes[i][3] && r->width==sizes[i][4] && r->height==sizes[i][5]);
    auris_image_close(s);
  }
}

int main(void) {
  flat(); interpolation(); tone_and_validation(); layouts();
  puts("Image checks passed"); return EXIT_SUCCESS;
}
#else
#include <stdio.h>
int main(void) { puts("Module 11 image library not implemented yet"); return 77; }
#endif
