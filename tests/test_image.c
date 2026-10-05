#ifdef AURIS_IMAGE_AVAILABLE
#include "auris_image.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(EXIT_FAILURE); } } while (0)

#define BLACK 64.0f
#define SW 64U
#define SH 48U
#define W 16U
#define H 12U

static uint16_t raw[SW * SH];
static uint8_t raw8[SW * SH];
static uint8_t rgb[W * H * 3U];

/* Fixed white balance and gain, no colour correction: the output is the
   sensor's linear level through the sRGB curve. */
static auris_image_config_t config(void) {
  return (auris_image_config_t){
      .source_width = SW, .source_height = SH, .width = W, .height = H,
      .bits = 10U, .black = BLACK, .gain = {1.0f, 1.0f, 1.0f},
      .colour_matrix = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
      .target = 0.18f, .max_gain = 16.0f};
}

static uint16_t counts(float level) {
  const float value = BLACK + level * (1023.0f - BLACK);
  return (uint16_t)(value > 1023.0f ? 1023.0f : value + 0.5f);
}

/* Paints the RGGB cell at (x, y), in cells, with linear levels 0..1. */
static void cell(size_t x, size_t y, float r, float g, float b) {
  raw[(2U * y) * SW + 2U * x] = counts(r);
  raw[(2U * y) * SW + 2U * x + 1U] = counts(g);
  raw[(2U * y + 1U) * SW + 2U * x] = counts(g);
  raw[(2U * y + 1U) * SW + 2U * x + 1U] = counts(b);
}

static void fill(float r, float g, float b) {
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) cell(x, y, r, g, b);
}

static float srgb(float linear) {
  return linear <= 0.0031308f ? 12.92f * linear
                              : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

static int near(unsigned int value, float expected, float tolerance) {
  return fabsf((float)value - expected) <= tolerance;
}

static const uint8_t *pixel(size_t x, size_t y) { return &rgb[(y * W + x) * 3U]; }

static auris_bayer_t input(void) {
  return (auris_bayer_t){raw, SW, SH, SW * 2U, sizeof(raw), 10U};
}
static auris_rgb24_t output(void) {
  return (auris_rgb24_t){rgb, W, H, W * 3U, sizeof(rgb)};
}

static void flat_and_validation(void) {
  auris_image_config_t c = config();
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  CHECK(auris_image_open(&s, &c) == -EINVAL); /* already open */
  const auris_image_rect_t *r = auris_image_content(s);
  CHECK(r->x == 0U && r->y == 0U && r->width == W && r->height == H);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  /* Half scale is half scale: black is subtracted, nothing else moves. */
  fill(0.5f, 0.5f, 0.5f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t i = 0U; i < W * H * 3U; ++i)
    CHECK(near(rgb[i], 255.0f * srgb(0.5f), 1.5f));
  /* At and below black is black; the sensor's limit is white. */
  for (size_t i = 0U; i < SW * SH; ++i) raw[i] = 60U;
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t i = 0U; i < W * H * 3U; ++i) CHECK(rgb[i] == 0U);
  for (size_t i = 0U; i < SW * SH; ++i) raw[i] = 1023U;
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t i = 0U; i < W * H * 3U; ++i) CHECK(rgb[i] == 255U);
  /* Buffers that do not hold the picture are refused. */
  in.bytes = sizeof(raw) - 2U;
  CHECK(auris_image_render(s, &in, &out) == -EINVAL);
  in = input(); in.bits = 8U;
  CHECK(auris_image_render(s, &in, &out) == -EINVAL);
  in = input(); out.bytes = sizeof(rgb) - 1U;
  CHECK(auris_image_render(s, &in, &out) == -EINVAL);
  out = output(); out.width = W - 1U;
  CHECK(auris_image_render(s, &in, &out) == -EINVAL);
  CHECK(auris_image_render(NULL, &in, &out) == -EINVAL);
  auris_image_close(s);
  s = NULL;
  c.bits = 12U; CHECK(auris_image_open(&s, &c) == -EINVAL && s == NULL);
  c = config(); c.source_width = 63U; CHECK(auris_image_open(&s, &c) == -EINVAL);
  c = config(); c.black = 2000.0f; CHECK(auris_image_open(&s, &c) == -EINVAL);
  c = config(); c.gain[1] = INFINITY; CHECK(auris_image_open(&s, &c) == -EINVAL);
  c = config(); c.width = 0U; CHECK(auris_image_open(&s, &c) == -EINVAL);
  c = config(); c.colour_matrix[1][1] = NAN; CHECK(auris_image_open(&s, &c) == -EINVAL);
  c = config(); c.target = 0.0f; CHECK(auris_image_open(&s, &c) == -EINVAL);
  c = config(); c.max_gain = 0.5f; CHECK(auris_image_open(&s, &c) == -EINVAL);
  CHECK(auris_image_open(NULL, &c) == -EINVAL);
  CHECK(auris_image_content(NULL) == NULL);
  auris_image_close(NULL);
}

