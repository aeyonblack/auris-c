#include "auris_spectrum.h"

#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct auris_fft {
  size_t size, upsample;
  float *time;
  fftwf_complex *frequency;
  fftwf_plan forward;
  fftwf_plan inverse;
  fftwf_plan fine_inverse;
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
  if (fft->fine_inverse != NULL) {
    fftwf_destroy_plan(fft->fine_inverse);
  }
  fftwf_free(fft->frequency);
  fftwf_free(fft->time);
  free(fft);
}

auris_fft_t *auris_fft_create_size(size_t size, size_t upsample) {
  /* Any hop is fine here; the size rules are the framer's. */
  if (!auris_analysis_size_valid(size, size) ||
      (upsample != 1U && upsample != 2U && upsample != 4U &&
       upsample != 8U)) {
    return NULL;
  }
  auris_fft_t *fft = calloc(1U, sizeof(*fft));
  if (fft == NULL) {
    return NULL;
  }
  const size_t fine = size * upsample;
  fft->size = size;
  fft->upsample = upsample;
  fft->time = fftwf_alloc_real(fine);
  fft->frequency = fftwf_alloc_complex(fine / 2U + 1U);
  if (fft->time == NULL || fft->frequency == NULL) {
    auris_fft_destroy(fft);
    return NULL;
  }
  fft->forward = fftwf_plan_dft_r2c_1d((int)size, fft->time, fft->frequency,
                                       FFTW_ESTIMATE);
  fft->inverse = fftwf_plan_dft_c2r_1d((int)size, fft->frequency, fft->time,
                                       FFTW_ESTIMATE);
  fft->fine_inverse = fftwf_plan_dft_c2r_1d((int)fine, fft->frequency,
                                            fft->time, FFTW_ESTIMATE);
  if (fft->forward == NULL || fft->inverse == NULL ||
      fft->fine_inverse == NULL) {
    auris_fft_destroy(fft);
    return NULL;
  }
  return fft;
}

auris_fft_t *auris_fft_create(void) {
  return auris_fft_create_size(AURIS_ANALYSIS_SIZE, AURIS_GCC_UPSAMPLE);
}

size_t auris_fft_size(const auris_fft_t *fft) {
  return fft == NULL ? 0U : fft->size;
}

size_t auris_fft_upsample(const auris_fft_t *fft) {
  return fft == NULL ? 0U : fft->upsample;
}

size_t auris_fft_curve_size(const auris_fft_t *fft) {
  return fft == NULL ? 0U : fft->size * fft->upsample;
}

auris_status_t auris_spectrum_apply(auris_fft_t *fft,
                                    const auris_analysis_frame_t *frame,
                                    auris_spectrum_t *out) {
  if (fft == NULL || frame == NULL || out == NULL || frame->sample_rate == 0U ||
      frame->channels == 0U || frame->channels > AURIS_AUDIO_CHANNELS ||
      frame->size != fft->size) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const size_t bins = fft->size / 2U + 1U;
  out->sample_rate = frame->sample_rate;
  out->channels = frame->channels;
  out->size = fft->size;
  out->first_frame = frame->first_frame;
  out->weights = NULL;
  for (size_t c = 0U; c < frame->channels; ++c) {
    memcpy(fft->time, frame->samples[c], fft->size * sizeof(float));
    fftwf_execute(fft->forward);
    for (size_t k = 0U; k < bins; ++k) {
      out->bins[c][k][0] = fft->frequency[k][0];
      out->bins[c][k][1] = fft->frequency[k][1];
    }
  }
  return AURIS_OK;
}

auris_status_t auris_spectrum_band(auris_spectrum_t *s, float low_hz,
                                   float high_hz) {
  if (s == NULL || s->sample_rate == 0U || s->channels > AURIS_AUDIO_CHANNELS ||
      !auris_analysis_size_valid(s->size, s->size) || !isfinite(low_hz) ||
      !isfinite(high_hz) || low_hz < 0.0f || low_hz >= high_hz) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const size_t bins = s->size / 2U + 1U;
  const float bin_hz = (float)s->sample_rate / (float)s->size;
  const float first = fmaxf(1.0f, ceilf(low_hz / bin_hz));
  const float last = fminf((float)(bins - 2U), floorf(high_hz / bin_hz));
  for (size_t c = 0U; c < s->channels; ++c) {
    for (size_t k = 0U; k < bins; ++k) {
      if ((float)k < first || (float)k > last) {
        s->bins[c][k][0] = 0.0f;
        s->bins[c][k][1] = 0.0f;
      }
    }
  }
  return AURIS_OK;
}

