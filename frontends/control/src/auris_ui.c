#include "auris_ui.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { int x,y,w,h; } rect_t;
/* Same rectangles drive drawing and hit testing. No invisible hit targets. */
static const rect_t buttons[]={
  {20,64,192,44},{224,64,192,44},{428,64,192,44},
  {20,122,143,44},{172,122,143,44},{324,122,143,44},{476,122,143,44},
  {20,180,192,44},{224,180,192,44},{428,180,192,44},
  {20,238,192,44},{224,238,192,44},{428,238,192,44}
};
static int hit(int x,int y) {
  for (size_t i=0U;i<sizeof(buttons)/sizeof(buttons[0]);++i) {
    rect_t r=buttons[i];
    if (x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h) return (int)i;
  }
  return -1;
}
void auris_ui_init(auris_ui_t *s,const auris_settings_t *settings,uint64_t now) {
  *s=(auris_ui_t){.settings=*settings,.visible=true,.pressed=-1,.last_touch=now};
}
void auris_ui_touch(auris_ui_t *s,const auris_touch_event_t *e,uint64_t now) {
  s->last_touch=now;
  if (e->kind==AURIS_TOUCH_CANCEL) { s->pressed=-1; return; }
  if (!s->visible) {
    if (e->kind==AURIS_TOUCH_DOWN) { s->visible=true; s->pressed=-1; }
    return; /* Wake-only gesture cannot activate a newly shown button. */
  }
  int b=hit(e->x,e->y);
  if (e->kind==AURIS_TOUCH_DOWN) { s->pressed=b; return; }
  if (e->kind==AURIS_TOUCH_MOVE) { if (b!=s->pressed) s->pressed=-1; return; }
  if (e->kind!=AURIS_TOUCH_UP) return;
  int selected=s->pressed; s->pressed=-1;
  if (selected<0 || b!=selected) return;
  auris_settings_t *c=&s->settings;
  if (b!=11) s->confirm_until=0U;
  switch (b) {
    case 0: c->overlay=!c->overlay; break;
    case 1: c->gating=!c->gating; break;
    case 2: c->smoothing=!c->smoothing; break;
    case 3: c->cutoff=fmaxf(0.0f,c->cutoff-0.05f); break;
    case 4: c->cutoff=fminf(0.95f,c->cutoff+0.05f); break;
    case 5: c->opacity=fmaxf(0.0f,c->opacity-0.05f); break;
    case 6: c->opacity=fminf(1.0f,c->opacity+0.05f); break;
    case 7: c->fps=15U; break;
    case 8: c->fps=20U; break;
    case 9: c->fps=30U; break;
    case 10: s->save_requested=true; return;
    case 11:
      if (!s->confirm_until || now>s->confirm_until) { s->confirm_until=now+3000000U; return; }
      *c=auris_settings_defaults(); s->confirm_until=0U; break;
    case 12: s->visible=false; return;
    default: return;
  }
  ++s->revision;
}
/* Compact original 5x7 bitmap alphabet, column-major, least-significant bit at top. */
static const char glyphs[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-+:/%?";
static const uint8_t font[][5]={
 {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {3,4,120,4,3},{97,81,73,69,67},
 {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
 {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
 {54,73,73,73,54},{6,73,73,41,30},
 {0,96,96,0,0},{8,8,8,8,8},{8,8,62,8,8},{0,54,54,0,0},
 {32,16,8,4,2},{99,19,8,100,99},{2,1,81,9,6}
};
static void box(auris_rgb24_t *f,rect_t r,uint32_t rgb,unsigned int alpha) {
  for (int y=r.y;y<r.y+r.h;++y) for (int x=r.x;x<r.x+r.w;++x) {
    if (x<0 || y<0 || x>=(int)f->width || y>=(int)f->height) continue;
    uint8_t *p=f->data+(size_t)y*f->stride+(size_t)x*3U;
    for (unsigned int c=0U;c<3U;++c)
      p[c]=(uint8_t)((alpha*((rgb>>(16U-8U*c))&255U)+(255U-alpha)*p[c]+127U)/255U);
  }
}
static void text(auris_rgb24_t *f,int x,int y,const char *str,unsigned int scale,uint32_t colour) {
  for (;*str;++str,x+=6*(int)scale) {
    const char *g=strchr(glyphs,*str);
    if (!g) continue;
    size_t index=(size_t)(g-glyphs);
    for (int col=0;col<5;++col) for (int row=0;row<7;++row)
      if (font[index][col]&(1U<<(unsigned int)row))
        box(f,(rect_t){x+col*(int)scale,y+row*(int)scale,(int)scale,(int)scale},colour,255U);
  }
}
int auris_ui_draw(auris_ui_t *s,auris_rgb24_t *f,const auris_ui_status_t *status,uint64_t now) {
  if (!s || !f || !status || !f->data || f->width!=640U || f->height!=360U ||
      f->stride<1920U || f->bytes<1920U || (f->bytes-1920U)/f->stride<359U) return -EINVAL;
  if (now-s->last_touch>10000000U && s->pressed<0) { s->visible=false; s->confirm_until=0U; }
  char label[100];
  box(f,(rect_t){0,0,640,44},0x08141fU,205U);
  const char *gate=!s->settings.gating ? "BYPASS" : !status->ready ? "WARMUP" :
      !status->active ? "CLOSED" : status->evidence ? "ACTIVE":"HOLD";
  (void)snprintf(label,sizeof(label),"AUDIO %.1f DBFS  %s  CONTRAST %.2f",
      (double)status->dbfs,gate,(double)status->contrast);
  text(f,10,5,label,1U,0xd9eaf1U);
  (void)snprintf(label,sizeof(label),"FPS %.1f/%u  REPLACED %llu",
      (double)status->fps,s->settings.fps,(unsigned long long)status->dropped);
  text(f,10,25,label,1U,0x6fdbd1U);
  text(f,490,17,s->visible ? "CONTROLS":"TAP TO OPEN",1U,0xffffffU);
  if (!s->visible) return 0;
  box(f,(rect_t){12,50,616,290},0x08141fU,235U);
  for (size_t i=0U;i<sizeof(buttons)/sizeof(buttons[0]);++i) {
    bool on=(i==0U && s->settings.overlay) || (i==1U && s->settings.gating) ||
      (i==2U && s->settings.smoothing) || (i==7U && s->settings.fps==15U) ||
      (i==8U && s->settings.fps==20U) || (i==9U && s->settings.fps==30U);
    switch (i) {
      case 0U: (void)snprintf(label,sizeof(label),"OVERLAY %s",on?"ON":"OFF"); break;
      case 1U: (void)snprintf(label,sizeof(label),"GATE %s",on?"ON":"OFF"); break;
      case 2U: (void)snprintf(label,sizeof(label),"SMOOTH %s",on?"ON":"OFF"); break;
      case 3U: strcpy(label,"CUTOFF -"); break;
      case 4U: strcpy(label,"CUTOFF +"); break;
      case 5U: strcpy(label,"OPACITY -"); break;
      case 6U: strcpy(label,"OPACITY +"); break;
      case 7U: strcpy(label,"15 FPS"); break;
      case 8U: strcpy(label,"20 FPS"); break;
      case 9U: strcpy(label,"30 FPS"); break;
      case 10U: strcpy(label,status->save_status==1 ? "SAVING":"SAVE"); break;
      case 11U: strcpy(label,s->confirm_until>now ? "CONFIRM RESET":"DEFAULTS"); break;
      default: strcpy(label,"HIDE"); break;
    }
    rect_t r=buttons[i];
    box(f,r,s->pressed==(int)i ? 0x267f9dU : on ? 0x176459U : 0x263d50U,255U);
    int width=(int)strlen(label)*12-2;
    text(f,r.x+(r.w-width)/2,r.y+15,label,2U,0xffffffU);
  }
  (void)snprintf(label,sizeof(label),"CUTOFF %.2f   OPACITY %.2f   %s",
      (double)s->settings.cutoff,(double)s->settings.opacity,
      status->save_status<0 ? "SAVE FAILED" : status->save_status==1 ? "SAVING" :
      s->revision!=s->saved_revision ? "UNSAVED":"NO UNSAVED CHANGES");
  text(f,26,300,label,1U,0xe3eef2U);
  text(f,26,320,"DEFAULTS NEEDS TWO TAPS. SAVE TO KEEP CHANGES. AUTO-HIDE 10S.",1U,0x9fb5c5U);
  return 0;
}
