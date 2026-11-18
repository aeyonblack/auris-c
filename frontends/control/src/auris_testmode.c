#include "auris_testmode.h"
#include "auris_ui.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { int x,y,w,h; } rect_t;
/* Same rectangles drive drawing and hit testing, as in the settings panel.
   While a clip records, everything stays in the two side bars: the picture
   between them is what the report shows. */
#define AURIS_PI 3.14159265f
#define RAIL_RIGHT 564
#define RAIL_WIDTH 72
static const rect_t exit_button={536,6,96,30};
static const rect_t location_buttons[AURIS_TESTPLAN_MAX_LOCATIONS]={
  {8,44,150,40},{164,44,150,40},{320,44,150,40},{476,44,150,40}};
#define ROW_TOP 92
#define ROW_HEIGHT 32
static const rect_t start_button={RAIL_RIGHT,126,RAIL_WIDTH,64};
static const rect_t later_button={RAIL_RIGHT,196,RAIL_WIDTH,40};
static const rect_t skip_button={RAIL_RIGHT,242,RAIL_WIDTH,40};
static const rect_t back_button={RAIL_RIGHT,288,RAIL_WIDTH,40};
static const rect_t cancel_button={RAIL_RIGHT,288,RAIL_WIDTH,40};
static const rect_t keep_button={RAIL_RIGHT,196,RAIL_WIDTH,64};
static const rect_t redo_button={RAIL_RIGHT,288,RAIL_WIDTH,40};
enum { B_NONE=-1,B_EXIT=0,B_LOCATION=1,B_ROW=10,B_START=30,B_LATER,B_SKIP,B_BACK,
       B_CANCEL,B_KEEP,B_REDO };