static void averaging(void) {
  /* Every output pixel covers 2 x 2 cells. A checkerboard of bright and
     dark cells comes out as their mean in linear light, with no pattern:
     every sensor pixel counts, which is also what lowers the noise. */
  auris_image_config_t c = config();
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) {
      const float v = ((x + y) & 1U) ? 0.8f : 0.2f;
      cell(x, y, v, v, v);
    }
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t i = 0U; i < W * H * 3U; ++i)
    CHECK(near(rgb[i], 255.0f * srgb(0.5f), 1.5f));
  /* A step edge stays where it is. */
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) {
      const float v = x < SW / 4U ? 0.1f : 0.6f;
      cell(x, y, v, v, v);
    }
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t y = 0U; y < H; ++y) {
    CHECK(near(pixel(W / 2U - 1U, y)[1], 255.0f * srgb(0.1f), 1.5f));
    CHECK(near(pixel(W / 2U, y)[1], 255.0f * srgb(0.6f), 1.5f));
  }
  auris_image_close(s);
}

static void highlights(void) {
  /* A lamp or a window: every channel at the sensor's limit. With red and
     blue gained up by the white balance it must still be white, not pink. */
  auris_image_config_t c = config();
  c.gain[0] = 1.6f; c.gain[2] = 2.2f;
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  for (size_t i = 0U; i < SW * SH; ++i) raw[i] = 1023U;
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t i = 0U; i < W * H * 3U; ++i) CHECK(rgb[i] == 255U);
  /* The edge of the lamp, half the cells of each pixel at the limit and
     half a neutral grey under the same light: no coloured fringe. */
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) {
      if ((x + y) & 1U) cell(x, y, 2.0f, 2.0f, 2.0f);
      else cell(x, y, 0.3f / 1.6f, 0.3f, 0.3f / 2.2f);
    }
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t i = 0U; i < W * H; ++i) {
    CHECK(abs((int)rgb[i * 3U] - (int)rgb[i * 3U + 1U]) <= 2);
    CHECK(abs((int)rgb[i * 3U + 2U] - (int)rgb[i * 3U + 1U]) <= 2);
    CHECK(near(rgb[i * 3U + 1U], 255.0f * srgb(0.65f), 2.0f));
  }
  auris_image_stats_t stats;
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(fabsf(stats.clipped - 0.5f) < 0.05f && stats.bright > 0.97f);
  auris_image_close(s);
}

