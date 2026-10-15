#include "auris_spectrum.h"

#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

struct auris_fft {
  size_t size, upsample;
  float *time;
  fftwf_complex *frequency;
  fftwf_plan forward;
  fftwf_plan inverse;
  fftwf_plan fine_inverse;
  /* auris_gcc_phat_lags: cos and sin of 2 pi k n / (size * upsample) for
     lags n = -max_lag..max_lag (rows) and bins k = 0..size / 2 (columns),
     plus room for one pair's weighted phasors. */
  size_t max_lag, row;
  float *cosine, *sine;
  float *phasor_re, *phasor_im;
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
  free(fft->cosine);
  free(fft->sine);
  free(fft->phasor_re);
  free(fft->phasor_im);
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
   spectrum, zero everywhere except bins first..last (kept inside the
   interior bins 1..size/2-1), and
   clears the rest of the (possibly longer) fine spectrum. Each bin's unit
   phasor is scaled by its weight and, when `gains` is given, by the pair's
   gain for that bin; *weight receives the total that voted. */
static bool prepare_phat(auris_fft_t *fft, const auris_spectrum_t *s, size_t a,
                         size_t b, const float *gains, size_t first,
                         size_t last, float *weight) {
  const float epsilon = 1.0e-12f;
  const size_t bins = fft->size / 2U + 1U;
  bool valid = false;
  float total = 0.0f;
  memset(fft->frequency, 0,
         (fft->size * fft->upsample / 2U + 1U) * sizeof(fftwf_complex));
  if (first < 1U) {
    first = 1U;
  }
  for (size_t k = first; k + 1U < bins && k <= last; ++k) {
    const float ar = s->bins[a][k][0];
    const float ai = s->bins[a][k][1];
    const float br = s->bins[b][k][0];
    const float bi = s->bins[b][k][1];
    const float real = br * ar + bi * ai;
    const float imag = bi * ar - br * ai;
    const float magnitude = hypotf(real, imag);
    const float w = (s->weights == NULL ? 1.0f : s->weights[k]) *
                    (gains == NULL ? 1.0f : gains[k]);
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
  out->valid = prepare_phat(fft, s, a, b, NULL, 1U, fft->size / 2U, NULL);
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
  return auris_gcc_phat_curve_gained(fft, s, a, b, NULL, curve, valid, weight);
}

auris_status_t auris_gcc_phat_curve_gained(auris_fft_t *fft,
                                           const auris_spectrum_t *s, size_t a,
                                           size_t b, const float *gains,
                                           float *curve, bool *valid,
                                           float *weight) {
  if (fft == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  return auris_gcc_phat_curve_band(fft, s, a, b, gains, 1U, fft->size / 2U,
                                   curve, valid, weight);
}

auris_status_t auris_gcc_phat_curve_band(auris_fft_t *fft,
                                         const auris_spectrum_t *s, size_t a,
                                         size_t b, const float *gains,
                                         size_t first_bin, size_t last_bin,
                                         float *curve, bool *valid,
                                         float *weight) {
  if (curve == NULL || valid == NULL || weight == NULL ||
      !pair_valid(fft, s, a, b) || first_bin > last_bin ||
      last_bin > fft->size / 2U) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  *valid = prepare_phat(fft, s, a, b, gains, first_bin, last_bin, weight);
  fftwf_execute(fft->fine_inverse);
  /* Divide by the frame length, not the fine length, so the curve has the
     same scale as auris_gcc_phat's correlation at every upsampling factor. */
  const size_t fine = fft->size * fft->upsample;
  for (size_t n = 0U; n < fine; ++n) {
    curve[n] = fft->time[n] / (float)fft->size;
  }
  return AURIS_OK;
}

auris_status_t auris_fft_reserve_lags(auris_fft_t *fft, size_t max_lag) {
  if (fft == NULL || max_lag > fft->size * fft->upsample / 2U) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  if (fft->cosine != NULL && fft->max_lag >= max_lag) {
    return AURIS_OK;
  }
  const size_t bins = fft->size / 2U + 1U;
  /* Rows padded to a multiple of four floats for the vector loads. */
  const size_t row = (bins + 3U) & ~(size_t)3U;
  const size_t rows = 2U * max_lag + 1U;
  float *cosine = calloc(rows * row, sizeof(float));
  float *sine = calloc(rows * row, sizeof(float));
  float *re = fft->phasor_re != NULL ? fft->phasor_re
                                     : calloc(row, sizeof(float));
  float *im = fft->phasor_im != NULL ? fft->phasor_im
                                     : calloc(row, sizeof(float));
  if (cosine == NULL || sine == NULL || re == NULL || im == NULL) {
    free(cosine);
    free(sine);
    if (fft->phasor_re == NULL) {
      free(re);
    }
    if (fft->phasor_im == NULL) {
      free(im);
    }
    return AURIS_ERROR_INTERNAL;
  }
  const size_t fine = fft->size * fft->upsample;
  for (size_t r = 0U; r < rows; ++r) {
    const long lag = (long)r - (long)max_lag;
    const size_t n = (size_t)(lag < 0 ? (long)fine + lag : lag);
    for (size_t k = 0U; k < bins; ++k) {
      /* k * n mod fine keeps the angle exact; fine is a power of two. */
      const double angle =
          6.283185307179586 * (double)((k * n) & (fine - 1U)) / (double)fine;
      cosine[r * row + k] = (float)cos(angle);
      sine[r * row + k] = (float)sin(angle);
    }
  }
  free(fft->cosine);
  free(fft->sine);
  fft->cosine = cosine;
  fft->sine = sine;
  fft->phasor_re = re;
  fft->phasor_im = im;
  fft->max_lag = max_lag;
  fft->row = row;
  return AURIS_OK;
}

/* sum over k in [0, count) of re[k] * c[k] - im[k] * s[k], in float with
   sixteen partial sums. */
static float project(const float *re, const float *im, const float *c,
                     const float *s, size_t count) {
#if defined(__ARM_NEON)
  float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
  size_t k = 0U;
  for (; k + 8U <= count; k += 8U) {
    a0 = vfmaq_f32(a0, vld1q_f32(re + k), vld1q_f32(c + k));
    a1 = vfmsq_f32(a1, vld1q_f32(im + k), vld1q_f32(s + k));
    a2 = vfmaq_f32(a2, vld1q_f32(re + k + 4U), vld1q_f32(c + k + 4U));
    a3 = vfmsq_f32(a3, vld1q_f32(im + k + 4U), vld1q_f32(s + k + 4U));
  }
  float sum = vaddvq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)));
#else
  float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  size_t k = 0U;
  for (; k + 4U <= count; k += 4U) {
    for (size_t j = 0U; j < 4U; ++j) {
      p[j] += re[k + j] * c[k + j] - im[k + j] * s[k + j];
    }
  }
  float sum = (p[0] + p[1]) + (p[2] + p[3]);
#endif
  for (; k < count; ++k) {
    sum += re[k] * c[k] - im[k] * s[k];
  }
  return sum;
}

