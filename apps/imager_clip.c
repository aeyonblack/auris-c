#include "imager_clip.h"
#include "auris_wav.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <jpeglib.h>

#define QUEUE 6U
#define PICTURES_PER_SECOND 5U
#define FRAMES_PER_SECOND 35U
#define JPEG_QUALITY 90
/* The first second of the source is someone pressing play. */
#define SOURCE_SETTLE_US 1000000U

typedef struct { unsigned char *data; unsigned long bytes; int32_t number; bool overlay; } jpeg_t;
typedef struct { unsigned char *rgb; int32_t number; bool overlay, used; } slot_t;
typedef struct { struct jpeg_error_mgr base; jmp_buf jump; } jpeg_error_t;

struct imager_clip {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t wake;
  bool stop, recording, saving, has_pictures;
  int save_result;
  auris_capture_profile_t profile;
  size_t frame_bytes;
  unsigned int picture_w, picture_h;
  char folder[256];
  char *meta;
  uint64_t source_us;
  unsigned char *audio;
  uint64_t audio_capacity, audio_frames, first_frame, next_frame, lost_frames;
  imager_clip_row_t *rows;
  size_t row_capacity, row_count;
  imager_clip_frame_t *frames;
  size_t frame_capacity, frame_count;
  slot_t queue[QUEUE];
  size_t queue_used;
  jpeg_t *jpegs;
  size_t jpeg_capacity, jpeg_count;
  int32_t next_picture;
  uint32_t pictures_lost;
  imager_clip_summary_t summary;
};