static void white_balance(void) {
  auris_image_config_t c = config();
  c.auto_white_balance = true;
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  auris_image_stats_t stats;
  /* Greys under warm light: red strong, blue weak. They come out grey. */
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) {
      const float v = 0.2f + 0.3f * (float)x / (float)(SW / 2U);
      cell(x, y, v / 1.5f, v, v / 2.0f);
    }
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(fabsf(stats.gain[0] - 1.5f) < 0.03f && stats.gain[1] == 1.0f &&
        fabsf(stats.gain[2] - 2.0f) < 0.04f);
  for (size_t i = 0U; i < W * H; ++i) {
    CHECK(abs((int)rgb[i * 3U] - (int)rgb[i * 3U + 1U]) <= 3);
    CHECK(abs((int)rgb[i * 3U + 2U] - (int)rgb[i * 3U + 1U]) <= 3);
  }
  auris_image_close(s);
  /* A quarter of the picture is a vivid red object and an eighth is a lamp
     at the limit. Neither is evidence about the light: same balance. */
  s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) {
      if (x < SW / 8U) cell(x, y, 0.6f, 0.05f, 0.03f);
      else if (x >= 7U * (SW / 2U) / 8U) cell(x, y, 2.0f, 2.0f, 2.0f);
    }
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(fabsf(stats.gain[0] - 1.5f) < 0.08f && fabsf(stats.gain[2] - 2.0f) < 0.1f);
  /* A later frame under cooler light: the balance follows, but not in one
     jump, so noise does not make the colour flicker. */
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) cell(x, y, 0.3f / 2.0f, 0.3f, 0.3f / 1.5f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(stats.gain[0] > 1.52f && stats.gain[0] < 1.8f);
  for (size_t frames = 0U; frames < 60U; ++frames)
    CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(fabsf(stats.gain[0] - 2.0f) < 0.05f && fabsf(stats.gain[2] - 1.5f) < 0.05f);
  auris_image_close(s);
}

