#ifndef AURIS_OUTPUT_H
#define AURIS_OUTPUT_H

#include "auris_image.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct auris_output auris_output_t;

typedef struct {
  uint32_t sequence;
  uint64_t camera_us;
  uint32_t flags;
  uint64_t audio_us;
  uint64_t composed_us;
  uint64_t published_us;
  bool visible;
  bool evidence;
  uint32_t samples;
  uint32_t fits;
  uint32_t checks;
  uint64_t dsp_us;
} auris_output_frame_t;

typedef struct {
  uint64_t submitted;
  uint64_t sent;
  uint64_t dropped;
  uint64_t encode_us;
  uint64_t write_us;
  size_t encoded_bytes;
} auris_output_stats_t;

/* JPEG is one replaceable output frontend. submit() only copies into a
   one-frame latest-value mailbox; encoding and writes happen on its worker. */
int auris_output_open_jpeg_stream(auris_output_t **out, int fd,
                                  uint32_t width, uint32_t height,
                                  unsigned int quality);
int auris_output_submit(auris_output_t *output, const auris_rgb24_t *image,
                        const auris_output_frame_t *frame);
int auris_output_flush(auris_output_t *output, unsigned int timeout_ms);
int auris_output_error(const auris_output_t *output);
void auris_output_stats(const auris_output_t *output,
                        auris_output_stats_t *stats);
void auris_output_close(auris_output_t *output);

#endif
