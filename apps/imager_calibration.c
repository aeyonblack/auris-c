#include "imager_calibration.h"
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double angle(auris_vec3f_t a, auris_vec3f_t b) {
  const double ax=a.x_m,ay=a.y_m,az=a.z_m,bx=b.x_m,by=b.y_m,bz=b.z_m;
  return 57.29577951308232*atan2(hypot(hypot(ay*bz-az*by,az*bx-ax*bz),ax*by-ay*bx),
                                ax*bx+ay*by+az*bz);
}

void imager_calibration_feed(imager_calibration_capture_t *c,
    auris_vec3f_t ray, uint64_t sample_us, uint64_t now, bool evidence) {
  if (c==NULL || !c->active || c->count>=CAL_CAPTURE_SAMPLES || now<c->start_us ||
      now>c->deadline_us) return;
  if (!evidence || sample_us<c->start_us || sample_us>now || now-sample_us>150000U ||
      !isfinite(ray.x_m) || !isfinite(ray.y_m) || !isfinite(ray.z_m)) {
    ++c->rejected; return;
  }
  c->rays[c->count++]=ray;
}

int imager_calibration_average(const imager_calibration_capture_t *c,
                               auris_vec3f_t *ray, double *scatter) {
  if (c==NULL || ray==NULL || scatter==NULL || c->count!=CAL_CAPTURE_SAMPLES ||
      c->rejected>CAL_CAPTURE_SAMPLES/4U) return -EAGAIN;
  double x=0,y=0,z=0;
  for (size_t i=0; i<c->count; ++i) {
    x+=c->rays[i].x_m; y+=c->rays[i].y_m; z+=c->rays[i].z_m;
  }
  const double length=hypot(hypot(x,y),z);
  if (!isfinite(length) || length<1.0) return -ERANGE;
  const auris_vec3f_t mean={(float)(x/length),(float)(y/length),(float)(z/length)};
  double sum=0,max=0;
  for (size_t i=0; i<c->count; ++i) {
    const double e=angle(c->rays[i],mean); sum+=e*e; max=fmax(max,e);
  }
  *scatter=sqrt(sum/(double)c->count);
  if (*scatter>1.5 || max>3.0) return -ERANGE;
  *ray=mean; return 0;
}

static bool replace_view(imager_calibration_t *s, const auris_view_t *view) {
  auris_overlay_config_t next=s->drawing;
  next.camera=*view;
  auris_overlay_t *replacement=NULL;
  const int err=auris_overlay_open(&replacement,&next);
  if (err<0) {
    fprintf(stderr,"CAL rejected geometry: %s (current preview preserved)\n",strerror(-err));
    return false;
  }
  auris_overlay_close(s->overlay);
  s->overlay=replacement; s->drawing=next;
  return true;
}

static bool invalidate(imager_calibration_t *s) {
  if (!replace_view(s,&s->baseline)) return false;
  s->solved=false; s->check_count=0;
  return true;
}

void imager_calibration_frame(imager_calibration_t *s, uint32_t seq,
                              uint64_t timestamp, bool monotonic) {
  const size_t i=s->next_frame++ % 16U;
  s->frames[i].sequence=seq; s->frames[i].timestamp=timestamp;
  s->frames[i].valid=monotonic;
}

