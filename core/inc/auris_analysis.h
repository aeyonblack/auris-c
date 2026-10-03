#ifndef AURIS_ANALYSIS_H
#define AURIS_ANALYSIS_H

#include "auris_audio.h"
#include "auris_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Default frame, used by auris_framer_reset/auris_preprocess_init and by
   the stages that still work at one fixed size (spectrum, GCC, SRP). */
#define AURIS_ANALYSIS_SIZE 512U
#define AURIS_ANALYSIS_HOP 256U

/* Limits for frames chosen at run time. Sizes are powers of two. */
#define AURIS_ANALYSIS_MIN_SIZE 64U
#define AURIS_ANALYSIS_MAX_SIZE 2048U

typedef struct {
  unsigned int sample_rate;
  size_t channels;
  size_t size; /* valid samples per channel */
  uint64_t first_frame;
  float samples[AURIS_AUDIO_CHANNELS][AURIS_ANALYSIS_MAX_SIZE];
} auris_analysis_frame_t;

typedef struct {
  auris_analysis_frame_t history;
  size_t size, hop;
  size_t filled;
  uint64_t next_frame;
  bool active;
} auris_framer_t;

typedef struct {
  size_t size;
  float window[AURIS_ANALYSIS_MAX_SIZE];
} auris_preprocess_t;

/* True for a power-of-two size within the limits and a hop between one
   capture block (AURIS_AUDIO_FRAMES) and the frame size. */
bool auris_analysis_size_valid(size_t size, size_t hop);

/* Configures and clears a framer. */
auris_status_t auris_framer_init(auris_framer_t *state, size_t size,
                                 size_t hop);
/* Same as auris_framer_init(state, AURIS_ANALYSIS_SIZE, AURIS_ANALYSIS_HOP). */
void auris_framer_reset(auris_framer_t *state);
/* Forgets the stream (history and continuity) but keeps size and hop. */
void auris_framer_restart(auris_framer_t *state);
auris_status_t auris_framer_push(auris_framer_t *state,
                                 const auris_audio_block_t *block,
                                 auris_analysis_frame_t *out, bool *ready);

/* Hann window of AURIS_ANALYSIS_SIZE samples. */
void auris_preprocess_init(auris_preprocess_t *state);
/* Hann window of `size` samples. */
auris_status_t auris_preprocess_init_size(auris_preprocess_t *state,
                                          size_t size);
/* The frame's size must equal the window's size. */
auris_status_t auris_preprocess_apply(const auris_preprocess_t *state,
                                      auris_analysis_frame_t *frame);

#endif