auris_status_t auris_spectrum_band_power(const auris_spectrum_t *s,
                                         size_t channel, float low_hz,
                                         float high_hz, float window_energy,
                                         float *power) {
  if (s == NULL || power == NULL || s->sample_rate == 0U ||
      s->channels > AURIS_AUDIO_CHANNELS || channel >= s->channels ||
      !auris_analysis_size_valid(s->size, s->size) || !isfinite(low_hz) ||
      !isfinite(high_hz) || low_hz < 0.0f || low_hz >= high_hz ||
      !isfinite(window_energy) || window_energy <= 0.0f ||
      window_energy > (float)s->size) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const size_t bins = s->size / 2U + 1U;
  const float bin_hz = (float)s->sample_rate / (float)s->size;
  const float first = fmaxf(1.0f, ceilf(low_hz / bin_hz));
  const float last = fminf((float)(bins - 2U), floorf(high_hz / bin_hz));
  if (first > last) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  double energy = 0.0;
  for (size_t k = (size_t)first; k <= (size_t)last; ++k) {
    const double re = (double)s->bins[channel][k][0];
    const double im = (double)s->bins[channel][k][1];
    energy += re * re + im * im;
  }
  /* x2 for the negative frequencies a real FFT does not store. */
  *power = (float)(2.0 * energy / ((double)s->size * (double)window_energy));
  return AURIS_OK;
}

static bool pair_valid(const auris_fft_t *fft, const auris_spectrum_t *s,
                       size_t a, size_t b) {
  return fft != NULL && s != NULL && s->size == fft->size &&
         s->channels <= AURIS_AUDIO_CHANNELS && a < s->channels &&
         b < s->channels && a != b;
}

/* Writes the PHAT-weighted cross-spectrum B x conj(A) into the scratch
   spectrum, zero everywhere except the interior bins 1..size/2-1, and
   clears the rest of the (possibly longer) fine spectrum. Each bin's unit
   phasor is scaled by its weight; *weight receives the total that voted. */
static bool prepare_phat(auris_fft_t *fft, const auris_spectrum_t *s, size_t a,
                         size_t b, float *weight) {
  const float epsilon = 1.0e-12f;
  const size_t bins = fft->size / 2U + 1U;
  bool valid = false;
  float total = 0.0f;
  memset(fft->frequency, 0,
         (fft->size * fft->upsample / 2U + 1U) * sizeof(fftwf_complex));
  for (size_t k = 1U; k + 1U < bins; ++k) {
    const float ar = s->bins[a][k][0];
    const float ai = s->bins[a][k][1];
    const float br = s->bins[b][k][0];
    const float bi = s->bins[b][k][1];
    const float real = br * ar + bi * ai;
    const float imag = bi * ar - br * ai;
    const float magnitude = hypotf(real, imag);
    const float w = s->weights == NULL ? 1.0f : s->weights[k];
    fft->frequency[k][0] = w * real / (magnitude + epsilon);
    fft->frequency[k][1] = w * imag / (magnitude + epsilon);
    if (magnitude > epsilon) {
      valid = true;
      total += w;
    }
  }
  if (weight != NULL) {
    *weight = total;
  }
  return valid;
}

auris_status_t auris_gcc_phat(auris_fft_t *fft, const auris_spectrum_t *s,
                              size_t a, size_t b, int max_lag,
                              auris_gcc_t *out) {
  if (out == NULL || !pair_valid(fft, s, a, b) || max_lag < 0 ||
      max_lag >= (int)fft->size / 2) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const size_t size = fft->size;
  out->size = size;
  out->lag_samples = 0;
  out->peak = 0.0f;
  out->valid = prepare_phat(fft, s, a, b, NULL);
  fftwf_execute(fft->inverse);
  for (size_t n = 0U; n < size; ++n) {
    out->correlation[n] = fft->time[n] / (float)size;
  }
  if (!out->valid) {
    return AURIS_OK;
  }
  float best = -INFINITY;
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    const size_t index = (size_t)(lag < 0 ? (int)size + lag : lag);
    if (out->correlation[index] > best) {
      best = out->correlation[index];
      out->lag_samples = lag;
    }
  }
  out->peak = best;
  return AURIS_OK;
}

auris_status_t auris_gcc_phat_curve(auris_fft_t *fft, const auris_spectrum_t *s,
                                    size_t a, size_t b, float *curve,
                                    bool *valid) {
  float weight = 0.0f;
  return auris_gcc_phat_curve_weighted(fft, s, a, b, curve, valid, &weight);
}

auris_status_t auris_gcc_phat_curve_weighted(auris_fft_t *fft,
                                             const auris_spectrum_t *s,
                                             size_t a, size_t b, float *curve,
                                             bool *valid, float *weight) {
  if (curve == NULL || valid == NULL || weight == NULL ||
      !pair_valid(fft, s, a, b)) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *valid = prepare_phat(fft, s, a, b, weight);
  fftwf_execute(fft->fine_inverse);
  /* Divide by the frame length, not the fine length, so the curve has the
     same scale as auris_gcc_phat's correlation at every upsampling factor. */
  const size_t fine = fft->size * fft->upsample;
  for (size_t n = 0U; n < fine; ++n) {
    curve[n] = fft->time[n] / (float)fft->size;
  }
  return AURIS_OK;
}
