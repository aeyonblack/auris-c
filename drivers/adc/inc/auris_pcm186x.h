#ifndef AURIS_PCM186X_H
#define AURIS_PCM186X_H

#include "auris_i2c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AURIS_CMB_ADC_MASTER 0x4AU /* U1: crystal, clock master */
#define AURIS_CMB_ADC_SLAVE 0x4BU  /* U2: clock slave */
#define AURIS_CMB_ADC_WRITES 36U   /* configuration writes, excluding reset */
#define AURIS_PCM186X_STATE_RUN 0x0FU

typedef struct {
  float gain_db; /* PGA gain for all eight channels, -12..40 in 0.5 dB steps */
  bool verify;   /* wait for Run, wait for the gain ramp, read back */
} auris_cmb_adc_config_t;

/* One register write. On read-back only the bits in `mask` are compared;
   mask 0 means "do not read back" (page selects). */
typedef struct {
  uint8_t address, reg, value, mask;
} auris_pcm186x_write_t;

typedef struct {
  uint8_t state[2]; /* Page 0 0x72: [0] master U1, [1] slave U2 */
  unsigned int run_ms, ramp_ms;
  size_t mismatches;
  auris_pcm186x_write_t first_mismatch; /* .value is the expected value */
  uint8_t first_actual;
} auris_cmb_adc_report_t;

/* Raw Page 0 status registers 0x72-0x75 and 0x78. */
typedef struct {
  uint8_t state, rate, ratios, clock_errors, power;
} auris_pcm186x_status_t;

auris_cmb_adc_config_t auris_cmb_adc_defaults(void);
int auris_pcm186x_gain_code(float gain_db, uint8_t *code);
/* Fills `out` with the ordered configuration writes: slave first, master
   last, and the master's clock-start write as the final entry. Returns
   the number of writes or a negative errno value. */
int auris_cmb_adc_sequence(const auris_cmb_adc_config_t *config,
                           auris_pcm186x_write_t *out, size_t capacity);
/* Probes, resets and configures both ADCs. `report` may be NULL. */
int auris_cmb_adc_configure(const auris_i2c_bus_t *bus,
                            const auris_cmb_adc_config_t *config,
                            auris_cmb_adc_report_t *report);
int auris_pcm186x_status(const auris_i2c_bus_t *bus, uint8_t address,
                         auris_pcm186x_status_t *out);

#endif
