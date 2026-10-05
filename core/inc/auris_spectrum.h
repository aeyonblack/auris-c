#ifndef AURIS_SPECTRUM_H
#define AURIS_SPECTRUM_H

#include "auris_analysis.h"
#include "auris_status.h"
#include <stddef.h>
#include <stdint.h>

/* Defaults: the 512-sample frame and 8x GCC refinement of the original
   ReSpeaker pipeline. */
#define AURIS_FFT_BINS (AURIS_ANALYSIS_SIZE / 2U + 1U)
#define AURIS_GCC_UPSAMPLE 8U
#define AURIS_GCC_SIZE (AURIS_ANALYSIS_SIZE * AURIS_GCC_UPSAMPLE)

/* Capacities for transforms sized at run time. */
#define AURIS_FFT_MAX_BINS (AURIS_ANALYSIS_MAX_SIZE / 2U + 1U)
#define AURIS_GCC_MAX_UPSAMPLE 8U
#define AURIS_GCC_MAX_SIZE (AURIS_ANALYSIS_MAX_SIZE * AURIS_GCC_MAX_UPSAMPLE)

typedef struct auris_fft auris_fft_t;

typedef struct {
  unsigned int sample_rate;
  size_t channels;
  size_t size; /* frame length; size / 2 + 1 bins are valid */
  uint64_t first_frame;
  /* Optional PHAT weight per bin (0..1), size / 2 + 1 values, set by the
     activity gate. NULL gives every bin with signal a weight of one. */
  const float *weights;
  float bins[AURIS_AUDIO_CHANNELS][AURIS_FFT_MAX_BINS][2];
} auris_spectrum_t;

typedef struct {
  size_t size; /* valid correlation values (the frame length) */
  float correlation[AURIS_ANALYSIS_MAX_SIZE];
  int lag_samples;
  float peak;
  bool valid;
} auris_gcc_t;

/* Transforms for AURIS_ANALYSIS_SIZE frames, refined AURIS_GCC_UPSAMPLE x. */
auris_fft_t *auris_fft_create(void);
/* Transforms for `size`-sample frames (a valid analysis size) whose fine
   GCC curves have size * upsample points (upsample 1, 2, 4 or 8). */
auris_fft_t *auris_fft_create_size(size_t size, size_t upsample);
void auris_fft_destroy(auris_fft_t *fft);
size_t auris_fft_size(const auris_fft_t *fft);
size_t auris_fft_upsample(const auris_fft_t *fft);
/* size * upsample: the number of floats a fine curve needs. */
size_t auris_fft_curve_size(const auris_fft_t *fft);

/* The frame's size must equal the transform's size. */
auris_status_t auris_spectrum_apply(auris_fft_t *fft,
                                    const auris_analysis_frame_t *frame,
                                    auris_spectrum_t *out);
/* Zeroes every bin outside [low_hz, high_hz], and DC and Nyquist, in every
   channel, so PHAT only gives votes to bins that can carry the source. */
auris_status_t auris_spectrum_band(auris_spectrum_t *spectrum, float low_hz,
                                   float high_hz);
/* Power of one channel over the bins in [low_hz, high_hz] (DC and Nyquist
   excluded), corrected for the window: a sine of amplitude A reads A*A/2.
   window_energy is the sum of the squared window samples. */
auris_status_t auris_spectrum_band_power(const auris_spectrum_t *spectrum,
                                         size_t channel, float low_hz,
                                         float high_hz, float window_energy,
                                         float *power);
/* Circular GCC-PHAT at integer lags; positive lag means b is later than a.
   |max_lag| must be below size / 2. */
auris_status_t auris_gcc_phat(auris_fft_t *fft,
                              const auris_spectrum_t *spectrum, size_t a,
                              size_t b, int max_lag, auris_gcc_t *out);
/* The same correlation sampled auris_fft_upsample(fft) times per sample.
   `curve` must hold auris_fft_curve_size(fft) floats. */
auris_status_t auris_gcc_phat_curve(auris_fft_t *fft,
                                    const auris_spectrum_t *spectrum, size_t a,
                                    size_t b, float *curve, bool *valid);
/* As auris_gcc_phat_curve. Also returns the total weight of the bins that
   voted: a pair whose weighted bins all agree peaks at 2 * weight / size. */
auris_status_t auris_gcc_phat_curve_weighted(auris_fft_t *fft,
                                             const auris_spectrum_t *spectrum,
                                             size_t a, size_t b, float *curve,
                                             bool *valid, float *weight);
/* As auris_gcc_phat_curve_weighted, with a further gain per bin for this
   pair: size / 2 + 1 values that multiply the spectrum's weights. NULL
   gives every bin a gain of one. */
auris_status_t auris_gcc_phat_curve_gained(auris_fft_t *fft,
                                           const auris_spectrum_t *spectrum,
                                           size_t a, size_t b,
                                           const float *gains, float *curve,
                                           bool *valid, float *weight);
/* As auris_gcc_phat_curve_gained, from bins first_bin..last_bin only (DC
   and Nyquist are never used). first_bin <= last_bin <= size / 2. */
auris_status_t auris_gcc_phat_curve_band(auris_fft_t *fft,
                                         const auris_spectrum_t *spectrum,
                                         size_t a, size_t b, const float *gains,
                                         size_t first_bin, size_t last_bin,
                                         float *curve, bool *valid,
                                         float *weight);

#endif
