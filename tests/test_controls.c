#include "auris_ui.h"
#include "auris_display.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void tap(auris_ui_t *ui,int x,int y,uint64_t now) {
  auris_touch_event_t e={AURIS_TOUCH_DOWN,x,y}; auris_ui_touch(ui,&e,now);
  e.kind=AURIS_TOUCH_UP; auris_ui_touch(ui,&e,now+1000U);
}
static bool event(auris_touch_decoder_t *s,unsigned short type,unsigned short code,
                   int value,auris_touch_event_t *out) {
  struct input_event in={.type=type,.code=code,.value=value};
  return auris_touch_decode(s,&in,out);
}
static void touch_tests(void) {
  auris_touch_decoder_t d; auris_touch_decoder_init(&d,10);
  auris_touch_event_t out;
  assert(!event(&d,EV_ABS,ABS_MT_TRACKING_ID,10,&out));
  (void)event(&d,EV_ABS,ABS_MT_POSITION_X,123,&out);
  (void)event(&d,EV_ABS,ABS_MT_POSITION_Y,456,&out);
  assert(event(&d,EV_SYN,SYN_REPORT,0,&out) && out.kind==AURIS_TOUCH_DOWN && out.x==123);
  (void)event(&d,EV_ABS,ABS_MT_SLOT,1,&out);
  (void)event(&d,EV_ABS,ABS_MT_TRACKING_ID,11,&out);
  assert(event(&d,EV_SYN,SYN_REPORT,0,&out) && out.kind==AURIS_TOUCH_CANCEL);
  (void)event(&d,EV_ABS,ABS_MT_TRACKING_ID,-1,&out);
  assert(!event(&d,EV_SYN,SYN_REPORT,0,&out));
  (void)event(&d,EV_ABS,ABS_MT_SLOT,0,&out);
  (void)event(&d,EV_ABS,ABS_MT_TRACKING_ID,-1,&out);
  assert(!event(&d,EV_SYN,SYN_REPORT,0,&out) && !d.blocked);
  (void)event(&d,EV_ABS,ABS_MT_TRACKING_ID,12,&out);
  assert(event(&d,EV_SYN,SYN_REPORT,0,&out));
  assert(event(&d,EV_SYN,SYN_DROPPED,0,&out) && out.kind==AURIS_TOUCH_CANCEL);
  assert(!event(&d,EV_SYN,SYN_REPORT,0,&out));
  auris_touch_decoder_init(&d,10);
  (void)event(&d,EV_ABS,ABS_MT_SLOT,99,&out);
  (void)event(&d,EV_ABS,ABS_MT_TRACKING_ID,20,&out);
  assert(!event(&d,EV_SYN,SYN_REPORT,0,&out));
  /* Inverse mapping agrees with the actual display pack at every pixel. */
  uint8_t src[8U*6U*3U];
  for (size_t i=0U;i<48U;++i) { src[i*3U]=(uint8_t)(i+1U); src[i*3U+1U]=0; src[i*3U+2U]=0; }
  auris_rgb24_t rgb={src,8U,6U,24U,sizeof(src)};
  uint32_t packed[17U*13U];
  for (unsigned int rot=0U;rot<360U;rot+=90U) {
    assert(!auris_display_pack(&rgb,packed,17U,13U,17U*4U,sizeof(packed),rot));
    for (int y=0;y<13;++y) for (int x=0;x<17;++x) {
      int ix,iy;
      bool inside=auris_touch_project(x,y,16,12,17U,13U,8U,6U,rot,&ix,&iy);
      if (inside) assert(packed[y*17+x]==((uint32_t)(iy*8+ix+1)<<16));
      else assert(packed[y*17+x]==0U);
    }
  }
}
int main(void) {
  touch_tests();
  auris_settings_t s=auris_settings_defaults(), original=s;
  assert(auris_settings_valid(&s));
  s.cutoff=1.0f; assert(!auris_settings_valid(&s)); s=original;
  char path[]="/tmp/auris-settings-test-XXXXXX";
  int fd=mkstemp(path); assert(fd>=0); assert(!close(fd));
  auris_settings_writer_t *w=NULL;
  assert(!auris_settings_writer_open(&w,path));
  s.gating=false; s.fps=20U;
  assert(!auris_settings_save(w,&s));
  auris_settings_writer_close(w); /* Shutdown drains pending save. */
  assert(!auris_settings_load(path,&original) && original.fps==20U && !original.gating);
  FILE *f=fopen(path,"w"); assert(f);
  assert(fputs("AURIS_SETTINGS 1\noverlay 1\ngating 1\nsmoothing 1\ncutoff nan\nopacity 1\nfps 30\n",f)>=0);
  assert(!fclose(f));
  assert(auris_settings_load(path,&original)==-EINVAL && original.fps==20U);
  assert(!unlink(path));
  assert(auris_settings_load(path,&original)==-ENOENT);
  w=NULL;
  assert(!auris_settings_writer_open(&w,"/no-such-auris-dir/settings"));
  assert(!auris_settings_save(w,&s));
  for (unsigned int i=0U;i<1000U && auris_settings_save_status(w)==1;++i) {
    struct timespec delay={0,1000000L}; (void)nanosleep(&delay,NULL);
  }
  assert(auris_settings_save_status(w)==-ENOENT);
  auris_settings_writer_close(w);
  auris_ui_t ui; auris_ui_init(&ui,&s,0U);
  tap(&ui,250,190,100U); assert(ui.settings.fps==20U);
  tap(&ui,440,190,200U); assert(ui.settings.fps==30U);
  tap(&ui,250,250,300U); assert(ui.settings.fps==30U); /* First reset only arms. */
  tap(&ui,250,250,2000U); assert(ui.settings.fps==15U && ui.settings.gating);
  tap(&ui,440,250,3000U); assert(!ui.visible);
  tap(&ui,250,80,4000U); assert(ui.visible && ui.settings.gating); /* Wake only. */
  tap(&ui,250,80,5000U); assert(!ui.settings.gating);
  auris_touch_event_t down={AURIS_TOUCH_DOWN,250,80}; auris_ui_touch(&ui,&down,6000U);
  down.kind=AURIS_TOUCH_MOVE; down.x=0; auris_ui_touch(&ui,&down,6100U);
  down.kind=AURIS_TOUCH_UP; down.x=250; auris_ui_touch(&ui,&down,6200U);
  assert(!ui.settings.gating);
  for (unsigned int i=0U;i<30U;++i) tap(&ui,200,140,7000U+i*2000U);
  assert(ui.settings.cutoff==0.95f);
  uint8_t *pixels=calloc(1U,640U*360U*3U); assert(pixels);
  auris_rgb24_t image={pixels,640U,360U,1920U,640U*360U*3U};
  auris_ui_status_t status={.dbfs=-40.0f,.fps=15.0f};
  assert(!auris_ui_draw(&ui,&image,&status,100000U));
  assert(!auris_ui_draw(&ui,&image,&status,11000000U) && !ui.visible);
  image.bytes=100U; assert(auris_ui_draw(&ui,&image,&status,0U)==-EINVAL);
  free(pixels);
  puts("settings, touch decoding, display inversion and UI: passed");
  return 0;
}
