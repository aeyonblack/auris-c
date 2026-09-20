#ifdef AURIS_OVERLAY_AVAILABLE
#include "auris_overlay.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static auris_overlay_config_t config(void) {
  const auris_view_t view = {.width = 5U, .height = 5U, .fx = 4, .fy = 4,
      .cx = 2, .cy = 2, .rotation = {{1,0,0}, {0,1,0}, {0,0,1}}};
  return (auris_overlay_config_t){.camera = view, .search = view,
      .layout = {7U, 5U, 1U, 0U, 5U, 5U}, .columns = 3U, .rows = 3U,
      .cutoff = 0.5f, .opacity = 1.0f, .mark_peak = false};
}

int main(void) {
  uint8_t bytes[130], expected[130];
  auris_rgb24_t frame = {bytes, 7U, 5U, 26U, sizeof(bytes)};
  const float scores[9] = {0,5,10,0,5,10,0,5,10};
  auris_overlay_config_t c = config();
  auris_overlay_t *s = NULL;
  assert(auris_overlay_open(&s, &c) == 0);
  memset(bytes, 40, sizeof(bytes));
  assert(auris_overlay_apply(s, scores, 9U, 2U, true, &frame) == 0);
  /* Analytic x ramp: transparent, transparent, blue rim, green, red. */
  const uint8_t wanted[5][3] = {{40,40,40},{40,40,40},{37,50,87},
                               {44,172,77},{255,29,22}};
  for (size_t y = 0; y < 5U; ++y) {
    for (size_t x = 0; x < 5U; ++x)
      assert(memcmp(bytes + y * 26U + (x + 1U) * 3U, wanted[x], 3U) == 0);
    for (size_t ch = 0; ch < 3U; ++ch) {
      assert(bytes[y * 26U + ch] == 40U);
      assert(bytes[y * 26U + 18U + ch] == 40U);
    }
    for (size_t p = 21U; p < 26U; ++p) assert(bytes[y * 26U + p] == 40U);
  }
  memcpy(expected, bytes, sizeof(bytes));
  assert(auris_overlay_apply(s, NULL, 0U, 0U, false, &frame) == 0);
  assert(memcmp(bytes, expected, sizeof(bytes)) == 0);
  float flat[9] = {0};
  assert(auris_overlay_apply(s, flat, 9U, 0U, true, &frame) == 0);
  assert(memcmp(bytes, expected, sizeof(bytes)) == 0);
  flat[8] = NAN;
  assert(auris_overlay_apply(s, flat, 9U, 0U, true, &frame) == -EINVAL);
  assert(memcmp(bytes, expected, sizeof(bytes)) == 0);
  frame.bytes = 124U;
  assert(auris_overlay_apply(s, scores, 9U, 2U, true, &frame) == -EINVAL);
  frame.bytes = sizeof(bytes);
  frame.stride = SIZE_MAX;
  assert(auris_overlay_apply(s, scores, 9U, 2U, true, &frame) == -EINVAL);
  frame.stride = 26U;
  assert(auris_overlay_set_style(s,NAN,0.5f)==-EINVAL);
  assert(auris_overlay_set_style(s,0.5f,1.1f)==-EINVAL);
  assert(auris_overlay_set_style(s,0.5f,0.0f)==0);
  memset(bytes,40,sizeof(bytes));
  assert(auris_overlay_apply(s,scores,9U,2U,true,&frame)==0);
  for (size_t i=0U;i<sizeof(bytes);++i) assert(bytes[i]==40U);
  assert(auris_overlay_set_style(s,0.5f,1.0f)==0);
  assert(auris_overlay_apply(s,scores,9U,2U,true,&frame)==0);
  assert(memcmp(bytes,expected,sizeof(bytes))==0);
  auris_overlay_close(s); s = NULL;

  /* A quarter-turn camera/search basis change turns a y ramp into x. */
  const float rotated[9] = {0,0,0,5,5,5,10,10,10};
  c.search.rotation[0][0] = 0; c.search.rotation[0][1] = -1;
  c.search.rotation[1][0] = 1; c.search.rotation[1][1] = 0;
  assert(auris_overlay_open(&s, &c) == 0);
  memset(bytes, 40, sizeof(bytes));
  assert(auris_overlay_apply(s, rotated, 9U, 6U, true, &frame) == 0);
  assert(memcmp(bytes, expected, sizeof(bytes)) == 0);
  auris_overlay_close(s); s = NULL;

  /* Search coverage smaller than the camera: never stretch into its margins. */
  c = config(); c.search.fx = 8.0f;
  assert(auris_overlay_open(&s, &c) == 0);
  memset(bytes, 40, sizeof(bytes));
  assert(auris_overlay_apply(s, scores, 9U, 2U, true, &frame) == 0);
  for (size_t y = 0; y < 5U; ++y)
    for (size_t ch = 0; ch < 3U; ++ch) {
      assert(bytes[y * 26U + 3U + ch] == 40U);
      assert(bytes[y * 26U + 15U + ch] == 40U);
    }
  auris_overlay_close(s); s = NULL;

  /* One pixel and marker clipping; also exercises upsampling edge clamps. */
  c = config(); c.layout = (auris_projection_layout_t){1,1,0,0,1,1};
  c.mark_peak = true;
  assert(auris_overlay_open(&s, &c) == 0);
  uint8_t tiny[3] = {0};
  auris_rgb24_t one = {tiny,1,1,3,3};
  assert(auris_overlay_apply(s, scores, 9, 4, true, &one) == 0);
  assert(tiny[0] == 0 && tiny[1] == 255 && tiny[2] == 255);
  auris_overlay_close(s); s = NULL;
  c = config(); c.layout = (auris_projection_layout_t){7,5,0,0,7,5};
  assert(auris_overlay_open(&s, &c) == 0);
  auris_overlay_close(s); s = NULL;
  c.cutoff = 1.0f;
  assert(auris_overlay_open(&s, &c) == -EINVAL && s == NULL);
  c = config(); c.layout.content_x = SIZE_MAX;
  assert(auris_overlay_open(&s, &c) == -EINVAL && s == NULL);
  puts("overlay checks passed");
  return 0;
}
#else
int main(void) { return 77; }
#endif