auris_status_t auris_gcc_phat_lags(auris_fft_t *fft, const auris_spectrum_t *s,
                                   size_t a, size_t b, const float *gains,
                                   size_t first_bin, size_t last_bin,
                                   const int *lags, size_t lag_count,
                                   float *values, bool *valid, float *weight) {
  if (values == NULL || valid == NULL || weight == NULL || lags == NULL ||
      lag_count == 0U || !pair_valid(fft, s, a, b) || first_bin > last_bin ||
      last_bin > fft->size / 2U || fft->cosine == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  const long reach = (long)fft->max_lag;
  for (size_t i = 0U; i < lag_count; ++i) {
    if ((long)lags[i] < -reach || (long)lags[i] > reach) {
      return AURIS_ERROR_INVALID_ARGUMENT;
    }
  }
  /* The same weighted phasors (and weight) as prepare_phat, kept densely
     between the first and last bins that carry any. */
  const float epsilon = 1.0e-12f;
  const size_t bins = fft->size / 2U + 1U;
  float *re = fft->phasor_re, *im = fft->phasor_im;
  size_t lowest = bins, highest = 0U;
  bool any = false;
  float total = 0.0f;
  for (size_t k = first_bin < 1U ? 1U : first_bin;
       k + 1U < bins && k <= last_bin; ++k) {
    const float ar = s->bins[a][k][0];
    const float ai = s->bins[a][k][1];
    const float br = s->bins[b][k][0];
    const float bi = s->bins[b][k][1];
    const float real = br * ar + bi * ai;
    const float imag = bi * ar - br * ai;
    const float magnitude = hypotf(real, imag);
    const float w = (s->weights == NULL ? 1.0f : s->weights[k]) *
                    (gains == NULL ? 1.0f : gains[k]);
    re[k] = w * real / (magnitude + epsilon);
    im[k] = w * imag / (magnitude + epsilon);
    if (magnitude > epsilon) {
      any = true;
      total += w;
    }
    if (re[k] != 0.0f || im[k] != 0.0f) {
      lowest = k < lowest ? k : lowest;
      highest = k;
    }
  }
  *valid = any;
  *weight = total;
  /* y[n] = 2 sum_k Re(X[k] e^(2 pi i k n / fine)), the c2r inverse of a
     spectrum that is zero at DC and Nyquist, divided by the frame length
     like auris_gcc_phat_curve_band. */
  const float scale = 2.0f / (float)fft->size;
  for (size_t i = 0U; i < lag_count; ++i) {
    float sum = 0.0f;
    if (highest >= lowest) {
      const size_t r = (size_t)((long)lags[i] + reach) * fft->row;
      sum = project(re + lowest, im + lowest, fft->cosine + r + lowest,
                    fft->sine + r + lowest, highest - lowest + 1U);
    }
    values[i] = sum * scale;
  }
  return AURIS_OK;
}
