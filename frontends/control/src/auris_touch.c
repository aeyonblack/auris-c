#include "auris_touch.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

void auris_touch_decoder_init(auris_touch_decoder_t *s,int count) {
  *s=(auris_touch_decoder_t){.count=count>0 && count<=32 ? count : 0};
  for (int i=0;i<32;++i) s->id[i]=-1;
}
bool auris_touch_decode(auris_touch_decoder_t *s,const struct input_event *in,
                        auris_touch_event_t *out) {
  if (in->type==EV_SYN && in->code==SYN_DROPPED) {
    s->dropped=true; s->blocked=true;
    bool was=s->down; s->down=false;
    *out=(auris_touch_event_t){AURIS_TOUCH_CANCEL,s->last_x,s->last_y};
    return was;
  }
  if (s->dropped) return false; /* Driver resynchronizes at the next SYN_REPORT. */
  if (in->type==EV_ABS) {
    if (in->code==ABS_MT_SLOT) s->slot=in->value;
    else if (s->slot>=0 && s->slot<s->count) {
      if (in->code==ABS_MT_TRACKING_ID) s->id[s->slot]=in->value;
      if (in->code==ABS_MT_POSITION_X) s->x[s->slot]=in->value;
      if (in->code==ABS_MT_POSITION_Y) s->y[s->slot]=in->value;
    }
  }
  if (in->type!=EV_SYN || in->code!=SYN_REPORT) return false;
  int contacts=0, selected=0;
  for (int i=0;i<s->count;++i) if (s->id[i]>=0) { ++contacts; selected=i; }
  if (contacts>1 || (contacts==1 && s->down && s->active_id!=s->id[selected]))
    s->blocked=true;
  if (s->blocked) {
    bool was=s->down; s->down=false;
    if (!contacts) s->blocked=false;
    *out=(auris_touch_event_t){AURIS_TOUCH_CANCEL,s->last_x,s->last_y};
    return was;
  }
  if (contacts==1) {
    *out=(auris_touch_event_t){s->down ? AURIS_TOUCH_MOVE : AURIS_TOUCH_DOWN,
                              s->x[selected],s->y[selected]};
    s->last_x=out->x; s->last_y=out->y; s->active_id=s->id[selected]; s->down=true;
    return true;
  }
  if (s->down) {
    s->down=false;
    *out=(auris_touch_event_t){AURIS_TOUCH_UP,s->last_x,s->last_y};
    return true;
  }
  return false;
}
bool auris_touch_project(int x,int y,int xmax,int ymax,unsigned int w,unsigned int h,
                         unsigned int iw,unsigned int ih,unsigned int rotation,
                         int *ix,int *iy) {
  if (!ix || !iy || xmax<=0 || ymax<=0 || x<0 || y<0 || x>xmax || y>ymax ||
      !w || !h || !iw || !ih || w>16384U || h>16384U || iw>4096U || ih>4096U ||
      rotation>270U || rotation%90U) return false;
  unsigned int rw=rotation%180U ? ih:iw, rh=rotation%180U ? iw:ih;
  unsigned int fw=w,fh=h;
  if ((uint64_t)w*rh<=(uint64_t)h*rw) fh=(unsigned int)((uint64_t)w*rh/rw);
  else fw=(unsigned int)((uint64_t)h*rw/rh);
  if (!fw) fw=1U;
  if (!fh) fh=1U;
  unsigned int px=(unsigned int)((uint64_t)(unsigned int)x*(w-1U)/(unsigned int)xmax);
  unsigned int py=(unsigned int)((uint64_t)(unsigned int)y*(h-1U)/(unsigned int)ymax);
  unsigned int ox=(w-fw)/2U,oy=(h-fh)/2U;
  if (px<ox || py<oy || px>=ox+fw || py>=oy+fh) return false;
  unsigned int rx=(unsigned int)(((uint64_t)(px-ox)*2U+1U)*rw/(2U*fw));
  unsigned int ry=(unsigned int)(((uint64_t)(py-oy)*2U+1U)*rh/(2U*fh));
  switch (rotation) {
    case 90U: *ix=(int)ry; *iy=(int)(ih-1U-rx); break;
    case 180U: *ix=(int)(iw-1U-rx); *iy=(int)(ih-1U-ry); break;
    case 270U: *ix=(int)(iw-1U-ry); *iy=(int)rx; break;
    default: *ix=(int)rx; *iy=(int)ry; break;
  }
  return true;
}
struct auris_touch {
  int fd;
  struct input_absinfo x,y;
  auris_touch_decoder_t decoder;
};
static int resync(auris_touch_t *s) {
  int32_t ids[33]={ABS_MT_TRACKING_ID};
  if (ioctl(s->fd,EVIOCGMTSLOTS((unsigned int)(s->decoder.count+1)*sizeof(int32_t)),ids)<0)
    return -errno;
  struct input_absinfo slot;
  if (ioctl(s->fd,EVIOCGABS(ABS_MT_SLOT),&slot)<0) return -errno;
  s->decoder.slot=slot.value;
  for (int i=0;i<s->decoder.count;++i) s->decoder.id[i]=ids[i+1];
  s->decoder.down=false; s->decoder.blocked=true; s->decoder.dropped=false;
  return 0;
}
static int open_path(auris_touch_t *s,const char *path) {
  s->fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
  if (s->fd<0) return -errno;
  struct input_absinfo slots;
  unsigned long prop=0U;
  int err=-ENOTSUP;
  if (ioctl(s->fd,EVIOCGPROP(sizeof(prop)),&prop)<0 || !(prop&(1UL<<INPUT_PROP_DIRECT)) ||
      ioctl(s->fd,EVIOCGABS(ABS_MT_SLOT),&slots)<0 || slots.minimum!=0 ||
      slots.maximum<0 || slots.maximum>=32 ||
      ioctl(s->fd,EVIOCGABS(ABS_MT_POSITION_X),&s->x)<0 ||
      ioctl(s->fd,EVIOCGABS(ABS_MT_POSITION_Y),&s->y)<0 ||
      s->x.maximum<=s->x.minimum || s->y.maximum<=s->y.minimum) goto fail;
  auris_touch_decoder_init(&s->decoder,slots.maximum+1);
  err=resync(s);
  if (err<0) goto fail;
  /* No finger held at startup: allow the first new gesture. */
  s->decoder.blocked=false;
  for (int i=0;i<s->decoder.count;++i) if (s->decoder.id[i]>=0) s->decoder.blocked=true;
  fprintf(stderr,"touch: %s, X %d..%d Y %d..%d, %d slots\n",path,
      s->x.minimum,s->x.maximum,s->y.minimum,s->y.maximum,s->decoder.count);
  return 0;
fail:
  (void)close(s->fd); s->fd=-1; return err;
}
int auris_touch_open(auris_touch_t **out,const char *path) {
  if (!out || *out) return -EINVAL;
  auris_touch_t *s=calloc(1U,sizeof(*s));
  if (!s) return -ENOMEM;
  s->fd=-1;
  int err=-ENODEV;
  if (path) err=open_path(s,path);
  else {
    glob_t files={0};
    if (!glob("/dev/input/event*",0,NULL,&files))
      for (size_t i=0U;i<files.gl_pathc;++i) { err=open_path(s,files.gl_pathv[i]); if (!err) break; }
    globfree(&files);
  }
  if (err) { free(s); return err; }
  *out=s; return 0;
}
int auris_touch_poll(auris_touch_t *s,auris_touch_callback_t callback,void *context) {
  if (!s || !callback) return -EINVAL;
  for (unsigned int batch=0U;batch<16U;++batch) {
    struct input_event events[32];
    ssize_t got=read(s->fd,events,sizeof(events));
    if (got<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    if (got<0 && errno==EINTR) continue;
    if (got<=0) return got<0 ? -errno : -ENODEV;
    if ((size_t)got%sizeof(events[0])) return -EIO;
    for (size_t i=0U;i<(size_t)got/sizeof(events[0]);++i) {
      if (s->decoder.dropped && events[i].type==EV_SYN && events[i].code==SYN_REPORT) {
        int err=resync(s); if (err) return err;
        continue;
      }
      auris_touch_event_t event;
      if (!auris_touch_decode(&s->decoder,&events[i],&event)) continue;
      int64_t x=(int64_t)event.x-s->x.minimum, y=(int64_t)event.y-s->y.minimum;
      int64_t dx=(int64_t)s->x.maximum-s->x.minimum,dy=(int64_t)s->y.maximum-s->y.minimum;
      if (x<0 || x>dx || y<0 || y>dy) event.kind=AURIS_TOUCH_CANCEL;
      event.x=(int)(x*65535/dx); event.y=(int)(y*65535/dy);
      callback(context,&event);
    }
  }
  return 0;
}
void auris_touch_close(auris_touch_t *s) {
  if (!s) return;
  (void)close(s->fd); free(s);
}
