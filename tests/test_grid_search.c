#define _POSIX_C_SOURCE 200809L
#include "auris_srp.h"
#include "auris_view.h"
#include "auris_array_configs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef AURIS_SRP_GRID_SEARCH
int main(void) {
  puts("Module 9 core implementation not installed yet");
  return 77;
}
#else
#define COLS 65U
#define ROWS 49U
#define COUNT (COLS * ROWS)
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "line %d: %s\n", __LINE__, #x); goto done; } } while (0)

static const auris_view_t view = {
  .width=640U, .height=480U, .fx=480.0f, .fy=480.0f,
  .cx=319.5f, .cy=239.5f,
  .rotation={{1,0,0},{0,1,0},{0,0,1}}
};

static void make_spectrum(const auris_array_t *array, float x, float y,
                          auris_spectrum_t *spectrum) {
  auris_vec3f_t ray;
  if (auris_view_ray(&view, x / 64.0f * 639.0f,
                    y / 48.0f * 479.0f, &ray) != AURIS_OK) {
    abort();
  }
  *spectrum = (auris_spectrum_t){.size = AURIS_ANALYSIS_SIZE, .sample_rate=16000U, .channels=4U};
  for (size_t m=0U; m<4U; ++m) {
    const auris_vec3f_t p = array->positions[m];
    const float advance = 16000.0f / array->speed_of_sound_mps *
        (p.x_m*ray.x_m + p.y_m*ray.y_m + p.z_m*ray.z_m);
    for (size_t k=10U; k<=128U; ++k) {
      const float phase = 6.283185307179586f*(float)k*advance/512.0f;
      spectrum->bins[m][k][0] = cosf(phase);
      spectrum->bins[m][k][1] = sinf(phase);
    }
  }
}

static double seconds(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec*1e-9;
}

int main(void) {
  int result=EXIT_FAILURE;
  const auris_array_t *array=auris_config_respeaker_usb_4();
  auris_vec3f_t grid[COUNT];
  auris_fft_t *fft=auris_fft_create();
  auris_srp_t *dense=NULL, *fast=NULL;
  auris_srp_result_t a, b;
  auris_spectrum_t spectrum;
  CHECK(auris_view_grid(&view,COLS,ROWS,grid,COUNT)==AURIS_OK);
  dense=auris_srp_create(array,16000U,grid,COUNT);
  fast=auris_srp_create(array,16000U,grid,COUNT);
  CHECK(fft && dense && fast);
  CHECK(auris_srp_set_grid(NULL,COLS,ROWS,4U)==AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_grid(fast,COLS,ROWS,0U)==AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_grid(fast,COLS,ROWS,3U)==AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_grid(fast,SIZE_MAX,ROWS,4U)==AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_grid(fast,COLS,ROWS-1U,4U)==AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_srp_set_grid(dense,COLS,ROWS,1U)==AURIS_OK);
  CHECK(auris_srp_set_grid(fast,COLS,ROWS,4U)==AURIS_OK);
  /* Include corners, borders, between-anchor and fractional directions. */
  const float positions[][2]={{0,0},{64,0},{0,48},{64,48},{32,24},
      {2,2},{62,46},{31.4f,23.6f},{46.8f,15.2f},{17.2f,38.6f}};
  for (size_t t=0U;t<sizeof positions/sizeof positions[0];++t) {
    make_spectrum(array,positions[t][0],positions[t][1],&spectrum);
    CHECK(auris_srp_apply(dense,fft,&spectrum,&a)==AURIS_OK);
    CHECK(auris_srp_apply(fast,fft,&spectrum,&b)==AURIS_OK);
    CHECK(a.valid && b.valid && !a.approximate && b.approximate);
    CHECK(a.evaluated==COUNT && b.evaluated>=221U && b.evaluated<=293U);
    CHECK(a.best==b.best && fabsf(a.peak-b.peak)<1e-6f);
    printf("target (%.1f, %.1f), dense (%zu, %zu), hierarchy (%zu, %zu)\n",
        (double)positions[t][0],(double)positions[t][1],
        a.best%COLS,a.best/COLS,b.best%COLS,b.best/COLS);
    auris_vec3f_t truth;
    CHECK(auris_view_ray(&view,positions[t][0]/64.0f*639.0f,
                        positions[t][1]/48.0f*479.0f,&truth)==AURIS_OK);
    const auris_vec3f_t found=grid[b.best];
    const float dot=truth.x_m*found.x_m+truth.y_m*found.y_m+truth.z_m*found.z_m;
    CHECK(acosf(fminf(1.0f,fmaxf(-1.0f,dot)))*57.295779513f < 3.0f);
    for (size_t q=0U;q<COUNT;++q) {
      CHECK(isfinite(b.scores[q]) && b.scores[q]<=b.peak+1e-6f);
      if (q%COLS%4U==0U && q/COLS%4U==0U) {
        CHECK(fabsf(a.scores[q]-b.scores[q])<1e-6f);
      }
    }
    float total=0;
    for(size_t p=0U;p<b.pairs;++p) total+=b.contributions[p];
    CHECK(fabsf(total/(float)b.pairs-b.peak)<1e-6f);
  }
  CHECK(auris_srp_set_smoothing(dense,.1f)==AURIS_OK);
  CHECK(auris_srp_set_smoothing(fast,.1f)==AURIS_OK);
  for(size_t t=0U;t<12U;++t) {
    make_spectrum(array,27.0f+(float)t*.7f,21.0f,&spectrum);
    spectrum.first_frame=(uint64_t)t*AURIS_ANALYSIS_HOP;
    CHECK(auris_srp_apply(dense,fft,&spectrum,&a)==AURIS_OK);
    CHECK(auris_srp_apply(fast,fft,&spectrum,&b)==AURIS_OK);
    CHECK(a.best==b.best);
  }
  spectrum=(auris_spectrum_t){.size = AURIS_ANALYSIS_SIZE, .sample_rate=16000U,.channels=4U};
  auris_srp_reset(dense); auris_srp_reset(fast);
  CHECK(auris_srp_apply(fast,fft,&spectrum,&b)==AURIS_OK);
  CHECK(!b.valid && b.best==0U && b.peak==0.0f);
  /* Representative isolated SRP timing, not end-to-end capture latency. */
  make_spectrum(array,46.8f,15.2f,&spectrum);
  CHECK(auris_srp_set_smoothing(dense,0)==AURIS_OK);
  CHECK(auris_srp_set_smoothing(fast,0)==AURIS_OK);
  double elapsed[2];
  for(size_t mode=0U;mode<2U;++mode) {
    auris_srp_t *s=mode?fast:dense;
    for(size_t n=0U;n<20U;++n) CHECK(auris_srp_apply(s,fft,&spectrum,&b)==AURIS_OK);
    const double start=seconds();
    for(size_t n=0U;n<500U;++n) CHECK(auris_srp_apply(s,fft,&spectrum,&b)==AURIS_OK);
    elapsed[mode]=(seconds()-start)*1000.0/500.0;
  }
  printf("SRP mean ms: exhaustive %.3f, hierarchical %.3f (500 calls each)\n",
         elapsed[0],elapsed[1]);
  puts("Grid search checks passed");
  result=EXIT_SUCCESS;
done:
  auris_srp_destroy(dense); auris_srp_destroy(fast); auris_fft_destroy(fft);
  return result;
}
#endif