static bool inside(rect_t r,int x,int y) {
  return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;
}
static rect_t row_rect(size_t index) {
  return (rect_t){8,ROW_TOP+(int)index*ROW_HEIGHT,624,ROW_HEIGHT-4};
}
static bool clip_here(const auris_testmode_t *m,size_t location,size_t test,size_t clip) {
  const auris_testplan_clip_t *c=&m->plan->clips[clip];
  return (c->locations&(1U<<location))!=0U && c->test==m->plan->tests[test].number;
}
void auris_testmode_count(const auris_testmode_t *m,size_t location,size_t test,
                          size_t *done,size_t *total) {
  *done=0U; *total=0U;
  for (size_t i=0U;i<m->plan->clip_count;++i) {
    if (!clip_here(m,location,test,i)) continue;
    ++*total;
    if (m->done[location][i]) ++*done;
  }
}
/* The next clip not done, starting after `from`; false if none is left. */
static bool next_clip(auris_testmode_t *m,size_t from) {
  const size_t count=m->plan->clip_count;
  for (size_t step=1U;step<=count;++step) {
    const size_t i=(from+step)%count;
    if (clip_here(m,m->location,m->test,i) && !m->done[m->location][i]) {
      m->clip=i;
      return true;
    }
  }
  return false;
}
void auris_testmode_init(auris_testmode_t *m,const auris_testplan_t *plan,
                         const auris_testmode_layout_t *layout) {
  memset(m,0,sizeof(*m));
  m->plan=plan;
  m->layout=*layout;
  m->pressed=B_NONE;
}
void auris_testmode_open(auris_testmode_t *m,unsigned int free_mb) {
  m->phase=AURIS_TESTMODE_CHOOSE;
  m->pressed=B_NONE;
  m->pending=AURIS_TESTMODE_ACT_NONE;
  m->free_mb=free_mb;
  m->message[0]='\0';
}
const auris_testplan_clip_t *auris_testmode_clip(const auris_testmode_t *m) {
  return &m->plan->clips[m->clip];
}
bool auris_testmode_recording(const auris_testmode_t *m) {
  return m->phase==AURIS_TESTMODE_QUIET || m->phase==AURIS_TESTMODE_SOURCE;
}
static void enter(auris_testmode_t *m,auris_testmode_phase_t phase,float seconds,uint64_t now) {
  m->phase=phase;
  m->phase_start=now;
  m->phase_end=now+(uint64_t)(seconds*1e6f);
}
static int button_at(const auris_testmode_t *m,int x,int y) {
  switch (m->phase) {
    case AURIS_TESTMODE_CHOOSE: {
      if (inside(exit_button,x,y)) return B_EXIT;
      for (size_t i=0U;i<m->plan->location_count;++i)
        if (inside(location_buttons[i],x,y)) return B_LOCATION+(int)i;
      for (size_t i=0U;i<m->plan->test_count;++i)
        if (inside(row_rect(i),x,y)) return B_ROW+(int)i;
      return B_NONE;
    }
    case AURIS_TESTMODE_READY:
      if (inside(start_button,x,y)) return B_START;
      if (inside(later_button,x,y)) return B_LATER;
      if (inside(skip_button,x,y)) return B_SKIP;
      if (inside(back_button,x,y)) return B_BACK;
      return B_NONE;
    case AURIS_TESTMODE_COUNTDOWN: case AURIS_TESTMODE_QUIET: case AURIS_TESTMODE_SOURCE:
      return inside(cancel_button,x,y) ? B_CANCEL : B_NONE;
    case AURIS_TESTMODE_REVIEW:
      if (!m->review.failed && inside(keep_button,x,y)) return B_KEEP;
      return inside(redo_button,x,y) ? B_REDO : B_NONE;
    default: return B_NONE;
  }
}
static void start_countdown(auris_testmode_t *m,float seconds,uint64_t now) {
  if (m->free_mb<AURIS_TESTMODE_MIN_FREE_MB) {
    (void)snprintf(m->message,sizeof(m->message),"NOT ENOUGH DISK SPACE");
    return;
  }
  m->message[0]='\0';
  enter(m,AURIS_TESTMODE_COUNTDOWN,seconds,now);
}
void auris_testmode_touch(auris_testmode_t *m,const auris_touch_event_t *e,uint64_t now) {
  if (m->phase==AURIS_TESTMODE_OFF) return;
  if (e->kind==AURIS_TOUCH_CANCEL) { m->pressed=B_NONE; return; }
  const int b=button_at(m,e->x,e->y);
  if (e->kind==AURIS_TOUCH_DOWN) { m->pressed=b; return; }
  if (e->kind==AURIS_TOUCH_MOVE) { if (b!=m->pressed) m->pressed=B_NONE; return; }
  if (e->kind!=AURIS_TOUCH_UP) return;
  const int selected=m->pressed;
  m->pressed=B_NONE;
  if (selected==B_NONE || b!=selected) return;
  if (b==B_EXIT) { m->pending=AURIS_TESTMODE_ACT_EXIT; return; }
  if (b>=B_LOCATION && b<B_ROW) { m->location=(size_t)(b-B_LOCATION); return; }
  if (b>=B_ROW && b<B_START) {
    m->test=(size_t)(b-B_ROW);
    size_t done,total;
    auris_testmode_count(m,m->location,m->test,&done,&total);
    if (total==0U) {
      (void)snprintf(m->message,sizeof(m->message),"NOT PLANNED FOR THIS LOCATION");
      return;
    }
    if (done==total) {
      (void)snprintf(m->message,sizeof(m->message),"THIS TEST IS FINISHED HERE");
      return;
    }
    m->message[0]='\0';
    (void)next_clip(m,m->plan->clip_count-1U);
    m->phase=AURIS_TESTMODE_READY;
    return;
  }
  switch (b) {
    case B_START: start_countdown(m,AURIS_TESTMODE_COUNTDOWN_S,now); break;
    case B_LATER: start_countdown(m,AURIS_TESTMODE_LONG_COUNTDOWN_S,now); break;
    case B_SKIP: (void)next_clip(m,m->clip); m->message[0]='\0'; break;
    case B_BACK: m->phase=AURIS_TESTMODE_CHOOSE; m->message[0]='\0'; break;
    case B_CANCEL:
      /* Before the recording starts there is nothing to stop. */
      if (m->phase!=AURIS_TESTMODE_COUNTDOWN) m->pending=AURIS_TESTMODE_ACT_CANCEL;
      m->phase=AURIS_TESTMODE_READY;
      break;
    case B_KEEP: m->pending=AURIS_TESTMODE_ACT_KEEP; break;
    case B_REDO: m->pending=AURIS_TESTMODE_ACT_REDO; m->phase=AURIS_TESTMODE_READY; break;
    default: break;
  }
}
auris_testmode_action_t auris_testmode_take(auris_testmode_t *m,uint64_t now) {
  if (m->pending!=AURIS_TESTMODE_ACT_NONE) {
    const auris_testmode_action_t action=m->pending;
    m->pending=AURIS_TESTMODE_ACT_NONE;
    if (action==AURIS_TESTMODE_ACT_EXIT) m->phase=AURIS_TESTMODE_OFF;
    return action;
  }
  if (now<m->phase_end) return AURIS_TESTMODE_ACT_NONE;
  switch (m->phase) {
    case AURIS_TESTMODE_COUNTDOWN:
      enter(m,AURIS_TESTMODE_QUIET,AURIS_TESTMODE_QUIET_S,now);
      return AURIS_TESTMODE_ACT_BEGIN;
    case AURIS_TESTMODE_QUIET:
      enter(m,AURIS_TESTMODE_SOURCE,auris_testmode_clip(m)->seconds,now);
      return AURIS_TESTMODE_ACT_SOURCE;
    case AURIS_TESTMODE_SOURCE:
      m->phase=AURIS_TESTMODE_SAVING;
      return AURIS_TESTMODE_ACT_END;
    default: return AURIS_TESTMODE_ACT_NONE;
  }
}
void auris_testmode_saved(auris_testmode_t *m,const auris_testmode_review_t *review) {
  m->review=*review;
  m->phase=AURIS_TESTMODE_REVIEW;
}
void auris_testmode_kept(auris_testmode_t *m,unsigned int free_mb) {
  m->done[m->location][m->clip]=true;
  m->free_mb=free_mb;
  m->phase=next_clip(m,m->clip) ? AURIS_TESTMODE_READY : AURIS_TESTMODE_CHOOSE;
}
void auris_testmode_fail(auris_testmode_t *m,const char *message) {
  (void)snprintf(m->message,sizeof(m->message),"%s",message);
  m->phase=AURIS_TESTMODE_READY;
}