static bool begin(imager_calibration_t *s, uint32_t seq, float x, float y,
                   bool checking, uint64_t now, imager_calibration_capture_t *capture) {
  if (s->pending || (!checking && s->count==AURIS_CALIBRATION_MAX_POINTS) ||
      (checking && (!s->solved || s->check_count==AURIS_CALIBRATION_MAX_POINTS))) return false;
  bool current=false;
  for (size_t i=0; i<16; ++i)
    if (s->frames[i].valid && s->frames[i].sequence==seq &&
        now>=s->frames[i].timestamp && now-s->frames[i].timestamp<=1000000U) current=true;
  const auris_projection_layout_t *l=&s->drawing.layout;
  if (!current || !isfinite(x) || !isfinite(y) || x<(float)l->content_x ||
      y<(float)l->content_y || x>(float)(l->content_x+l->content_width-1U) ||
      y>(float)(l->content_y+l->content_height-1U)) return false;
  auris_view_t unrotated=s->baseline;
  for (size_t i=0; i<3; ++i) for (size_t j=0; j<3; ++j)
    unrotated.rotation[i][j]=i==j ? 1.0f:0.0f;
  const float u=fminf((float)(unrotated.width-1U),fmaxf(0,
      (x-(float)l->content_x+0.5f)*(float)unrotated.width/(float)l->content_width-0.5f));
  const float v=fminf((float)(unrotated.height-1U),fmaxf(0,
      (y-(float)l->content_y+0.5f)*(float)unrotated.height/(float)l->content_height-0.5f));
  auris_vec3f_t clicked;
  if (auris_view_ray(&unrotated,u,v,&clicked)!=AURIS_OK) return false;
  for (size_t i=0; i<s->count; ++i)
    if (angle(clicked,s->pairs[i].camera)<(checking ? 2.0:4.0)) return false;
  if (checking) for (size_t i=0; i<s->check_count; ++i)
    if (angle(clicked,s->checks[i].camera)<4.0) return false;
  if (!checking && !invalidate(s)) return false;
  s->clicked=clicked; s->pending=true; s->checking=checking;
  *capture=(imager_calibration_capture_t){.active=true,.start_us=now+300000U,
                                         .deadline_us=now+2500000U};
  fprintf(stderr,"CAL collecting %s: hold source still for about 1.5 seconds\n",
          checking ? "validation point":"fit point");
  return true;
}

void imager_calibration_finish(imager_calibration_t *s,
                               const imager_calibration_capture_t *capture) {
  if (!s->pending) return;
  s->pending=false;
  auris_vec3f_t measured; double scatter=0;
  if (imager_calibration_average(capture,&measured,&scatter)<0) {
    fprintf(stderr,"CAL rejected unstable/insufficient evidence: %zu/64 good, %zu rejected, scatter %.2f deg\n",
        capture->count,capture->rejected,scatter);
    return;
  }
  const auris_calibration_pair_t pair={s->clicked,measured};
  if (s->checking) {
    s->checks[s->check_count++]=pair;
    double deg,px;
    if (auris_calibration_error(&s->candidate,pair,&deg,&px)<0) { deg=180; px=INFINITY; }
    fprintf(stderr,"CAL validation %zu: %.3f deg, %.2f source pixels; scatter %.2f deg\n",
            s->check_count,deg,px,scatter);
  } else {
    s->pairs[s->count++]=pair;
    fprintf(stderr,"CAL accepted fit point %zu/32; scatter %.2f deg\n",s->count,scatter);
  }
  fprintf(stderr,"CAL PAIR %s camera %.9g %.9g %.9g array %.9g %.9g %.9g\n",
      s->checking ? "check":"fit",(double)pair.camera.x_m,(double)pair.camera.y_m,
      (double)pair.camera.z_m,(double)pair.array.x_m,(double)pair.array.y_m,(double)pair.array.z_m);
}

static void solve(imager_calibration_t *s) {
  auris_calibration_result_t result;
  if (auris_calibration_solve(s->pairs,s->count,3.0,&result)<0) {
    fprintf(stderr,"CAL fit rejected: need >=6 spread-out positions, >=75%% consensus within 3 deg\n");
    return;
  }
  auris_view_t candidate=s->baseline;
  memcpy(candidate.rotation,result.rotation,sizeof(candidate.rotation));
  if (!replace_view(s,&candidate)) return;
  s->candidate=candidate; s->solved=true; s->check_count=0;
  double before=0,after=0;
  for (size_t i=0; i<s->count; ++i) {
    double da,pa,db,pb;
    if (auris_calibration_error(&s->baseline,s->pairs[i],&db,&pb)<0) { db=180; pb=INFINITY; }
    if (auris_calibration_error(&candidate,s->pairs[i],&da,&pa)<0) { da=180; pa=INFINITY; }
    if (result.used[i]) { before+=pb*pb; after+=pa*pa; }
    fprintf(stderr,"CAL fit point %zu %s: %.2f -> %.2f deg; %.1f -> %.1f source px\n",
        i+1U,result.used[i] ? "inlier":"OUTLIER",db,da,pb,pa);
  }
  fprintf(stderr,"CAL PREVIEW: %zu/%zu inliers; RMS %.3f deg; RMS source px %.2f -> %.2f. Collect >=3 NEW validation points before save.\n",
      result.inliers,s->count,result.rms_deg,sqrt(before/(double)result.inliers),
      sqrt(after/(double)result.inliers));
}

