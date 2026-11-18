#include "imager_clip.h"
#include "auris_wav.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const auris_capture_profile_t profile={.device="test",.sample_rate=48000U,
    .raw_channels=8U,.format=AURIS_SAMPLE_S32_LE};
static int32_t block[128U*8U];
static uint8_t pixels[640U*360U*3U];

static bool exists(const char *folder,const char *name) {
  char path[400];
  (void)snprintf(path,sizeof(path),"%s/%s",folder,name);
  struct stat seen;
  return stat(path,&seen)==0;
}
static size_t count_lines(const char *folder,const char *name) {
  char path[400];
  (void)snprintf(path,sizeof(path),"%s/%s",folder,name);
  FILE *f=fopen(path,"r");
  assert(f);
  size_t lines=0U;
  int c;
  while ((c=fgetc(f))!=EOF) if (c=='\n') ++lines;
  (void)fclose(f);
  return lines;
}
static bool contains(const char *folder,const char *name,const char *wanted) {
  char path[400];
  static char text[8192];
  (void)snprintf(path,sizeof(path),"%s/%s",folder,name);
  FILE *f=fopen(path,"r");
  assert(f);
  const size_t bytes=fread(text,1U,sizeof(text)-1U,f);
  (void)fclose(f);
  text[bytes]='\0';
  return strstr(text,wanted)!=NULL;
}
static void wait_saved(imager_clip_t *clip,imager_clip_summary_t *summary) {
  int status;
  while ((status=imager_clip_poll(clip,summary))==1) (void)usleep(1000U);
  assert(status==0);
}
int main(void) {
  char root[]="/tmp/auris-clip-XXXXXX";
  assert(mkdtemp(root));
  char folder[128];
  (void)snprintf(folder,sizeof(folder),"%s/flat/test1/a.part",root);

  imager_clip_t *clip=NULL;
  assert(imager_clip_open(&clip,&profile,4U,3U)==0);
  assert(!imager_clip_recording(clip));
  /* Not recording: all of this is ignored. */
  imager_clip_audio(clip,block,128U,0U);
  assert(imager_clip_next_picture(clip)==-1);
  assert(imager_clip_end(clip)==-EINVAL);

  assert(imager_clip_begin(clip,folder,"  \"id\": \"a\"",1.0,true)==0);
  assert(imager_clip_recording(clip));
  assert(imager_clip_begin(clip,folder,"",1.0,true)==-EBUSY);

  /* 0.5 s of audio. Block 3 is missing, which must show as lost samples. */
  uint64_t position=1000U;
  for (int32_t i=0;i<188;++i) {
    for (size_t n=0U;n<128U*8U;++n) block[n]=i;
    if (i==3) { position+=128U; continue; }
    imager_clip_audio(clip,block,128U,position);
    position+=128U;
  }
  /* Log: quiet at -70 dBFS, then source at -40 dBFS, half of it shown. The
     first second after the source starts does not count. */
  const uint64_t source=10000000U;
  imager_clip_source(clip,source);
  for (uint64_t i=0U;i<100U;++i) {
    imager_clip_row(clip,&(imager_clip_row_t){.published_us=source-1000000U+i*1000U,
        .level_dbfs=-70.0f,.valid=true});
    imager_clip_row(clip,&(imager_clip_row_t){.published_us=source+100000U+i*1000U,
        .level_dbfs=0.0f,.valid=false});
    imager_clip_row(clip,&(imager_clip_row_t){.published_us=source+2000000U+i*1000U,
        .level_dbfs=-40.0f,.valid=i%2U==0U});
  }
  /* Pictures: the crop must come from the right place of the canvas. */
  auris_rgb24_t canvas={pixels,640U,360U,1920U,sizeof(pixels)};
  memset(pixels,50,sizeof(pixels));
  for (int32_t i=0;i<3;++i) {
    const int32_t number=imager_clip_next_picture(clip);
    assert(number==i);
    while (!imager_clip_picture(clip,&canvas,80U,0U,number,false)) (void)usleep(1000U);
    while (!imager_clip_picture(clip,&canvas,80U,0U,number,true)) (void)usleep(1000U);
    imager_clip_frame(clip,&(imager_clip_frame_t){.sequence=(uint32_t)i,.composed_us=1000000U+(uint64_t)i*50000U,
        .replaced=(uint64_t)i,.visible=true,.picture=number});
  }
  assert(!imager_clip_picture(clip,&canvas,639U,0U,0,false)); /* off the canvas */
  assert(imager_clip_end(clip)==0);
  assert(!imager_clip_recording(clip));
  imager_clip_audio(clip,block,128U,position); /* late: ignored */

  imager_clip_summary_t s;
  wait_saved(clip,&s);
  assert(s.audio_frames==187U*128U && s.lost_frames==128U);
  assert(s.rows==300U && s.frames==3U && s.pictures==3U && s.pictures_lost==0U);
  assert(s.shown_pct==50.0f);
  assert(s.quiet_dbfs>-70.5f && s.quiet_dbfs<-69.5f);
  assert(s.source_dbfs>-40.5f && s.source_dbfs<-39.5f);
  assert(s.video_fps>19.5f && s.video_fps<20.5f);
  assert(s.longest_gap_ms>49.0f && s.longest_gap_ms<51.0f);
  assert(s.replaced==2U);

  assert(exists(folder,"meta.json") && exists(folder,"audio.wav"));
  assert(exists(folder,"camera/000000.jpg") && exists(folder,"overlay/000002.jpg"));
  assert(!exists(folder,"overlay/000003.jpg"));
  assert(count_lines(folder,"log.csv")==301U && count_lines(folder,"frames.csv")==4U);
  assert(contains(folder,"meta.json","\"id\": \"a\""));
  assert(contains(folder,"meta.json","\"lost_frames\": 128"));
  assert(contains(folder,"meta.json","\"first_frame\": 1000"));

  /* The WAV reads back through the imager's own reader, samples in order. */
  char wav[160];
  (void)snprintf(wav,sizeof(wav),"%s/audio.wav",folder);
  auris_wav_reader_t reader={0};
  size_t map[8]={0,1,2,3,4,5,6,7};
  assert(auris_wav_open(&reader,wav,&profile,map,8U)==0);
  assert(reader.info.frames==187U*128U);
  auris_wav_close(&reader);

  /* Setting aside never overwrites. */
  assert(imager_clip_set_aside(folder,"redo")==0);
  assert(!exists(folder,"meta.json"));
  char aside[160];
  (void)snprintf(aside,sizeof(aside),"%s.redo1",folder);
  assert(exists(aside,"meta.json"));
  assert(imager_clip_set_aside(folder,"redo")==0); /* nothing there: fine */

  /* No pictures: log and audio only. Cancel leaves nothing behind. */
  assert(imager_clip_begin(clip,folder,"",1.0,false)==0);
  assert(imager_clip_next_picture(clip)==-1);
  imager_clip_audio(clip,block,128U,0U);
  imager_clip_cancel(clip);
  assert(!imager_clip_recording(clip));
  wait_saved(clip,NULL);
  struct stat seen;
  assert(stat(folder,&seen)!=0);

  assert(imager_clip_begin(clip,folder,"",1.0,false)==0);
  /* More audio than there is room for counts as lost, not as a crash. */
  for (uint64_t i=0U;i<400U;++i) imager_clip_audio(clip,block,128U,i*128U);
  assert(imager_clip_end(clip)==0);
  wait_saved(clip,&s);
  assert(s.audio_frames==48000U && s.lost_frames==400U*128U-48000U);
  assert(!exists(folder,"camera/000000.jpg"));

  /* A save that cannot happen is reported. */
  char blocked[160];
  (void)snprintf(blocked,sizeof(blocked),"%s/audio.wav/x",folder);
  assert(imager_clip_begin(clip,blocked,"",1.0,false)==0);
  assert(imager_clip_end(clip)==0);
  int status;
  while ((status=imager_clip_poll(clip,NULL))==1) (void)usleep(1000U);
  assert(status<0);
  assert(imager_clip_begin(clip,folder,"",1.0,false)==0); /* it can record again */
  imager_clip_cancel(clip);

  imager_clip_close(clip);
  char command[80];
  (void)snprintf(command,sizeof(command),"rm -rf %s",root);
  assert(system(command)==0);
  puts("imager clip OK");
  return 0;
}
