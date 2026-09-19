#include "auris_spectrum.h"
#include "auris_analysis.h"
#include "auris_audio.h"
#include "auris_status.h"

#include <errno.h>
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct auris_fft {
  float *time;
  fftwf_complex *frequency;
  fftwf_plan forward;
  fftwf_plan inverse;
};

void auris_fft_destroy(auris_fft_t *fft) {
  if (fft == NULL) {
    return;
  }
  if (fft->forward != NULL) {
    fftwf_destroy_plan(fft->forward);
  }
  if (fft->inverse != NULL) {
    fftwf_destroy_plan(fft->inverse);
  }
  fftwf_free(fft->frequency);
  fftwf_free(fft->time);
  free(fft);
}

auris_fft_t *auris_fft_create(void) {
  auris_fft_t *fft = calloc(1U, sizeof(*fft));
  if (fft == NULL) {
    return NULL;
  }
  fft->time = fftwf_alloc_real(AURIS_ANALYSIS_SIZE);
  fft->frequency = fftwf_alloc_complex(AURIS_FFT_BINS);
  if (fft->time == NULL || fft->frequency == NULL) {
    auris_fft_destroy(fft);
    return NULL;
  }
  fft->forward = fftwf_plan_dft_r2c_1d((int)AURIS_ANALYSIS_SIZE, fft->time,
                                       fft->frequency, FFTW_ESTIMATE);
  fft->inverse = fftwf_plan_dft_c2r_1d((int)AURIS_ANALYSIS_SIZE, fft->frequency,
                                       fft->time, FFTW_ESTIMATE);
  if (fft->forward == NULL || fft->inverse == NULL) {
    auris_fft_destroy(fft);
    return NULL;
  }
  return fft;
}

auris_status_t auris_spectrum_apply(auris_fft_t *fft,
                                    const auris_analysis_frame_t *frame,
                                    auris_spectrum_t *out) {
  if (fft == NULL || frame == NULL || out == NULL || frame->sample_rate == 0U ||
      frame->channels == 0U || frame->channels > AURIS_AUDIO_CHANNELS) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  out->sample_rate = frame->sample_rate;
  out->channels = frame->channels;
  out->first_frame = frame->first_frame;
  for (size_t c = 0U; c < frame->channels; ++c) {
    memcpy(fft->time, frame->samples[c], AURIS_ANALYSIS_SIZE * sizeof(float));
    fftwf_execute(fft->forward);
    for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
      out->bins[c][k][0] = fft->frequency[k][0];
      out->bins[c][k][1] = fft->frequency[k][1];
    }
  }
  return AURIS_OK;
}

auris_status_t auris_gcc_phat(auris_fft_t *fft,
                              const auris_spectrum_t *spectrum, size_t a,
                              size_t b, int max_lag, auris_gcc_t *out) {
  if (fft == NULL || spectrum == NULL || out == NULL ||
      spectrum->channels > AURIS_AUDIO_CHANNELS || a >= spectrum->channels ||
      b >= spectrum->channels || a == b || max_lag < 0 ||
      max_lag >= (int)AURIS_ANALYSIS_SIZE / 2) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const float epsilon = 1.0e-12f;
  out->valid = false;
  out->lag_samples = 0;
  out->peak = 0.0f;
  for (size_t k = 0U; k < AURIS_FFT_BINS; ++k) {
    const float ar = spectrum->bins[a][k][0];
    const float ai = spectrum->bins[a][k][1];
    const float br = spectrum->bins[b][k][0];
    const float bi = spectrum->bins[b][k][1];
    const float real = br * ar + bi * ai;
    const float imag = bi * ar - br * ai;
    const float magnitude = hypotf(real, imag);
    fft->frequency[k][0] = 0.0f;
    fft->frequency[k][1] = 0.0f;
    if (k > 0U && k + 1U < AURIS_FFT_BINS) {
      fft->frequency[k][0] = real / (magnitude + epsilon);
      fft->frequency[k][1] = imag / (magnitude + epsilon);
      if (magnitude > epsilon) {
        out->valid = true;
      }
    }
  }
  fftwf_execute(fft->inverse);
  for (size_t n = 0U; n < AURIS_ANALYSIS_SIZE; ++n) {
    out->correlation[n] = fft->time[n] / (float)AURIS_ANALYSIS_SIZE;
  }
  if (!out->valid) {
    return AURIS_OK;
  }
  float best = -INFINITY;
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    const size_t index =
        (size_t)(lag < 0 ? (int)AURIS_ANALYSIS_SIZE + lag : lag);
    if (out->correlation[index] > best) {
      best = out->correlation[index];
      out->lag_samples = lag;
    }
  }
  out->peak = best;
  return AURIS_OK;
}
