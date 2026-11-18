#ifndef AURIS_UI_H
#define AURIS_UI_H
#include "auris_image.h"
#include "auris_settings.h"
#include "auris_touch.h"
typedef struct {
  auris_settings_t settings;
  bool visible,save_requested,test_requested;
  int pressed;
  uint64_t last_touch,confirm_until;
  unsigned int revision,saved_revision;
} auris_ui_t;
typedef struct {
  float dbfs,contrast,fps;
  bool ready,active,evidence;
  uint64_t dropped;
  int save_status;
} auris_ui_status_t;
void auris_ui_init(auris_ui_t *ui,const auris_settings_t *settings,uint64_t now_us);
/* Coordinates are in the unrotated RGB canvas, not physical touch axes. */
void auris_ui_touch(auris_ui_t *ui,const auris_touch_event_t *event,uint64_t now_us);
/* Logical 640x360 UI; rejects incompatible or undersized buffers. */
int auris_ui_draw(auris_ui_t *ui,auris_rgb24_t *frame,
                   const auris_ui_status_t *status,uint64_t now_us);
/* The panel's drawing tools. Clipped to the frame. */
void auris_ui_fill(auris_rgb24_t *frame,int x,int y,int w,int h,uint32_t rgb,unsigned int alpha);
void auris_ui_text(auris_rgb24_t *frame,int x,int y,const char *text,unsigned int scale,uint32_t rgb);
#endif