static void brightness(void) {
  auris_image_config_t c = config();
  c.auto_exposure = true;
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  auris_image_stats_t stats;
  /* An underexposed frame is lifted to the target in software. */
  fill(0.02f, 0.02f, 0.02f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(fabsf(stats.mean - 0.02f) < 0.002f && fabsf(stats.bright - 0.02f) < 0.01f);
  CHECK(stats.clipped == 0.0f && fabsf(stats.exposure - 9.0f) < 0.5f);
  CHECK(near(pixel(3U, 3U)[1], 255.0f * srgb(0.18f), 4.0f));
  /* The sensor's exposure doubles: the software gain follows at once, so
     the picture does not flash. */
  fill(0.04f, 0.04f, 0.04f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(near(pixel(3U, 3U)[1], 255.0f * srgb(0.18f), 4.0f));
  /* A small change is followed gradually. */
  fill(0.044f, 0.044f, 0.044f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(stats.exposure < 4.5f && stats.exposure > 0.18f / 0.044f + 0.05f);
  /* It never darkens the picture, and its lift is limited. */
  fill(0.5f, 0.5f, 0.5f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(stats.exposure == 1.0f);
  CHECK(near(pixel(3U, 3U)[1], 255.0f * srgb(0.5f), 1.5f));
  fill(0.001f, 0.001f, 0.001f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(stats.exposure == 16.0f);
  CHECK(auris_image_stats(NULL, &stats) == -EINVAL);
  CHECK(auris_image_stats(s, NULL) == -EINVAL);
  auris_image_close(s);
}

static void colour(void) {
  /* The matrix works on linear light after the white balance. */
  auris_image_config_t c = config();
  const float m[3][3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}};
  memcpy(c.colour_matrix, m, sizeof(m));
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  fill(0.1f, 0.4f, 0.7f);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(near(pixel(5U, 5U)[0], 255.0f * srgb(0.4f), 1.5f) &&
        near(pixel(5U, 5U)[1], 255.0f * srgb(0.7f), 1.5f) &&
        near(pixel(5U, 5U)[2], 255.0f * srgb(0.1f), 1.5f));
  auris_image_close(s);
  /* A strong matrix multiplies noise, which shows as coloured speckle in
     the dark. Below the knee, colour fades toward grey; above, it stays. */
  const float strong[3][3] = {{2.2f, -0.7f, -0.5f}, {-0.7f, 2.8f, -1.1f},
                              {-0.3f, -0.7f, 2.0f}};
  memcpy(c.colour_matrix, strong, sizeof(strong));
  unsigned int spread[2][2];
  for (size_t knee = 0U; knee < 2U; ++knee) {
    c.shadow_knee = knee == 0U ? 0.0f : 0.02f;
    s = NULL;
    CHECK(auris_image_open(&s, &c) == 0);
    for (size_t level = 0U; level < 2U; ++level) {
      const float v = level == 0U ? 0.004f : 0.4f;
      fill(1.3f * v, v, 0.7f * v);
      CHECK(auris_image_render(s, &in, &out) == 0);
      spread[knee][level] = (unsigned int)abs((int)pixel(5U, 5U)[0] - (int)pixel(5U, 5U)[2]);
    }
    auris_image_close(s);
  }
  CHECK(spread[0][0] >= 8U && spread[1][0] * 2U <= spread[0][0]);
  CHECK(spread[1][1] * 10U >= spread[0][1] * 9U);
}

static void tone(void) {
  /* A little contrast: mid-grey stays, darks go darker and lights lighter,
     and the order of levels never changes. */
  auris_image_config_t c = config();
  c.contrast = 0.35f;
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  fill(0.2140f, 0.2140f, 0.2140f); /* codes to one half */
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(near(pixel(4U, 4U)[1], 127.5f, 1.5f));
  fill(0.02f, 0.02f, 0.02f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(near(pixel(4U, 4U)[1], 30.7f, 1.5f)); /* 38.7 without */
  fill(0.6f, 0.6f, 0.6f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(near(pixel(4U, 4U)[1], 212.0f, 1.5f)); /* 203.4 without */
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x) {
      const float v = (float)x / (float)(SW / 2U - 1U);
      cell(x, y, v, v, v);
    }
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t x = 1U; x < W; ++x) CHECK(pixel(x, 4U)[1] >= pixel(x - 1U, 4U)[1]);
  CHECK(pixel(0U, 4U)[1] < 40U && pixel(W - 1U, 4U)[1] > 245U);
  auris_image_close(s);
  s = NULL;
  c.contrast = 1.5f;
  CHECK(auris_image_open(&s, &c) == -EINVAL);
}

static void measuring(void) {
  /* A frame can be looked at without being drawn, to settle the exposure
     before the first picture is shown. */
  auris_image_config_t c = config();
  c.auto_exposure = true;
  c.auto_white_balance = true;
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  auris_image_stats_t stats;
  fill(0.03f / 1.5f, 0.03f, 0.03f / 2.0f);
  memset(rgb, 7, sizeof(rgb));
  CHECK(auris_image_measure(s, &in) == 0);
  CHECK(rgb[0] == 7U && rgb[sizeof(rgb) - 1U] == 7U);
  CHECK(auris_image_stats(s, &stats) == 0);
  CHECK(fabsf(stats.mean - 0.03f) < 0.002f && fabsf(stats.exposure - 6.0f) < 0.3f);
  /* At this level a sample is only 14 to 29 counts: allow for rounding. */
  CHECK(fabsf(stats.gain[0] - 1.5f) < 0.08f && fabsf(stats.gain[2] - 2.0f) < 0.1f);
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(near(pixel(4U, 4U)[0], 255.0f * srgb(0.18f), 4.0f) &&
        near(pixel(4U, 4U)[2], 255.0f * srgb(0.18f), 4.0f));
  in.bytes = 10U;
  CHECK(auris_image_measure(s, &in) == -EINVAL);
  CHECK(auris_image_measure(NULL, &in) == -EINVAL);
  auris_image_close(s);
}

static void shading(void) {
  /* The lens passes less light toward its edge. A grid of gains, 1 at the
     left here and 2 at the right, undoes it. */
  static const float gains[2U * 2U * 3U] = {1, 1, 1, 2, 2, 2, 1, 1, 1, 2, 2, 2};
  auris_image_config_t c = config();
  c.shading = (auris_shading_t){.columns = 2U, .rows = 2U, .gains = gains};
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  fill(0.2f, 0.2f, 0.2f);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  CHECK(auris_image_render(s, &in, &out) == 0);
  CHECK(near(pixel(0U, 4U)[1], 255.0f * srgb(0.2f * (1.0f + 0.5f / 16.0f)), 2.0f));
  CHECK(near(pixel(W - 1U, 4U)[1], 255.0f * srgb(0.2f * (2.0f - 0.5f / 16.0f)), 2.0f));
  for (size_t x = 1U; x < W; ++x) CHECK(pixel(x, 4U)[1] >= pixel(x - 1U, 4U)[1]);
  CHECK(pixel(8U, 0U)[1] == pixel(8U, H - 1U)[1]);
  auris_image_close(s);
  s = NULL;
  static const float bad[2U * 2U * 3U] = {1, 1, 1, 0, 2, 2, 1, 1, 1, 2, 2, 2};
  c.shading.gains = bad;
  CHECK(auris_image_open(&s, &c) == -EINVAL);
  c.shading = (auris_shading_t){.columns = 1U, .rows = 2U, .gains = gains};
  CHECK(auris_image_open(&s, &c) == -EINVAL);
}

static void eight_bit(void) {
  /* The same scene from an 8-bit sensor mode: the same picture. */
  auris_image_config_t c = config();
  c.gain[0] = 1.5f; c.gain[2] = 1.8f;
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  for (size_t y = 0U; y < SH / 2U; ++y)
    for (size_t x = 0U; x < SW / 2U; ++x)
      cell(x, y, 0.1f + 0.01f * (float)x, 0.3f, 0.05f + 0.01f * (float)y);
  auris_bayer_t in = input();
  auris_rgb24_t out = output();
  CHECK(auris_image_render(s, &in, &out) == 0);
  static uint8_t wide[W * H * 3U];
  memcpy(wide, rgb, sizeof(rgb));
  auris_image_close(s);
  for (size_t i = 0U; i < SW * SH; ++i) raw8[i] = (uint8_t)((raw[i] + 2U) / 4U);
  c.bits = 8U; c.black = BLACK / 4.0f;
  s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  /* A padded stride, as capture drivers report. */
  static uint8_t padded[(SW + 8U) * SH];
  for (size_t y = 0U; y < SH; ++y) memcpy(&padded[y * (SW + 8U)], &raw8[y * SW], SW);
  auris_bayer_t in8 = {padded, SW, SH, SW + 8U, sizeof(padded), 8U};
  CHECK(auris_image_render(s, &in8, &out) == 0);
  for (size_t i = 0U; i < sizeof(rgb); ++i) CHECK(abs((int)rgb[i] - (int)wide[i]) <= 4);
  auris_image_close(s);
}

static void layouts(void) {
  /* The 4:3 sensor inside each canvas, bars at the sides or top and bottom. */
  const uint32_t sizes[][6]={{640,360,80,0,479,360},{1280,720,161,0,958,720},
                           {360,640,0,185,360,270},{1,1,0,0,1,1}};
  for(size_t i=0U;i<4U;++i) {
    auris_image_config_t c=config();
    c.source_width=1640U; c.source_height=1232U; c.width=sizes[i][0]; c.height=sizes[i][1];
    auris_image_t *s=NULL;
    CHECK(auris_image_open(&s,&c)==0);
    const auris_image_rect_t *r=auris_image_content(s);
    CHECK(r->x==sizes[i][2] && r->y==sizes[i][3] && r->width==sizes[i][4] && r->height==sizes[i][5]);
    auris_image_close(s);
  }
  /* The bars are black and the picture fills the rest. */
  static uint8_t canvas[32U * 12U * 3U];
  auris_image_config_t c = config();
  c.width = 32U;
  auris_image_t *s = NULL;
  CHECK(auris_image_open(&s, &c) == 0);
  fill(0.5f, 0.5f, 0.5f);
  memset(canvas, 99, sizeof(canvas));
  auris_bayer_t in = input();
  auris_rgb24_t out = {canvas, 32U, H, 32U * 3U, sizeof(canvas)};
  CHECK(auris_image_render(s, &in, &out) == 0);
  for (size_t y = 0U; y < H; ++y)
    for (size_t x = 0U; x < 32U; ++x)
      CHECK(x < 8U || x >= 24U ? canvas[(y * 32U + x) * 3U + 1U] == 0U
                               : near(canvas[(y * 32U + x) * 3U + 1U], 187.5f, 1.5f));
  auris_image_close(s);
}

int main(void) {
  flat_and_validation(); averaging(); highlights(); white_balance();
  brightness(); colour(); tone(); measuring(); shading(); eight_bit(); layouts();
  puts("Image checks passed"); return EXIT_SUCCESS;
}
#else
#include <stdio.h>
int main(void) { puts("Module 11 image library not implemented yet"); return 77; }
#endif
