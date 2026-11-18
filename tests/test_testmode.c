#include "auris_testmode.h"
#include "auris_ui.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char plan_text[]=
  "# comment\n\n"
  "location flat\nlocation hall_way\n"
  "test 1 VIEW_AND_RANGE\ntest 4 RATE\n"
  "clip flat 1 a 2_M CENTRE PINK STILL 0 0 11 yes\n"
  "clip flat,hall_way 1 b 2_M 30_LEFT PINK IN_HAND -30 20 11 yes\n"
  "clip hall_way 4 c 2_M - NOISE STILL - - 30 no\n";

static const auris_testmode_layout_t layout={1358.5f,1356.9f,819.5f,615.5f,1640.0f,1232.0f,80,0,479,360};
static uint64_t now=1000000U;

static void tap(auris_testmode_t *m,int x,int y) {
  auris_touch_event_t e={AURIS_TOUCH_DOWN,x,y};
  auris_testmode_touch(m,&e,now);
  e.kind=AURIS_TOUCH_UP;
  auris_testmode_touch(m,&e,now+1000U);
}
static void plan_tests(void) {
  static auris_testplan_t plan;
  size_t bad=0U;
  assert(auris_testplan_parse(plan_text,&plan,&bad)==0);
  assert(plan.location_count==2U && plan.test_count==2U && plan.clip_count==3U);
  assert(strcmp(plan.tests[0].title,"VIEW AND RANGE")==0);
  assert(strcmp(plan.locations[1],"hall_way")==0);
  const auris_testplan_clip_t *b=&plan.clips[1];
  assert(b->locations==3U && b->test==1U && strcmp(b->position,"30 LEFT")==0);
  assert(strcmp(b->device,"IN HAND")==0 && b->has_target && b->right_deg==-30.0f && b->up_deg==20.0f);
  assert(b->seconds==11.0f && b->pictures);
  assert(!plan.clips[2].has_target && !plan.clips[2].pictures);
  char dir[64];
  assert(auris_testplan_clip_dir(&plan,1U,1U,"results",dir,sizeof(dir))==0);
  assert(strcmp(dir,"results/hall_way/test1/b")==0);
  assert(auris_testplan_clip_dir(&plan,1U,1U,"results",dir,8U)==-ENAMETOOLONG);

  static const struct { const char *text; size_t line; } bad_plans[]={
    {"location a\ntest 1 T\nclip nowhere 1 x 1 2 3 4 0 0 11 yes\n",3U},
    {"location a\ntest 1 T\nclip a 2 x 1 2 3 4 0 0 11 yes\n",3U},
    {"location a\ntest 1 T\nclip a 1 ../x 1 2 3 4 0 0 11 yes\n",3U},
    {"location a\ntest 1 T\nclip a 1 x 1 2 3 4 0 0 11 maybe\n",3U},
    {"location a\ntest 1 T\nclip a 1 x 1 2 3 4 0 0 0 yes\n",3U},
    {"location a\ntest 1 T\nclip a 1 x 1 2 3 4 0 0 11 yes extra\n",3U},
    {"location a\ntest 1 T\nclip a 1 x 1 2 3 4 0 0 11 yes\nclip a 1 x 1 2 3 4 0 0 11 yes\n",4U},
    {"location ../a\n",1U},
    {"nonsense\n",1U},
    {"location a\n",2U},
  };
  for (size_t i=0U;i<sizeof(bad_plans)/sizeof(bad_plans[0]);++i) {
    auris_testplan_t kept=plan;
    bad=0U;
    assert(auris_testplan_parse(bad_plans[i].text,&plan,&bad)==-EINVAL);
    assert(bad==bad_plans[i].line);
    assert(memcmp(&kept,&plan,sizeof(plan))==0); /* all or nothing */
  }
  assert(auris_testplan_load("/nonexistent/plan",&plan,NULL)==-ENOENT);
}
static void shipped_plan_test(const char *path) {
  static auris_testplan_t plan;
  size_t bad=0U;
  const int loaded=auris_testplan_load(path,&plan,&bad);
  if (loaded) fprintf(stderr,"%s: error %d at line %zu\n",path,loaded,bad);
  assert(loaded==0);
  size_t clips=0U;
  for (size_t i=0U;i<plan.clip_count;++i)
    for (size_t l=0U;l<plan.location_count;++l)
      if (plan.clips[i].locations&(1U<<l)) ++clips;
  assert(clips==45U);
}
static void flow_tests(void) {
  static auris_testplan_t plan;
  static auris_testmode_t mode;
  assert(auris_testplan_parse(plan_text,&plan,NULL)==0);
  auris_testmode_t *m=&mode;
  auris_testmode_init(m,&plan,&layout);
  assert(m->phase==AURIS_TESTMODE_OFF);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);
  auris_testmode_open(m,4000U);
  assert(m->phase==AURIS_TESTMODE_CHOOSE);

  size_t done,total;
  auris_testmode_count(m,0U,0U,&done,&total);
  assert(done==0U && total==2U);
  auris_testmode_count(m,1U,1U,&done,&total);
  assert(done==0U && total==1U);

  /* A test with nothing planned here stays on the list and says why. */
  tap(m,300,92+32+10);
  assert(m->phase==AURIS_TESTMODE_CHOOSE && m->message[0]);
  /* First test, flat: its first clip. A second location can be chosen too. */
  tap(m,300,92+10);
  assert(m->phase==AURIS_TESTMODE_READY && strcmp(auris_testmode_clip(m)->id,"a")==0);
  assert(!m->message[0]);
  /* Skip goes round the clips still to do. */
  tap(m,600,260);
  assert(strcmp(auris_testmode_clip(m)->id,"b")==0);
  tap(m,600,260);
  assert(strcmp(auris_testmode_clip(m)->id,"a")==0);

  /* Start: nothing records until the countdown is over. */
  tap(m,600,150);
  assert(m->phase==AURIS_TESTMODE_COUNTDOWN);
  assert(!auris_testmode_recording(m));
  now+=4000000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);
  now+=1100000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_BEGIN);
  assert(m->phase==AURIS_TESTMODE_QUIET && auris_testmode_recording(m));
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);
  now+=5100000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_SOURCE);
  assert(m->phase==AURIS_TESTMODE_SOURCE);
  now+=11100000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_END);
  assert(m->phase==AURIS_TESTMODE_SAVING && !auris_testmode_recording(m));
  now+=60000000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);
  auris_testmode_saved(m,&(auris_testmode_review_t){.shown_pct=87.0f});
  assert(m->phase==AURIS_TESTMODE_REVIEW);

  /* Redo: same clip again, nothing marked done. */
  tap(m,600,300);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_REDO);
  assert(m->phase==AURIS_TESTMODE_READY && strcmp(auris_testmode_clip(m)->id,"a")==0);
  auris_testmode_count(m,0U,0U,&done,&total);
  assert(done==0U);

  /* Cancel in the countdown: nothing was recording, so nothing to stop. */
  tap(m,600,150);
  tap(m,600,300);
  assert(m->phase==AURIS_TESTMODE_READY);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);

  /* Cancel while recording. */
  tap(m,600,150);
  now+=5100000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_BEGIN);
  tap(m,600,300);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_CANCEL);
  assert(m->phase==AURIS_TESTMODE_READY);
  now+=60000000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);

  /* The long countdown. */
  tap(m,600,215);
  assert(m->phase==AURIS_TESTMODE_COUNTDOWN);
  now+=29000000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);
  now+=1100000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_BEGIN);
  now+=5100000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_SOURCE);
  now+=11100000U;
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_END);

  /* A failed save offers no Keep. */
  auris_testmode_saved(m,&(auris_testmode_review_t){.failed=true});
  tap(m,600,220);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);
  tap(m,600,300);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_REDO);
  assert(m->phase==AURIS_TESTMODE_READY);

  /* Keep: the next clip, then back to the list when none is left. */
  m->phase=AURIS_TESTMODE_REVIEW;
  m->review=(auris_testmode_review_t){0};
  tap(m,600,220);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_KEEP);
  auris_testmode_kept(m,3900U);
  assert(m->phase==AURIS_TESTMODE_READY && strcmp(auris_testmode_clip(m)->id,"b")==0);
  auris_testmode_count(m,0U,0U,&done,&total);
  assert(done==1U && total==2U);
  m->phase=AURIS_TESTMODE_REVIEW;
  tap(m,600,220);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_KEEP);
  auris_testmode_kept(m,3800U);
  assert(m->phase==AURIS_TESTMODE_CHOOSE);
  /* A finished test does not reopen, and the other location is separate. */
  tap(m,300,92+10);
  assert(m->phase==AURIS_TESTMODE_CHOOSE && m->message[0]);
  tap(m,240,60);
  assert(m->location==1U);
  tap(m,300,92+10);
  assert(m->phase==AURIS_TESTMODE_READY && strcmp(auris_testmode_clip(m)->id,"b")==0);
  tap(m,600,300);
  assert(m->phase==AURIS_TESTMODE_CHOOSE);

  /* A press that slides off a button does nothing. */
  auris_touch_event_t e={AURIS_TOUCH_DOWN,580,20};
  auris_testmode_touch(m,&e,now);
  e=(auris_touch_event_t){AURIS_TOUCH_MOVE,100,300};
  auris_testmode_touch(m,&e,now);
  e=(auris_touch_event_t){AURIS_TOUCH_UP,580,20};
  auris_testmode_touch(m,&e,now);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_NONE);

  /* Out of disk: no recording starts. */
  tap(m,300,92+10);
  m->free_mb=AURIS_TESTMODE_MIN_FREE_MB-1U;
  tap(m,600,150);
  assert(m->phase==AURIS_TESTMODE_READY && m->message[0]);
  m->free_mb=4000U;
  m->message[0]='\0';
  auris_testmode_fail(m,"NO MEMORY");
  assert(m->phase==AURIS_TESTMODE_READY && strcmp(m->message,"NO MEMORY")==0);
  tap(m,600,300);

  tap(m,580,20);
  assert(auris_testmode_take(m,now)==AURIS_TESTMODE_ACT_EXIT);
  assert(m->phase==AURIS_TESTMODE_OFF);
}
static void draw_tests(void) {
  static auris_testplan_t plan;
  static auris_testmode_t mode;
  static uint8_t pixels[640U*360U*3U];
  assert(auris_testplan_parse(plan_text,&plan,NULL)==0);
  auris_rgb24_t f={pixels,640U,360U,1920U,sizeof(pixels)};
  const auris_testmode_live_t live={-48.0f,true};
  auris_testmode_init(&mode,&plan,&layout);
  auris_testmode_open(&mode,4000U);
  static const auris_testmode_phase_t phases[]={
    AURIS_TESTMODE_CHOOSE,AURIS_TESTMODE_READY,AURIS_TESTMODE_COUNTDOWN,AURIS_TESTMODE_QUIET,
    AURIS_TESTMODE_SOURCE,AURIS_TESTMODE_SAVING,AURIS_TESTMODE_REVIEW};
  for (size_t i=0U;i<sizeof(phases)/sizeof(phases[0]);++i) {
    memset(pixels,128,sizeof(pixels));
    mode.phase=phases[i];
    mode.phase_start=now; mode.phase_end=now+5000000U;
    assert(auris_testmode_draw(&mode,&f,&live,now)==0);
    /* The picture's middle stays the picture while a clip is recording. */
    const uint8_t *middle=pixels+200U*1920U+320U*3U;
    if (phases[i]==AURIS_TESTMODE_QUIET || phases[i]==AURIS_TESTMODE_SOURCE)
      assert(middle[0]==128U && middle[1]==128U && middle[2]==128U);
  }
  auris_rgb24_t small={pixels,320U,360U,960U,sizeof(pixels)};
  assert(auris_testmode_draw(&mode,&small,&live,now)==-EINVAL);
}
static void ui_button_test(void) {
  auris_settings_t settings=auris_settings_defaults();
  auris_ui_t ui;
  auris_ui_init(&ui,&settings,now);
  assert(!ui.test_requested);
  auris_touch_event_t e={AURIS_TOUCH_DOWN,500,300};
  auris_ui_touch(&ui,&e,now);
  e.kind=AURIS_TOUCH_UP;
  auris_ui_touch(&ui,&e,now+1000U);
  assert(ui.test_requested);
}
int main(int argc,char **argv) {
  plan_tests();
  flow_tests();
  draw_tests();
  ui_button_test();
  if (argc>1) shipped_plan_test(argv[1]);
  puts("testmode OK");
  return 0;
}
