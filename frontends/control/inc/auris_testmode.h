#ifndef AURIS_TESTMODE_H
#define AURIS_TESTMODE_H
#include "auris_image.h"
#include "auris_testplan.h"
#include "auris_touch.h"
#include <stdint.h>
/* The screens that capture one qualification clip at a time: choose a test,
   set up the clip, record it without touching anything, review it. This file
   decides what happens and draws it; the imager does the recording when
   auris_testmode_take asks for it. */
#define AURIS_TESTMODE_COUNTDOWN_S 5.0f
#define AURIS_TESTMODE_LONG_COUNTDOWN_S 30.0f
#define AURIS_TESTMODE_QUIET_S 5.0f
#define AURIS_TESTMODE_MIN_FREE_MB 200U
typedef enum {
  AURIS_TESTMODE_OFF, AURIS_TESTMODE_CHOOSE, AURIS_TESTMODE_READY,
  AURIS_TESTMODE_COUNTDOWN, AURIS_TESTMODE_QUIET, AURIS_TESTMODE_SOURCE,
  AURIS_TESTMODE_SAVING, AURIS_TESTMODE_REVIEW
} auris_testmode_phase_t;
typedef enum {
  AURIS_TESTMODE_ACT_NONE,
  AURIS_TESTMODE_ACT_BEGIN,  /* start recording the selected clip */
  AURIS_TESTMODE_ACT_SOURCE, /* the source starts now */
  AURIS_TESTMODE_ACT_END,    /* stop recording and save */
  AURIS_TESTMODE_ACT_CANCEL, /* stop recording and save nothing */
  AURIS_TESTMODE_ACT_KEEP,   /* the saved clip counts */
  AURIS_TESTMODE_ACT_REDO,   /* set the saved clip aside */
  AURIS_TESTMODE_ACT_EXIT
} auris_testmode_action_t;
typedef struct {
  float shown_pct, source_dbfs, quiet_dbfs, video_fps;
  uint64_t lost_frames, replaced;
  bool failed;
} auris_testmode_review_t;
typedef struct { float dbfs; bool shown; } auris_testmode_live_t;
/* Where a direction lands on the canvas: the camera's model and picture. */
typedef struct {
  float fx,fy,cx,cy,sensor_w,sensor_h;
  int x,y,w,h;
} auris_testmode_layout_t;
typedef struct {
  const auris_testplan_t *plan;
  auris_testmode_layout_t layout;
  auris_testmode_phase_t phase;
  size_t location,test,clip; /* selected: indices into the plan */
  bool done[AURIS_TESTPLAN_MAX_LOCATIONS][AURIS_TESTPLAN_MAX_CLIPS];
  int pressed;
  uint64_t phase_end,phase_start;
  auris_testmode_action_t pending;
  auris_testmode_review_t review;
  unsigned int free_mb;
  char message[48];
} auris_testmode_t;
void auris_testmode_init(auris_testmode_t *mode,const auris_testplan_t *plan,
                         const auris_testmode_layout_t *layout);
void auris_testmode_open(auris_testmode_t *mode,unsigned int free_mb);
/* Coordinates are in the 640x360 canvas, as for auris_ui_touch. */
void auris_testmode_touch(auris_testmode_t *mode,const auris_touch_event_t *event,uint64_t now_us);
/* Advances the timers. Returns one thing to do now; call until NONE. */
auris_testmode_action_t auris_testmode_take(auris_testmode_t *mode,uint64_t now_us);
/* The save finished; the screen moves to the review. */
void auris_testmode_saved(auris_testmode_t *mode,const auris_testmode_review_t *review);
/* After KEEP was done: marks the clip and selects the next one to record. */
void auris_testmode_kept(auris_testmode_t *mode,unsigned int free_mb);
/* A step failed: back to the clip's setup screen, with the reason shown. */
void auris_testmode_fail(auris_testmode_t *mode,const char *message);
const auris_testplan_clip_t *auris_testmode_clip(const auris_testmode_t *mode);
/* Clips of this location and test, and how many of them are done. */
void auris_testmode_count(const auris_testmode_t *mode,size_t location,size_t test,
                          size_t *done,size_t *total);
bool auris_testmode_recording(const auris_testmode_t *mode);
int auris_testmode_draw(auris_testmode_t *mode,auris_rgb24_t *frame,
                        const auris_testmode_live_t *live,uint64_t now_us);
#endif
