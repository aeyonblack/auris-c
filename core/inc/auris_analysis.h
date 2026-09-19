#ifndef AURIS_ANALYSIS_H
#define AURIS_ANALYSIS_H

#include "auris_audio.h"
#include "auris_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AURIS_ANALYSIS_SIZE 512U
#define AURIS_ANALYSIS_HOP 256U

typedef struct {
  unsigned int sample_rate;
  size_t channels;
  uint64_t first_frame;
  float samples[AURIS_AUDIO_CHANNELS][AURIS_ANALYSIS_SIZE];
} auris_analysis_frame_t;

typedef struct {
  auris_analysis_frame_t history;
  size_t filled;
  uint64_t next_frame;
  bool active;
} auris_framer_t;

typedef struct {
  float window[AURIS_ANALYSIS_SIZE];
} auris_preprocess_t;

void auris_framer_reset(auris_framer_t *state);
auris_status_t auris_framer_push(auris_framer_t *state,
                                 const auris_audio_block_t *block,
                                 auris_analysis_frame_t *out, bool *ready);
void auris_preprocess_init(auris_preprocess_t *state);
auris_status_t auris_preprocess_apply(const auris_preprocess_t *state,
                                      auris_analysis_frame_t *frame);

#endif