#define INK 0xffffffU
#define DIM 0x9fb5c5U
#define PANEL 0x08141fU
#define ON_COLOUR 0x176459U
#define IDLE_COLOUR 0x263d50U
#define PRESSED_COLOUR 0x267f9dU
#define LIVE_COLOUR 0x5dcaa5U
#define REC_COLOUR 0xf09595U

static void fill(auris_rgb24_t *f,rect_t r,uint32_t rgb,unsigned int alpha) {
  auris_ui_fill(f,r.x,r.y,r.w,r.h,rgb,alpha);
}
static void centred(auris_rgb24_t *f,rect_t r,const char *label,unsigned int scale,uint32_t rgb) {
  const int width=(int)strlen(label)*6*(int)scale-(int)scale;
  auris_ui_text(f,r.x+(r.w-width)/2,r.y+(r.h-7*(int)scale)/2,label,scale,rgb);
}
static void button(auris_rgb24_t *f,const auris_testmode_t *m,rect_t r,int id,const char *label,
                   uint32_t colour) {
  fill(f,r,m->pressed==id ? PRESSED_COLOUR : colour,255U);
  centred(f,r,label,1U,INK);
}
static void upper(char *out,size_t capacity,const char *in) {
  size_t i=0U;
  for (;in[i] && i+1U<capacity;++i) {
    const char c=in[i];
    out[i]=c=='_' ? ' ' : (char)toupper((unsigned char)c);
  }
  out[i]='\0';
}
/* A label above its value, in a side bar. Returns the next free line. */
static int item(auris_rgb24_t *f,int x,int y,const char *label,const char *value) {
  auris_ui_text(f,x,y,label,1U,DIM);
  auris_ui_text(f,x,y+10,value,1U,INK);
  return y+26;
}
static void rails(auris_rgb24_t *f) {
  fill(f,(rect_t){0,0,80,360},0x000000U,255U);
  fill(f,(rect_t){560,0,80,360},0x000000U,255U);
}
static void clip_rail(auris_rgb24_t *f,const auris_testmode_t *m) {
  const auris_testplan_clip_t *c=auris_testmode_clip(m);
  size_t done,total;
  auris_testmode_count(m,m->location,m->test,&done,&total);
  char text[32];
  (void)snprintf(text,sizeof(text),"%u",c->test);
  int y=item(f,4,6,"TEST",text);
  (void)snprintf(text,sizeof(text),"%zu OF %zu",done+1U,total);
  y=item(f,4,y,"CLIP",text);
  y=item(f,4,y,"DISTANCE",c->distance);
  y=item(f,4,y,"POSITION",c->position);
  y=item(f,4,y,"SOURCE",c->source);
  y=item(f,4,y,"DEVICE",c->device);
  (void)snprintf(text,sizeof(text),"%.0f S",(double)c->seconds);
  y=item(f,4,y,"PLAYS FOR",text);
  (void)item(f,4,y,"PICTURES",c->pictures ? "YES":"NO");
}
static void target(auris_rgb24_t *f,const auris_testmode_t *m) {
  const auris_testplan_clip_t *c=auris_testmode_clip(m);
  if (!c->has_target) return;
  const auris_testmode_layout_t *l=&m->layout;
  const float deg=AURIS_PI/180.0f;
  const float u=l->cx+l->fx*tanf(c->right_deg*deg);
  const float v=l->cy-l->fy*tanf(c->up_deg*deg);
  const float x=(float)l->x+(u+0.5f)*(float)l->w/l->sensor_w;
  const float y=(float)l->y+(v+0.5f)*(float)l->h/l->sensor_h;
  for (int i=0;i<32;i+=2) {
    const float a=(float)i*2.0f*AURIS_PI/32.0f;
    const int dot_x=(int)lroundf(x+26.0f*cosf(a)),dot_y=(int)lroundf(y+26.0f*sinf(a));
    /* Near the picture's edge the ring is cut off there, not drawn over the bars. */
    if (dot_x>l->x+1 && dot_x<l->x+l->w-2) fill(f,(rect_t){dot_x-1,dot_y-1,3,3},INK,255U);
  }
  fill(f,(rect_t){(int)lroundf(x)-1,(int)lroundf(y)-1,3,3},INK,255U);
}
static void banner_at(auris_rgb24_t *f,int y,const char *line,unsigned int scale,uint32_t colour) {
  const rect_t r={84,y,472,scale>1U ? 34 : 22};
  fill(f,r,PANEL,215U);
  centred(f,r,line,scale,colour);
}
static void banner(auris_rgb24_t *f,const char *line,unsigned int scale,uint32_t colour) {
  banner_at(f,6,line,scale,colour);
}
static void draw_choose(auris_rgb24_t *f,const auris_testmode_t *m) {
  fill(f,(rect_t){0,0,640,360},PANEL,255U);
  char text[80];
  auris_ui_text(f,10,10,"TEST CAPTURE",2U,INK);
  (void)snprintf(text,sizeof(text),"%.1f GB FREE",(double)m->free_mb/1024.0);
  auris_ui_text(f,380,16,text,1U,m->free_mb<AURIS_TESTMODE_MIN_FREE_MB ? REC_COLOUR:DIM);
  button(f,m,exit_button,B_EXIT,"EXIT",IDLE_COLOUR);
  for (size_t i=0U;i<m->plan->location_count;++i) {
    char name[24];
    upper(name,sizeof(name),m->plan->locations[i]);
    button(f,m,location_buttons[i],B_LOCATION+(int)i,name,i==m->location ? ON_COLOUR:IDLE_COLOUR);
  }
  for (size_t i=0U;i<m->plan->test_count;++i) {
    const rect_t r=row_rect(i);
    size_t done,total;
    auris_testmode_count(m,m->location,i,&done,&total);
    fill(f,r,m->pressed==B_ROW+(int)i ? PRESSED_COLOUR:IDLE_COLOUR,255U);
    (void)snprintf(text,sizeof(text),"TEST %u: %s",m->plan->tests[i].number,m->plan->tests[i].title);
    auris_ui_text(f,r.x+10,r.y+10,text,1U,INK);
    if (total) (void)snprintf(text,sizeof(text),"%zu OF %zu",done,total);
    else (void)snprintf(text,sizeof(text),"NOT HERE");
    auris_ui_text(f,r.x+r.w-10-6*(int)strlen(text),r.y+10,text,1U,total ? INK:DIM);
  }
  if (m->message[0]) auris_ui_text(f,10,346,m->message,1U,REC_COLOUR);
}
static void draw_ready(auris_rgb24_t *f,const auris_testmode_t *m) {
  rails(f);
  clip_rail(f,m);
  target(f,m);
  /* At the bottom: the ring is often near the top of the picture, and no
     ring in this plan goes below 20 degrees down. */
  if (m->message[0]) banner_at(f,332,m->message,1U,REC_COLOUR);
  else banner_at(f,332,auris_testmode_clip(m)->has_target ?
                 "PUT THE SOURCE IN THE RING AND PRESS START" :
                 "SET UP THE SOURCE AND PRESS START",1U,INK);
  button(f,m,start_button,B_START,"START",ON_COLOUR);
  button(f,m,later_button,B_LATER,"IN 30 S",IDLE_COLOUR);
  button(f,m,skip_button,B_SKIP,"SKIP",IDLE_COLOUR);
  button(f,m,back_button,B_BACK,"BACK",IDLE_COLOUR);
}
static void draw_capture(auris_rgb24_t *f,const auris_testmode_t *m,const auris_testmode_live_t *live,
                         uint64_t now) {
  rails(f);
  clip_rail(f,m);
  const uint64_t left_us=m->phase_end>now ? m->phase_end-now : 0U;
  const uint64_t span=m->phase_end>m->phase_start ? m->phase_end-m->phase_start : 1U;
  char text[32];
  const bool counting=m->phase==AURIS_TESTMODE_COUNTDOWN;
  const char *phase=counting ? "GET CLEAR" : m->phase==AURIS_TESTMODE_QUIET ? "STAY QUIET":"SOURCE ON";
  const uint32_t colour=m->phase==AURIS_TESTMODE_SOURCE ? LIVE_COLOUR:INK;
  if (!counting) auris_ui_text(f,RAIL_RIGHT,8,"RECORDING",1U,REC_COLOUR);
  auris_ui_text(f,RAIL_RIGHT,30,phase,1U,colour);
  (void)snprintf(text,sizeof(text),"%.0f",ceil((double)left_us/1e6));
  auris_ui_text(f,RAIL_RIGHT,46,text,4U,colour);
  const rect_t bar={RAIL_RIGHT,86,RAIL_WIDTH,6};
  fill(f,bar,0x3a3936U,255U);
  fill(f,(rect_t){bar.x,bar.y,(int)((uint64_t)bar.w*(span-(left_us>span?span:left_us))/span),bar.h},colour,255U);
  (void)snprintf(text,sizeof(text),"%.0f DBFS",(double)live->dbfs);
  int y=item(f,RAIL_RIGHT,108,"LEVEL",text);
  (void)item(f,RAIL_RIGHT,y,"SHOWN",live->shown ? "YES":"NO");
  button(f,m,cancel_button,B_CANCEL,"CANCEL",IDLE_COLOUR);
  /* Only the first seconds, and large: this may be read from across a room.
     The saved pictures are taken before this is drawn. */
  if (m->phase==AURIS_TESTMODE_SOURCE && now-m->phase_start<3000000U)
    banner(f,"START THE SOURCE",3U,LIVE_COLOUR);
  else if (m->phase!=AURIS_TESTMODE_SOURCE) banner(f,phase,3U,INK);
}
static void draw_review(auris_rgb24_t *f,const auris_testmode_t *m) {
  rails(f);
  char text[32];
  if (m->phase==AURIS_TESTMODE_SAVING) {
    banner(f,"SAVING",3U,INK);
    return;
  }
  const auris_testmode_review_t *r=&m->review;
  if (r->failed) {
    banner(f,"THE CLIP COULD NOT BE SAVED",1U,REC_COLOUR);
    button(f,m,redo_button,B_REDO,"BACK",IDLE_COLOUR);
    return;
  }
  auris_ui_text(f,4,6,"CLIP SAVED",1U,LIVE_COLOUR);
  (void)snprintf(text,sizeof(text),"%.0f %%",(double)r->shown_pct);
  int y=item(f,4,24,"SHOWN",text);
  (void)snprintf(text,sizeof(text),"%.0f DBFS",(double)r->source_dbfs);
  y=item(f,4,y,"SOURCE",text);
  (void)snprintf(text,sizeof(text),"%.0f DBFS",(double)r->quiet_dbfs);
  y=item(f,4,y,"QUIET",text);
  (void)snprintf(text,sizeof(text),"%llu",(unsigned long long)r->lost_frames);
  y=item(f,4,y,"AUDIO LOST",text);
  (void)snprintf(text,sizeof(text),"%.1f FPS",(double)r->video_fps);
  y=item(f,4,y,"VIDEO",text);
  (void)snprintf(text,sizeof(text),"%llu",(unsigned long long)r->replaced);
  (void)item(f,4,y,"NOT SHOWN",text);
  banner(f,"THE PICTURE AS THE CLIP ENDED",1U,DIM);
  button(f,m,keep_button,B_KEEP,"KEEP",ON_COLOUR);
  button(f,m,redo_button,B_REDO,"REDO",IDLE_COLOUR);
}
int auris_testmode_draw(auris_testmode_t *m,auris_rgb24_t *f,const auris_testmode_live_t *live,uint64_t now) {
  if (!m || !f || !live || !f->data || f->width!=640U || f->height!=360U ||
      f->stride<1920U || f->bytes<1920U || (f->bytes-1920U)/f->stride<359U) return -EINVAL;
  switch (m->phase) {
    case AURIS_TESTMODE_CHOOSE: draw_choose(f,m); break;
    case AURIS_TESTMODE_READY: draw_ready(f,m); break;
    case AURIS_TESTMODE_COUNTDOWN: case AURIS_TESTMODE_QUIET: case AURIS_TESTMODE_SOURCE:
      draw_capture(f,m,live,now); break;
    case AURIS_TESTMODE_SAVING: case AURIS_TESTMODE_REVIEW: draw_review(f,m); break;
    default: break;
  }
  return 0;
}
