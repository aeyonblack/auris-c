#include "auris_outputs.h"
#include "auris_stream.h"
#include "auris_display.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct auris_outputs {
  auris_output_t *sink[2];
  int failed[2];
  auris_display_info_t display;
};
static const char *const names[2]={"stream","display"};
int auris_outputs_option(auris_outputs_config_t *c,int argc,char **argv,int *index) {
  if (!c || !argv || !index || *index<0 || *index>=argc) return -EINVAL;
  const char *option=argv[*index];
  if (strcmp(option,"--output") && strcmp(option,"--drm-device") &&
      strcmp(option,"--connector") && strcmp(option,"--rotation")) return 0;
  if (*index+1>=argc) return -EINVAL;
  const char *value=argv[++*index];
  if (!strcmp(option,"--output")) {
    if (!c->explicit_selection) { c->stream=false; c->display=false; }
    c->explicit_selection=true;
    if (!strcmp(value,"stream")) c->stream=true;
    else if (!strcmp(value,"display")) c->display=true;
    else return -EINVAL;
  } else if (!strcmp(option,"--drm-device")) c->drm_device=value;
  else {
    char *end=NULL;
    errno=0;
    unsigned long number=strtoul(value,&end,10);
    if (value[0]<'0' || value[0]>'9' || errno || *end || number>UINT32_MAX) return -EINVAL;
    if (!strcmp(option,"--connector")) c->connector=(uint32_t)number;
    else {
      if (number>270U || number%90U) return -EINVAL;
      c->rotation=(unsigned int)number;
    }
  }
  return 1;
}
int auris_outputs_open(auris_outputs_t **out,const auris_outputs_config_t *c,
                       int fd,uint32_t w,uint32_t h) {
  if (!out || *out || !c) return -EINVAL;
  auris_outputs_t *s=calloc(1U,sizeof(*s));
  if (!s) return -ENOMEM;
  const bool stream=c->stream || !c->explicit_selection;
  if (stream) s->failed[0]=auris_output_open_jpeg_stream(&s->sink[0],fd,w,h,80U);
  if (c->display) {
    const auris_display_config_t display={c->drm_device,c->connector,c->rotation};
    s->failed[1]=auris_output_open_display_info(&s->sink[1],w,h,&display,&s->display);
  }
  for (size_t i=0U;i<2U;++i)
    if (s->failed[i]<0) fprintf(stderr,"%s output unavailable: %s\n",names[i],strerror(-s->failed[i]));
  if (!s->sink[0] && !s->sink[1]) {
    int err=s->failed[1] ? s->failed[1] : s->failed[0] ? s->failed[0] : -EINVAL;
    free(s); return err;
  }
  *out=s; return 0;
}
static int available(auris_outputs_t *s) {
  if (!s) return -EINVAL;
  bool alive=false;
  for (size_t i=0U;i<2U;++i) {
    if (!s->sink[i] || s->failed[i]) continue;
    int err=auris_output_error(s->sink[i]);
    if (err<0) {
      s->failed[i]=err;
      fprintf(stderr,"%s output stopped: %s; other outputs continue\n",names[i],strerror(-err));
    } else alive=true;
  }
  return alive ? 0 : -EPIPE;
}
int auris_outputs_error(auris_outputs_t *s) { return available(s); }
int auris_outputs_submit(auris_outputs_t *s,const auris_rgb24_t *rgb,
                         const auris_output_frame_t *f) {
  if (!s) return -EINVAL;
  for (size_t i=0U;i<2U;++i) {
    if (!s->sink[i] || s->failed[i]) continue;
    int err=auris_output_submit(s->sink[i],rgb,f);
    if (err==-EINVAL) return err;
    /* Worker failures are recorded and reported by available(). */
    if (err<0 && !auris_output_error(s->sink[i])) return err;
  }
  return available(s);
}
int auris_outputs_flush(auris_outputs_t *s,unsigned int timeout_ms) {
  if (!s) return -EINVAL;
  int result=0;
  for (size_t i=0U;i<2U;++i)
    if (s->sink[i] && !s->failed[i]) {
      int err=auris_output_flush(s->sink[i],timeout_ms);
      if (err<0) result=err;
    }
  return result;
}
void auris_outputs_stats(const auris_outputs_t *s,auris_output_stats_t *stats) {
  auris_output_stats(s ? (s->sink[0] && !s->failed[0] ? s->sink[0] : s->sink[1]) : NULL,stats);
}
void auris_outputs_close(auris_outputs_t *s) {
  if (!s) return;
  for (size_t i=0U;i<2U;++i) if (s->sink[i]) {
    auris_output_stats_t stats;
    auris_output_stats(s->sink[i],&stats);
    fprintf(stderr,"%s: submitted %llu, presented %llu, replaced %llu\n",names[i],
        (unsigned long long)stats.submitted,(unsigned long long)stats.sent,
        (unsigned long long)stats.dropped);
    auris_output_close(s->sink[i]);
  }
  free(s);
}
bool auris_outputs_display_info(const auris_outputs_t *s,auris_display_info_t *info) {
  if (!s || !info || !s->sink[1] || s->failed[1]) return false;
  *info=s->display; return true;
}
