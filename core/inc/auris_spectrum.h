#ifndef AURIS_SPECTRUM_H
#define AURIS_SPECTRUM_H

#include "auris_analysis.h"
#include "auris_status.h"
#include <stddef.h>
#include <stdint.h>

#define AURIS_FFT_BINS (AURIS_ANALYSIS_SIZE / 2U + 1U)

typedef struct auris_fft auris_fft_t;

typedef struct {
  unsigned int sample_rate;
  size_t channels;
  uint64_t first_frame;
  float bins[AURIS_AUDIO_CHANNELS][AURIS_FFT_BINS][2];
} auris_spectrum_t;

typedef struct {
  float correlation[AURIS_ANALYSIS_SIZE];
  int lag_samples;
  float peak;
  bool valid;
} auris_gcc_t;

auris_fft_t *auris_fft_create(void);
void auris_fft_destroy(auris_fft_t *fft);
auris_status_t auris_spectrum_apply(auris_fft_t *fft,
                                    const auris_analysis_frame_t *frame,
                                    auris_spectrum_t *out);
auris_status_t auris_gcc_phat(auris_fft_t *fft,
                              const auris_spectrum_t *spectrum, size_t a,
                              size_t b, int max_lag, auris_gcc_t *out);

#endif