static void jpeg_fail(j_common_ptr common) {
  jpeg_error_t *error=(jpeg_error_t *)common->err;
  longjmp(error->jump,1);
}
static int encode(const unsigned char *rgb,unsigned int w,unsigned int h,
                  unsigned char **data,unsigned long *bytes) {
  struct jpeg_compress_struct jpeg={0};
  jpeg_error_t error;
  volatile bool created=false;
  jpeg.err=jpeg_std_error(&error.base);
  error.base.error_exit=jpeg_fail;
  if (setjmp(error.jump)) {
    if (created) jpeg_destroy_compress(&jpeg);
    free(*data); *data=NULL;
    return -EIO;
  }
  jpeg_create_compress(&jpeg); created=true;
  jpeg_mem_dest(&jpeg,data,bytes);
  jpeg.image_width=w; jpeg.image_height=h;
  jpeg.input_components=3; jpeg.in_color_space=JCS_RGB;
  jpeg_set_defaults(&jpeg);
  jpeg_set_quality(&jpeg,JPEG_QUALITY,TRUE);
  jpeg_start_compress(&jpeg,TRUE);
  while (jpeg.next_scanline<jpeg.image_height) {
    JSAMPROW row=(JSAMPROW)(rgb+(size_t)jpeg.next_scanline*(size_t)w*3U);
    (void)jpeg_write_scanlines(&jpeg,&row,1U);
  }
  jpeg_finish_compress(&jpeg); jpeg_destroy_compress(&jpeg);
  return 0;
}
static int make_folders(const char *path) {
  char partial[256];
  const size_t length=strlen(path);
  if (length>=sizeof(partial)) return -ENAMETOOLONG;
  for (size_t i=1U;i<=length;++i) {
    if (path[i]!='/' && path[i]!='\0') continue;
    memcpy(partial,path,i);
    partial[i]='\0';
    if (mkdir(partial,0755)!=0 && errno!=EEXIST) return -errno;
  }
  return 0;
}
static int join(char *out,size_t capacity,const char *folder,const char *name) {
  const int n=snprintf(out,capacity,"%s/%s",folder,name);
  return n<0 || (size_t)n>=capacity ? -ENAMETOOLONG : 0;
}
static float to_dbfs(double power,size_t count) {
  return count ? (float)(10.0*log10(power/(double)count+1e-12)) : -120.0f;
}
/* Rows are in time order; only the audio thread appends, and it has stopped. */
static void summarise(struct imager_clip *c) {
  imager_clip_summary_t *s=&c->summary;
  double quiet=0.0,source=0.0;
  size_t quiet_count=0U,source_count=0U,shown=0U;
  for (size_t i=0U;i<c->row_count;++i) {
    const imager_clip_row_t *r=&c->rows[i];
    const double power=pow(10.0,(double)r->level_dbfs/10.0);
    if (r->published_us<c->source_us) { quiet+=power; ++quiet_count; continue; }
    if (r->published_us<c->source_us+SOURCE_SETTLE_US) continue;
    source+=power; ++source_count;
    if (r->valid) ++shown;
  }
  s->quiet_dbfs=to_dbfs(quiet,quiet_count);
  s->source_dbfs=to_dbfs(source,source_count);
  s->shown_pct=source_count ? 100.0f*(float)shown/(float)source_count : 0.0f;
  s->audio_frames=c->audio_frames;
  s->lost_frames=c->lost_frames;
  s->pictures_lost=c->pictures_lost;
  s->pictures=(uint32_t)(c->jpeg_count/2U);
  s->rows=(uint32_t)c->row_count;
  s->frames=(uint32_t)c->frame_count;
  s->replaced=0U;
  s->video_fps=0.0f;
  s->longest_gap_ms=0.0f;
  if (c->frame_count>=2U) {
    const imager_clip_frame_t *first=&c->frames[0],*last=&c->frames[c->frame_count-1U];
    s->replaced=last->replaced-first->replaced;
    if (last->composed_us>first->composed_us)
      s->video_fps=(float)((double)(c->frame_count-1U)*1e6/(double)(last->composed_us-first->composed_us));
    for (size_t i=1U;i<c->frame_count;++i) {
      const float gap=(float)(c->frames[i].composed_us-c->frames[i-1U].composed_us)/1000.0f;
      if (gap>s->longest_gap_ms) s->longest_gap_ms=gap;
    }
  }
}
static int write_meta(const struct imager_clip *c,const char *folder) {
  char path[300];
  int err=join(path,sizeof(path),folder,"meta.json");
  if (err<0) return err;
  FILE *f=fopen(path,"w");
  if (!f) return -errno;
  const imager_clip_summary_t *s=&c->summary;
  (void)fprintf(f,"{\n%s%s  \"source_start_us\": %llu,\n  \"first_frame\": %llu,\n"
      "  \"audio_frames\": %llu,\n  \"lost_frames\": %llu,\n  \"shown_pct\": %.1f,\n"
      "  \"source_dbfs\": %.1f,\n  \"quiet_dbfs\": %.1f,\n  \"video_fps\": %.2f,\n"
      "  \"longest_frame_gap_ms\": %.1f,\n  \"frames_not_shown\": %llu,\n"
      "  \"pictures\": %u,\n  \"pictures_lost\": %u\n}\n",
      c->meta,c->meta[0] ? ",\n":"",
      (unsigned long long)c->source_us,(unsigned long long)c->first_frame,
      (unsigned long long)s->audio_frames,(unsigned long long)s->lost_frames,
      (double)s->shown_pct,(double)s->source_dbfs,(double)s->quiet_dbfs,(double)s->video_fps,
      (double)s->longest_gap_ms,(unsigned long long)s->replaced,s->pictures,s->pictures_lost);
  err=ferror(f) ? -EIO : 0;
  return fclose(f)!=0 && err==0 ? -errno : err;
}
static int write_log(const struct imager_clip *c,const char *folder) {
  char path[300];
  int err=join(path,sizeof(path),folder,"log.csv");
  if (err<0) return err;
  FILE *f=fopen(path,"w");
  if (!f) return -errno;
  (void)fputs("centre_us,published_us,first_frame,level_dbfs,noise_dbfs,weight_sum,ready,active,"
              "evidence,valid,fresh,column,row,x,y,z,strength,confidence,contrast,dsp_us\n",f);
  for (size_t i=0U;i<c->row_count;++i) {
    const imager_clip_row_t *r=&c->rows[i];
    (void)fprintf(f,"%llu,%llu,%llu,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%.3f,%.3f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%u\n",
        (unsigned long long)r->centre_us,(unsigned long long)r->published_us,
        (unsigned long long)r->first_frame,(double)r->level_dbfs,(double)r->noise_dbfs,
        (double)r->weight_sum,r->ready,r->active,r->evidence,r->valid,r->fresh,
        (double)r->column,(double)r->row,(double)r->x,(double)r->y,(double)r->z,
        (double)r->strength,(double)r->confidence,(double)r->contrast,r->dsp_us);
  }
  err=ferror(f) ? -EIO : 0;
  return fclose(f)!=0 && err==0 ? -errno : err;
}
static int write_frames(const struct imager_clip *c,const char *folder) {
  char path[300];
  int err=join(path,sizeof(path),folder,"frames.csv");
  if (err<0) return err;
  FILE *f=fopen(path,"w");
  if (!f) return -errno;
  (void)fputs("sequence,camera_us,audio_us,composed_us,replaced,visible,strength,picture\n",f);
  for (size_t i=0U;i<c->frame_count;++i) {
    const imager_clip_frame_t *r=&c->frames[i];
    (void)fprintf(f,"%u,%llu,%llu,%llu,%llu,%d,%.4f,%d\n",r->sequence,
        (unsigned long long)r->camera_us,(unsigned long long)r->audio_us,
        (unsigned long long)r->composed_us,(unsigned long long)r->replaced,
        r->visible,(double)r->strength,r->picture);
  }
  err=ferror(f) ? -EIO : 0;
  return fclose(f)!=0 && err==0 ? -errno : err;
}
static int write_audio(const struct imager_clip *c,const char *folder) {
  char path[300];
  int err=join(path,sizeof(path),folder,"audio.wav");
  if (err<0) return err;
  auris_wav_writer_t writer={0};
  err=auris_wav_writer_open(&writer,path,&c->profile);
  if (err<0) return err;
  err=auris_wav_writer_write(&writer,c->audio,(size_t)c->audio_frames);
  const int closed=auris_wav_writer_close(&writer);
  return err<0 ? err : closed;
}
static int write_pictures(const struct imager_clip *c,const char *folder) {
  if (!c->has_pictures) return 0;
  char path[320];
  int err=join(path,sizeof(path),folder,"camera");
  if (err==0) err=make_folders(path);
  if (err==0) err=join(path,sizeof(path),folder,"overlay");
  if (err==0) err=make_folders(path);
  if (err<0) return err;
  for (size_t i=0U;i<c->jpeg_count;++i) {
    const jpeg_t *j=&c->jpegs[i];
    const int n=snprintf(path,sizeof(path),"%s/%s/%06d.jpg",folder,j->overlay ? "overlay":"camera",j->number);
    if (n<0 || (size_t)n>=sizeof(path)) return -ENAMETOOLONG;
    FILE *f=fopen(path,"wb");
    if (!f) return -errno;
    const size_t written=fwrite(j->data,1U,j->bytes,f);
    const int closed=fclose(f);
    if (written!=j->bytes || closed!=0) return -EIO;
  }
  return 0;
}
static int save_clip(struct imager_clip *c) {
  summarise(c);
  int err=make_folders(c->folder);
  if (err==0) err=write_audio(c,c->folder);
  if (err==0) err=write_log(c,c->folder);
  if (err==0) err=write_frames(c,c->folder);
  if (err==0) err=write_pictures(c,c->folder);
  /* Last, so a folder with meta.json is a finished clip. */
  if (err==0) err=write_meta(c,c->folder);
  sync();
  return err;
}
static void release_recording(struct imager_clip *c) {
  for (size_t i=0U;i<c->jpeg_count;++i) free(c->jpegs[i].data);
  for (size_t i=0U;i<QUEUE;++i) c->queue[i].used=false;
  c->queue_used=0U;
  free(c->jpegs); c->jpegs=NULL;
  free(c->audio); c->audio=NULL;
  free(c->rows); c->rows=NULL;
  free(c->frames); c->frames=NULL;
  free(c->meta); c->meta=NULL;
  c->jpeg_count=0U;
}
static void *worker(void *context) {
  struct imager_clip *c=context;
  (void)pthread_mutex_lock(&c->mutex);
  while (!c->stop) {
    size_t found=QUEUE;
    for (size_t i=0U;i<QUEUE && found==QUEUE;++i) if (c->queue[i].used) found=i;
    if (found<QUEUE) {
      slot_t *slot=&c->queue[found];
      unsigned char *data=NULL;
      unsigned long bytes=0U;
      (void)pthread_mutex_unlock(&c->mutex);
      const int err=encode(slot->rgb,c->picture_w,c->picture_h,&data,&bytes);
      (void)pthread_mutex_lock(&c->mutex);
      if (err==0 && c->jpeg_count<c->jpeg_capacity)
        c->jpegs[c->jpeg_count++]=(jpeg_t){data,bytes,slot->number,slot->overlay};
      else { free(data); ++c->pictures_lost; }
      slot->used=false;
      --c->queue_used;
      continue;
    }
    if (c->saving) {
      (void)pthread_mutex_unlock(&c->mutex);
      /* Nothing else touches the recording while saving is set. */
      const int err=save_clip(c);
      (void)pthread_mutex_lock(&c->mutex);
      c->save_result=err;
      c->saving=false;
      release_recording(c);
      continue;
    }
    (void)pthread_cond_wait(&c->wake,&c->mutex);
  }
  (void)pthread_mutex_unlock(&c->mutex);
  return NULL;
}
int imager_clip_open(imager_clip_t **out,const auris_capture_profile_t *profile,
                     unsigned int picture_w,unsigned int picture_h) {
  if (!out || !profile || !picture_w || !picture_h) return -EINVAL;
  struct imager_clip *c=calloc(1U,sizeof(*c));
  if (!c) return -ENOMEM;
  c->profile=*profile;
  c->frame_bytes=profile->raw_channels*(profile->format==AURIS_SAMPLE_S32_LE ? 4U:2U);
  c->picture_w=picture_w; c->picture_h=picture_h;
  (void)pthread_mutex_init(&c->mutex,NULL);
  (void)pthread_cond_init(&c->wake,NULL);
  for (size_t i=0U;i<QUEUE;++i) {
    c->queue[i].rgb=malloc((size_t)picture_w*picture_h*3U);
    if (!c->queue[i].rgb) { imager_clip_close(c); return -ENOMEM; }
  }
  const int err=pthread_create(&c->thread,NULL,worker,c);
  if (err) {
    for (size_t i=0U;i<QUEUE;++i) free(c->queue[i].rgb);
    (void)pthread_mutex_destroy(&c->mutex);
    (void)pthread_cond_destroy(&c->wake);
    free(c);
    return -err;
  }
  *out=c;
  return 0;
}
int imager_clip_begin(imager_clip_t *c,const char *folder,const char *meta,double seconds,bool pictures) {
  if (!c || !folder || !meta || !(seconds>0.0) || strlen(folder)>=sizeof(c->folder)) return -EINVAL;
  (void)pthread_mutex_lock(&c->mutex);
  const bool busy=c->recording || c->saving;
  (void)pthread_mutex_unlock(&c->mutex);
  if (busy) return -EBUSY;
  const uint64_t audio_capacity=(uint64_t)(seconds*(double)c->profile.sample_rate);
  const size_t row_capacity=(size_t)(seconds*(double)c->profile.sample_rate/128.0)+64U;
  const size_t frame_capacity=(size_t)(seconds*FRAMES_PER_SECOND)+64U;
  const size_t jpeg_capacity=pictures ? ((size_t)(seconds*PICTURES_PER_SECOND)+8U)*2U : 0U;
  unsigned char *audio=malloc((size_t)audio_capacity*c->frame_bytes);
  imager_clip_row_t *rows=calloc(row_capacity,sizeof(*rows));
  imager_clip_frame_t *frames=calloc(frame_capacity,sizeof(*frames));
  jpeg_t *jpegs=calloc(jpeg_capacity ? jpeg_capacity:1U,sizeof(*jpegs));
  char *copy=strdup(meta);
  if (!audio || !rows || !frames || !jpegs || !copy) {
    free(audio); free(rows); free(frames); free(jpegs); free(copy);
    return -ENOMEM;
  }
  /* Touch every page now, so the audio thread never waits for one. */
  memset(audio,0,(size_t)audio_capacity*c->frame_bytes);
  (void)pthread_mutex_lock(&c->mutex);
  (void)snprintf(c->folder,sizeof(c->folder),"%s",folder);
  c->meta=copy;
  c->audio=audio; c->audio_capacity=audio_capacity; c->audio_frames=0U;
  c->first_frame=0U; c->next_frame=0U; c->lost_frames=0U;
  c->rows=rows; c->row_capacity=row_capacity; c->row_count=0U;
  c->frames=frames; c->frame_capacity=frame_capacity; c->frame_count=0U;
  c->jpegs=jpegs; c->jpeg_capacity=jpeg_capacity; c->jpeg_count=0U;
  c->next_picture=0; c->pictures_lost=0U;
  c->source_us=UINT64_MAX;
  c->has_pictures=pictures;
  c->save_result=0;
  c->recording=true;
  (void)pthread_mutex_unlock(&c->mutex);
  return 0;
}
void imager_clip_source(imager_clip_t *c,uint64_t now_us) {
  (void)pthread_mutex_lock(&c->mutex);
  c->source_us=now_us;
  (void)pthread_mutex_unlock(&c->mutex);
}
bool imager_clip_recording(const imager_clip_t *c) {
  pthread_mutex_t *mutex=&((struct imager_clip *)c)->mutex;
  (void)pthread_mutex_lock(mutex);
  const bool recording=c->recording;
  (void)pthread_mutex_unlock(mutex);
  return recording;
}
void imager_clip_audio(imager_clip_t *c,const void *raw,size_t frames,uint64_t first_frame) {
  (void)pthread_mutex_lock(&c->mutex);
  if (c->recording) {
    if (c->audio_frames==0U) c->first_frame=first_frame;
    else if (first_frame!=c->next_frame)
      c->lost_frames+=first_frame>c->next_frame ? first_frame-c->next_frame : frames;
    c->next_frame=first_frame+frames;
    const uint64_t room=c->audio_capacity-c->audio_frames;
    const size_t count=frames<room ? frames:(size_t)room;
    memcpy(c->audio+(size_t)c->audio_frames*c->frame_bytes,raw,count*c->frame_bytes);
    c->audio_frames+=count;
    c->lost_frames+=frames-count;
  }
  (void)pthread_mutex_unlock(&c->mutex);
}
void imager_clip_row(imager_clip_t *c,const imager_clip_row_t *row) {
  (void)pthread_mutex_lock(&c->mutex);
  if (c->recording && c->row_count<c->row_capacity) c->rows[c->row_count++]=*row;
  (void)pthread_mutex_unlock(&c->mutex);
}
int32_t imager_clip_next_picture(imager_clip_t *c) {
  (void)pthread_mutex_lock(&c->mutex);
  const int32_t number=c->recording && c->has_pictures ? c->next_picture++ : -1;
  (void)pthread_mutex_unlock(&c->mutex);
  return number;
}
bool imager_clip_picture(imager_clip_t *c,const auris_rgb24_t *canvas,unsigned int x,unsigned int y,
                         int32_t number,bool overlay) {
  if (number<0 || x+c->picture_w>canvas->width || y+c->picture_h>canvas->height) return false;
  bool queued=false;
  (void)pthread_mutex_lock(&c->mutex);
  if (c->recording && c->has_pictures) {
    size_t found=QUEUE;
    for (size_t i=0U;i<QUEUE && found==QUEUE;++i) if (!c->queue[i].used) found=i;
    if (found<QUEUE) {
      slot_t *slot=&c->queue[found];
      /* The slot is free, so the worker is not reading it. */
      for (unsigned int row=0U;row<c->picture_h;++row)
        memcpy(slot->rgb+(size_t)row*c->picture_w*3U,
               canvas->data+(size_t)(y+row)*canvas->stride+(size_t)x*3U,(size_t)c->picture_w*3U);
      slot->number=number; slot->overlay=overlay; slot->used=true;
      ++c->queue_used;
      queued=true;
      (void)pthread_cond_signal(&c->wake);
    } else ++c->pictures_lost;
  }
  (void)pthread_mutex_unlock(&c->mutex);
  return queued;
}
void imager_clip_frame(imager_clip_t *c,const imager_clip_frame_t *frame) {
  (void)pthread_mutex_lock(&c->mutex);
  if (c->recording && c->frame_count<c->frame_capacity) c->frames[c->frame_count++]=*frame;
  (void)pthread_mutex_unlock(&c->mutex);
}
int imager_clip_end(imager_clip_t *c) {
  (void)pthread_mutex_lock(&c->mutex);
  const int result=c->recording ? 0 : -EINVAL;
  if (c->recording) {
    c->recording=false;
    c->saving=true;
    (void)pthread_cond_signal(&c->wake);
  }
  (void)pthread_mutex_unlock(&c->mutex);
  return result;
}
void imager_clip_cancel(imager_clip_t *c) {
  (void)pthread_mutex_lock(&c->mutex);
  if (c->recording) {
    c->recording=false;
    /* The worker may be compressing a slot; wait for it to hand it back. */
    while (c->queue_used>0U) {
      (void)pthread_cond_signal(&c->wake);
      (void)pthread_mutex_unlock(&c->mutex);
      (void)usleep(1000U);
      (void)pthread_mutex_lock(&c->mutex);
    }
    release_recording(c);
  }
  (void)pthread_mutex_unlock(&c->mutex);
}
int imager_clip_poll(imager_clip_t *c,imager_clip_summary_t *summary) {
  (void)pthread_mutex_lock(&c->mutex);
  const int result=c->saving ? 1 : c->save_result;
  if (!c->saving && summary) *summary=c->summary;
  (void)pthread_mutex_unlock(&c->mutex);
  return result;
}
void imager_clip_close(imager_clip_t *c) {
  if (!c) return;
  (void)pthread_mutex_lock(&c->mutex);
  const bool started=c->queue[QUEUE-1U].rgb!=NULL;
  c->stop=true;
  (void)pthread_cond_signal(&c->wake);
  (void)pthread_mutex_unlock(&c->mutex);
  if (started) (void)pthread_join(c->thread,NULL);
  release_recording(c);
  for (size_t i=0U;i<QUEUE;++i) free(c->queue[i].rgb);
  (void)pthread_mutex_destroy(&c->mutex);
  (void)pthread_cond_destroy(&c->wake);
  free(c);
}
int imager_clip_set_aside(const char *folder,const char *tag) {
  struct stat seen;
  if (stat(folder,&seen)!=0) return errno==ENOENT ? 0 : -errno;
  char name[300];
  for (unsigned int n=1U;n<1000U;++n) {
    const int written=snprintf(name,sizeof(name),"%s.%s%u",folder,tag,n);
    if (written<0 || (size_t)written>=sizeof(name)) return -ENAMETOOLONG;
    if (stat(name,&seen)==0) continue;
    if (errno!=ENOENT) return -errno;
    return rename(folder,name)==0 ? 0 : -errno;
  }
  return -EEXIST;
}
