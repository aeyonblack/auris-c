#include "auris_display.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

typedef struct {
  uint32_t handle, fb, pitch;
  size_t bytes;
  uint32_t *map;
} buffer_t;
typedef struct {
  int fd;
  uint32_t connector, crtc;
  drmModeModeInfo mode;
  drmModeCrtc *saved;
  uint32_t *saved_connectors;
  int saved_count;
  buffer_t buffers[2];
  unsigned int front, rotation;
  bool modeset, pending;
  uint64_t last_check;
  /* Display-plane path: the picture is packed at its own (rotated) size into
     `small` and an overlay plane scales it to dst_* on the panel, while the
     primary plane shows buffers[0], left black. plane 0: CPU path. */
  uint32_t plane, small_w, small_h, dst_x, dst_y, dst_w, dst_h;
  buffer_t small[2];
  bool plane_shown;
  /* Overlay planes that could carry it, tried in order until one accepts:
     on the AM68/TDA4 display controller only some pipelines can scale. */
  uint32_t candidates[8];
  size_t candidate_count, candidate_next;
} display_t;
static void raise_plane(display_t *s,uint32_t found);

static uint64_t now_us(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec*1000000U+(uint64_t)t.tv_nsec/1000U;
}
static void flipped(int fd, unsigned int sequence, unsigned int sec,
                     unsigned int usec, void *context) {
  (void)fd; (void)sequence; (void)sec; (void)usec;
  display_t *s=context;
  s->pending=false;
}
static int connected(display_t *s) {
  drmModeConnector *c=drmModeGetConnector(s->fd,s->connector);
  if (!c) return -errno;
  const bool yes=c->connection==DRM_MODE_CONNECTED;
  drmModeFreeConnector(c);
  return yes ? 0 : -ENODEV;
}
static int start(void *context, auris_output_cancel_t cancel, void *token) {
  display_t *s=context;
  if (cancel(token)) return -ECANCELED;
  if (drmModeSetCrtc(s->fd,s->crtc,s->buffers[0].fb,0U,0U,
                     &s->connector,1,&s->mode)) return -errno;
  s->modeset=true;
  return 0;
}
static int present(void *context, const auris_rgb24_t *rgb,
                    const auris_output_frame_t *frame, auris_output_stats_t *stats,
                    auris_output_cancel_t cancel, void *token) {
  (void)frame;
  display_t *s=context;
  uint64_t began=now_us();
  if (began-s->last_check>=1000000U) {
    int err=connected(s);
    if (err<0) return err;
    s->last_check=began;
  }
  if (s->plane) {
    buffer_t *p=&s->small[1U-s->front];
    int err=auris_display_pack(rgb,p->map,s->small_w,s->small_h,p->pitch,
                               p->bytes,s->rotation);
    stats->encode_us=now_us()-began;
    stats->encoded_bytes=p->bytes;
    if (err<0) return err;
    if (cancel(token)) return -ECANCELED;
    began=now_us();
    for (;;) {
      /* Waits for the vertical blank, like the page flip below. */
      if (drmModeSetPlane(s->fd,s->plane,s->crtc,p->fb,0U,(int32_t)s->dst_x,
                          (int32_t)s->dst_y,s->dst_w,s->dst_h,0U,0U,
                          s->small_w<<16U,s->small_h<<16U)==0) {
        if (!s->plane_shown)
          fprintf(stderr,"display: plane %u scales %ux%u to %ux%u at %u,%u\n",s->plane,
                  s->small_w,s->small_h,s->dst_w,s->dst_h,s->dst_x,s->dst_y);
        s->plane_shown=true;
        s->front=1U-s->front;
        stats->write_us=now_us()-began;
        return 0;
      }
      err=-errno;
      if (s->plane_shown) return err;
      fprintf(stderr,"display: plane %u refused the picture (%s)\n",s->plane,strerror(-err));
      if (s->candidate_next>=s->candidate_count) break;
      s->plane=s->candidates[s->candidate_next++];
      raise_plane(s,s->plane);
    }
    fprintf(stderr,"display: no plane can scale it; scaling on the CPU\n");
    s->plane=0U;
    s->front=0U;
    began=now_us();
  }
  buffer_t *b=&s->buffers[1U-s->front];
  int err=auris_display_pack(rgb,b->map,s->mode.hdisplay,s->mode.vdisplay,
                             b->pitch,b->bytes,s->rotation);
  stats->encode_us=now_us()-began;
  stats->encoded_bytes=b->bytes;
  if (err<0) return err;
  if (cancel(token)) return -ECANCELED;
  began=now_us();
  if (drmModePageFlip(s->fd,s->crtc,b->fb,DRM_MODE_PAGE_FLIP_EVENT,s)) return -errno;
  s->pending=true;
  drmEventContext events={0};
  events.version=DRM_EVENT_CONTEXT_VERSION;
  events.page_flip_handler=flipped;
  while (s->pending) {
    if (cancel(token)) return -ECANCELED;
    if (now_us()-began>1000000U) return -ETIMEDOUT;
    struct pollfd p={.fd=s->fd,.events=POLLIN};
    int ready=poll(&p,1U,50);
    if (ready<0) { if (errno==EINTR) continue; return -errno; }
    if (p.revents&(POLLERR|POLLHUP|POLLNVAL)) return -EIO;
    if ((p.revents&POLLIN) && drmHandleEvent(s->fd,&events)) return -errno;
  }
  s->front=1U-s->front;
  stats->write_us=now_us()-began;
  return 0;
}
static void close_display(void *context) {
  display_t *s=context;
  if (!s) return;
  if (s->fd>=0 && s->plane_shown)
    (void)drmModeSetPlane(s->fd,s->plane,s->crtc,0U,0U,0,0,0U,0U,0U,0U,0U,0U);
  if (s->fd>=0 && s->modeset && s->saved) {
    int rc;
    if (s->saved->mode_valid)
      rc=drmModeSetCrtc(s->fd,s->saved->crtc_id,s->saved->buffer_id,
                         s->saved->x,s->saved->y,s->saved_connectors,
                         s->saved_count,&s->saved->mode);
    else rc=drmModeSetCrtc(s->fd,s->crtc,0U,0U,0U,NULL,0,NULL);
    if (rc) fprintf(stderr,"display: previous mode restore failed: %s\n",strerror(errno));
  }
  for (size_t i=0U;i<4U;++i) {
    buffer_t *b=i<2U ? &s->buffers[i] : &s->small[i-2U];
    if (b->map) (void)munmap(b->map,b->bytes);
    if (b->fb) (void)drmModeRmFB(s->fd,b->fb);
    if (b->handle) {
      struct drm_mode_destroy_dumb d={.handle=b->handle};
      (void)drmIoctl(s->fd,DRM_IOCTL_MODE_DESTROY_DUMB,&d);
    }
  }
  if (s->saved) drmModeFreeCrtc(s->saved);
  free(s->saved_connectors);
  if (s->fd>=0) { (void)drmDropMaster(s->fd); (void)close(s->fd); }
  free(s);
}
static int allocate_sized(display_t *s,buffer_t *b,uint32_t w,uint32_t h) {
  struct drm_mode_create_dumb create={0};
  create.width=w; create.height=h; create.bpp=32U;
  if (drmIoctl(s->fd,DRM_IOCTL_MODE_CREATE_DUMB,&create)) return -errno;
  b->handle=create.handle; b->pitch=create.pitch;
  if (create.size>SIZE_MAX) return -EOVERFLOW;
  b->bytes=(size_t)create.size;
  uint32_t handles[4]={b->handle,0,0,0}, pitches[4]={b->pitch,0,0,0}, offsets[4]={0};
  if (drmModeAddFB2(s->fd,create.width,create.height,DRM_FORMAT_XRGB8888,
                     handles,pitches,offsets,&b->fb,0U)) return -errno;
  struct drm_mode_map_dumb map={.handle=b->handle};
  if (drmIoctl(s->fd,DRM_IOCTL_MODE_MAP_DUMB,&map)) return -errno;
  void *memory=mmap(NULL,b->bytes,PROT_READ|PROT_WRITE,MAP_SHARED,s->fd,(off_t)map.offset);
  if (memory==MAP_FAILED) return -errno;
  b->map=memory; memset(b->map,0,b->bytes);
  return 0;
}
static int allocate_buffer(display_t *s,buffer_t *b) {
  return allocate_sized(s,b,s->mode.hdisplay,s->mode.vdisplay);
}
/* Raises an overlay plane above the primary plane where it has zpos. */
static void raise_plane(display_t *s,uint32_t found) {
  drmModeObjectProperties *props=drmModeObjectGetProperties(s->fd,found,DRM_MODE_OBJECT_PLANE);
  if (props) {
    for (uint32_t i=0U;i<props->count_props;++i) {
      drmModePropertyRes *prop=drmModeGetProperty(s->fd,props->props[i]);
      if (!prop) continue;
      if (!strcmp(prop->name,"zpos") && (prop->flags&DRM_MODE_PROP_RANGE) &&
          prop->count_values==2 && prop->values[1]>=1U && props->prop_values[i]<1U)
        (void)drmModeObjectSetProperty(s->fd,found,DRM_MODE_OBJECT_PLANE,prop->prop_id,1U);
      drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
  }
}
/* Overlay planes that can show XRGB8888 on our CRTC (legacy clients only see
   overlay planes), in the driver's order; returns the first. */
static uint32_t find_planes(display_t *s) {
  drmModeRes *r=drmModeGetResources(s->fd);
  if (!r) return 0U;
  int index=-1;
  for (int i=0;i<r->count_crtcs;++i) if (r->crtcs[i]==s->crtc) index=i;
  drmModeFreeResources(r);
  if (index<0 || index>=32) return 0U;
  drmModePlaneRes *planes=drmModeGetPlaneResources(s->fd);
  if (!planes) return 0U;
  s->candidate_count=0U;
  for (uint32_t i=0U;i<planes->count_planes && s->candidate_count<8U;++i) {
    drmModePlane *p=drmModeGetPlane(s->fd,planes->planes[i]);
    if (!p) continue;
    if ((p->possible_crtcs&(1U<<(unsigned int)index)) && !p->crtc_id)
      for (uint32_t f=0U;f<p->count_formats;++f)
        if (p->formats[f]==DRM_FORMAT_XRGB8888) {
          s->candidates[s->candidate_count++]=p->plane_id;
          break;
        }
    drmModeFreePlane(p);
  }
  drmModeFreePlaneResources(planes);
  if (!s->candidate_count) return 0U;
  s->candidate_next=1U;
  raise_plane(s,s->candidates[0]);
  return s->candidates[0];
}
static int select_display(display_t *s, const char *path, uint32_t requested) {
  s->fd=open(path,O_RDWR|O_CLOEXEC);
  if (s->fd<0) return -errno;
  drmModeRes *r=drmModeGetResources(s->fd);
  if (!r) return -ENODEV;
  int err=-ENODEV;
  for (int i=0;i<r->count_connectors && !s->connector;++i) {
    drmModeConnector *c=drmModeGetConnector(s->fd,r->connectors[i]);
    if (!c) continue;
    if (c->connection==DRM_MODE_CONNECTED && c->count_modes>0 &&
        (!requested || c->connector_id==requested)) {
      uint32_t crtc=0U;
      for (int e=0;e<c->count_encoders && !crtc;++e) {
        drmModeEncoder *encoder=drmModeGetEncoder(s->fd,c->encoders[e]);
        if (!encoder) continue;
        if (encoder->encoder_id==c->encoder_id) crtc=encoder->crtc_id;
        for (int j=0;j<r->count_crtcs && !crtc && j<32;++j) {
          if (!(encoder->possible_crtcs&(1U<<(unsigned int)j))) continue;
          drmModeCrtc *candidate=drmModeGetCrtc(s->fd,r->crtcs[j]);
          if (candidate && !candidate->mode_valid) crtc=r->crtcs[j];
          if (candidate) drmModeFreeCrtc(candidate);
        }
        drmModeFreeEncoder(encoder);
      }
      if (crtc) {
        s->crtc=crtc; s->connector=c->connector_id; s->mode=c->modes[0];
        for (int m=0;m<c->count_modes;++m)
          if (c->modes[m].type&DRM_MODE_TYPE_PREFERRED) { s->mode=c->modes[m]; break; }
        err=0;
      }
    }
    drmModeFreeConnector(c);
  }
  if (!err) {
    s->saved=drmModeGetCrtc(s->fd,s->crtc);
    s->saved_connectors=calloc((size_t)r->count_connectors,sizeof(uint32_t));
    if (!s->saved || !s->saved_connectors) err=-ENOMEM;
    else for (int i=0;i<r->count_connectors;++i) {
      drmModeConnector *c=drmModeGetConnector(s->fd,r->connectors[i]);
      if (!c) continue;
      drmModeEncoder *e=c->encoder_id ? drmModeGetEncoder(s->fd,c->encoder_id) : NULL;
      if (e && e->crtc_id==s->crtc) s->saved_connectors[s->saved_count++]=c->connector_id;
      if (e) drmModeFreeEncoder(e);
      drmModeFreeConnector(c);
    }
  }
  drmModeFreeResources(r);
  return err;
}
int auris_output_open_display(auris_output_t **out,uint32_t w,uint32_t h,
                              const auris_display_config_t *config) {
  return auris_output_open_display_info(out,w,h,config,NULL);
}
int auris_output_open_display_info(auris_output_t **out,uint32_t w,uint32_t h,
                              const auris_display_config_t *config,auris_display_info_t *info) {
  if (!out || *out || !config || config->rotation>270U || config->rotation%90U ||
      !w || !h || w>4096U || h>4096U) return -EINVAL;
  display_t *s=calloc(1U,sizeof(*s));
  if (!s) return -ENOMEM;
  s->fd=-1; s->rotation=config->rotation;
  int err=-ENODEV;
  if (config->device) err=select_display(s,config->device,config->connector);
  else {
    glob_t cards={0};
    if (glob("/dev/dri/card[0-9]*",0,NULL,&cards)==0) {
      for (size_t i=0U;i<cards.gl_pathc;++i) {
        err=select_display(s,cards.gl_pathv[i],config->connector);
        if (!err || s->connector) break;
        if (s->fd>=0) { (void)close(s->fd); s->fd=-1; }
      }
    }
    globfree(&cards);
  }
  if (err<0) goto fail;
  if (!drmIsMaster(s->fd) && drmSetMaster(s->fd)) { err=-errno; goto fail; }
  uint64_t dumb=0U;
  if (drmGetCap(s->fd,DRM_CAP_DUMB_BUFFER,&dumb) || !dumb) { err=-ENOTSUP; goto fail; }
  for (size_t i=0U;i<2U;++i) { err=allocate_buffer(s,&s->buffers[i]); if (err<0) goto fail; }
  /* AURIS_DISPLAY_CPU_SCALE=1 keeps the CPU path, for comparison. */
  const char *cpu=getenv("AURIS_DISPLAY_CPU_SCALE");
  if (!(cpu && *cpu && strcmp(cpu,"0"))) {
    const bool turn=s->rotation==90U || s->rotation==270U;
    s->small_w=turn ? h : w; s->small_h=turn ? w : h;
    s->plane=find_planes(s);
    if (s->plane && (auris_display_fit(s->small_w,s->small_h,s->mode.hdisplay,
                                       s->mode.vdisplay,&s->dst_x,&s->dst_y,
                                       &s->dst_w,&s->dst_h) ||
                     allocate_sized(s,&s->small[0],s->small_w,s->small_h)<0 ||
                     allocate_sized(s,&s->small[1],s->small_w,s->small_h)<0))
      s->plane=0U;
  }
  if (s->plane)
    fprintf(stderr,"display: connector %u, preferred mode %ux%u, rotation %u, XRGB8888, "
            "%zu overlay plane(s) to try for scaling %ux%u to %ux%u\n",s->connector,
            s->mode.hdisplay,s->mode.vdisplay,s->rotation,s->candidate_count,s->small_w,
            s->small_h,s->dst_w,s->dst_h);
  else
    fprintf(stderr,"display: connector %u, preferred mode %ux%u, rotation %u, XRGB8888, "
            "scaled on the CPU\n",s->connector,s->mode.hdisplay,s->mode.vdisplay,s->rotation);
  const auris_output_backend_t backend={start,present,close_display};
  if (info) *info=(auris_display_info_t){s->mode.hdisplay,s->mode.vdisplay,s->rotation};
  return auris_output_open_backend(out,w,h,s,&backend);
fail:
  close_display(s); return err;
}