void imager_calibration_command(imager_calibration_t *s, const char *line,
                                uint64_t now, imager_calibration_capture_t *capture) {
  const bool point=strncmp(line,"point ",6)==0, check=strncmp(line,"check ",6)==0;
  if (point || check) {
    char *end;
    const char *token=line+6;
    errno=0;
    const uintmax_t seq=strtoumax(token,&end,10);
    bool valid=token[0]!='-' && token[0]!='+' && end!=token && errno==0 &&
               seq<=UINT32_MAX && isspace((unsigned char)*end);
    token=end; errno=0;
    const float x=strtof(token,&end);
    valid=valid && end!=token && errno==0 && isspace((unsigned char)*end);
    token=end; errno=0;
    const float y=strtof(token,&end);
    valid=valid && end!=token && errno==0;
    while (isspace((unsigned char)*end)) ++end;
    if (!valid || *end!='\0' || !begin(s,(uint32_t)seq,x,y,check,now,capture))
      fprintf(stderr,"CAL click rejected: stale frame, border, duplicate, busy, or no fit for validation\n");
    return;
  }
  if (strcmp(line,"cancel")==0) {
    capture->active=false; s->pending=false; fprintf(stderr,"CAL collection cancelled\n"); return;
  }
  if (s->pending) { fprintf(stderr,"CAL busy: wait or cancel the current collection\n"); return; }
  if (strcmp(line,"solve")==0) { solve(s); return; }
  if (strcmp(line,"baseline")==0) { (void)replace_view(s,&s->baseline); fprintf(stderr,"CAL baseline preview\n"); return; }
  if (strcmp(line,"preview")==0 && s->solved) { (void)replace_view(s,&s->candidate); fprintf(stderr,"CAL fitted preview\n"); return; }
  if (strcmp(line,"clear")==0 || strcmp(line,"undo")==0) {
    if (!invalidate(s)) return;
    if (line[0]=='c') s->count=0;
    else if (s->count!=0U) --s->count;
    fprintf(stderr,"CAL %zu fit points remain; candidate/validation cleared\n",s->count); return;
  }
  if (strcmp(line,"save")==0) {
    if (!s->solved) {
      fprintf(stderr,"CAL save rejected: solve and validate a candidate first\n"); return;
    }
    double sum=0,max=0;
    for (size_t i=0; i<s->check_count; ++i) {
      double d,p;
      if (auris_calibration_error(&s->candidate,s->checks[i],&d,&p)<0) d=180;
      sum+=d*d; max=fmax(max,d);
    }
    const double rms=s->check_count==0 ? INFINITY:sqrt(sum/(double)s->check_count);
    if (s->check_count<3U || rms>2.5 || max>4.0) {
      fprintf(stderr,"CAL save rejected: validation %zu, RMS %.2f deg, max %.2f deg (need >=3, <=2.5, <=4)\n",
          s->check_count,rms,max); return;
    }
    const int err=auris_camera_calibration_save(s->path,s->array_id,&s->candidate);
    if (err<0) fprintf(stderr,"CAL save failed: %s\n",strerror(-err));
    else fprintf(stderr,"CAL SAVED %s; held-out RMS %.3f deg, max %.3f deg\n",s->path,rms,max);
    return;
  }
  fprintf(stderr,"CAL unknown/unavailable command: %s\n",line);
}
