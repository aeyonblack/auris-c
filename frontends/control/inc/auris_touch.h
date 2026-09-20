#ifndef AURIS_TOUCH_H
#define AURIS_TOUCH_H
#include <stdbool.h>
#include <stdint.h>
#include <linux/input.h>
typedef enum { AURIS_TOUCH_DOWN, AURIS_TOUCH_MOVE, AURIS_TOUCH_UP,
               AURIS_TOUCH_CANCEL } auris_touch_kind_t;
typedef struct { auris_touch_kind_t kind; int x,y; } auris_touch_event_t;
typedef void (*auris_touch_callback_t)(void *,const auris_touch_event_t *);
/* Single-finger gestures only. Multi-touch cancels until every finger lifts. */
typedef struct {
  int slot, count, id[32], x[32], y[32], last_x,last_y,active_id;
  bool down, blocked, dropped;
} auris_touch_decoder_t;
void auris_touch_decoder_init(auris_touch_decoder_t *decoder,int slots);
bool auris_touch_decode(auris_touch_decoder_t *decoder,const struct input_event *input,
                        auris_touch_event_t *event);
/* Inverse of display fit+clockwise rotation. Letterbox touches return false. */
bool auris_touch_project(int x,int y,int xmax,int ymax,
                         unsigned int screen_w,unsigned int screen_h,
                         unsigned int image_w,unsigned int image_h,
                         unsigned int rotation,int *image_x,int *image_y);
typedef struct auris_touch auris_touch_t;
int auris_touch_open(auris_touch_t **out,const char *path);
/* Bounded, nonblocking; callback coordinates are normalized to 0..65535. */
int auris_touch_poll(auris_touch_t *touch,auris_touch_callback_t callback,void *context);
void auris_touch_close(auris_touch_t *touch);
#endif
