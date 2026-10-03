#include "auris_pcm186x.h"

#include <errno.h>
#include <math.h>

#define RESET_SETTLE_MS 100U
#define POLL_MS 100U
#define RUN_POLLS 20U  /* up to 2 s for Run */
#define RAMP_POLLS 50U /* up to 5 s for the smoothed gain ramp */
#define GAIN 0x100U    /* marker: substitute the PGA gain code */

/* Page 0 register writes. value may be GAIN; mask 0 = no read-back. */
typedef struct {
  uint8_t reg;
  uint16_t value;
  uint8_t mask;
} step_t;

/* Settings shared by both chips (datasheet SLAS831C p.69-74). */
static const step_t COMMON[] = {
    {0x00U, 0x00U, 0x00U}, /* page 0 */
    {0x01U, GAIN, 0xFFU},  /* PGA ADC1L */
    {0x02U, GAIN, 0xFFU},  /* PGA ADC1R */
    {0x03U, GAIN, 0xFFU},  /* PGA ADC2L */
    {0x04U, GAIN, 0xFFU},  /* PGA ADC2R */
    {0x05U, 0x86U, 0xFFU}, /* smooth gain change, independent channels */
    {0x06U, 0x41U, 0xFFU}, /* ADC1L <- VINL1, single-ended */
    {0x07U, 0x41U, 0xFFU}, /* ADC1R <- VINR1 */
    {0x08U, 0x44U, 0xFFU}, /* ADC2L <- VINL3 */
    {0x09U, 0x44U, 0xFFU}, /* ADC2R <- VINR3 */
    {0x0AU, 0x00U, 0xFFU}, /* secondary ADC: no input */
    {0x0BU, 0x44U, 0xFFU}, /* 24-bit, I2S */
};

/* U2: clock slave. Its clocks arrive on the SCK/BCK/LRCK pins. */
static const step_t SLAVE[] = {
    {0x10U, 0x00U, 0xFFU}, /* GPIO0 = GPIO */
    {0x11U, 0x50U, 0xFFU}, /* GPIO3 = DOUT2 (DATA4) */
    {0x12U, 0x00U, 0xFFU}, /* GPIO0 input */
    {0x13U, 0x40U, 0xFFU}, /* GPIO3 output */
    {0x20U, 0x41U, 0xF1U}, /* SCK only, slave, clock detect on */
};

/* U1: clock master from the 24.576 MHz crystal. 0x20 starts the clocks,
   so it must be the final write of the whole sequence. */
static const step_t MASTER[] = {
    {0x10U, 0x03U, 0xFFU}, /* GPIO0 = SCK out (buffered to U2) */
    {0x11U, 0x50U, 0xFFU}, /* GPIO3 = DOUT2 (DATA2) */
    {0x12U, 0x04U, 0xFFU}, /* GPIO0 output */
    {0x13U, 0x40U, 0xFFU}, /* GPIO3 output */
    {0x26U, 0x07U, 0xFFU}, /* BCK = SCK / 8 = 3.072 MHz */
    {0x27U, 0x3FU, 0xFFU}, /* LRCK = BCK / 64 = 48 kHz */
    {0x20U, 0x91U, 0xF1U}, /* crystal only, master, clock detect on */
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

_Static_assert(2U * COUNT(COMMON) + COUNT(SLAVE) + COUNT(MASTER) ==
                   AURIS_CMB_ADC_WRITES,
               "sequence length must match the public constant");

auris_cmb_adc_config_t auris_cmb_adc_defaults(void) {
  return (auris_cmb_adc_config_t){.gain_db = 32.0F, .verify = true};
}

int auris_pcm186x_gain_code(float gain_db, uint8_t *code) {
  if (code == NULL || !isfinite(gain_db) || gain_db < -12.0F ||
      gain_db > 40.0F) {
    return -EINVAL;
  }
  const float halves = gain_db * 2.0F;
  if (halves != floorf(halves)) {
    return -EINVAL;
  }
  /* Two's complement in 8 bits: -1 dB = -2 half-steps = 0xFE. */
  *code = (uint8_t)((int)halves & 0xFF);
  return 0;
}

static size_t append(auris_pcm186x_write_t *out, size_t at, uint8_t address,
                     const step_t *steps, size_t count, uint8_t gain) {
  for (size_t i = 0U; i < count; ++i) {
    out[at + i] = (auris_pcm186x_write_t){
        .address = address,
        .reg = steps[i].reg,
        .value = steps[i].value == GAIN ? gain : (uint8_t)steps[i].value,
        .mask = steps[i].mask};
  }
  return at + count;
}

int auris_cmb_adc_sequence(const auris_cmb_adc_config_t *config,
                           auris_pcm186x_write_t *out, size_t capacity) {
  uint8_t gain;
  if (config == NULL || out == NULL ||
      auris_pcm186x_gain_code(config->gain_db, &gain) != 0) {
    return -EINVAL;
  }
  if (capacity < AURIS_CMB_ADC_WRITES) {
    return -ENOSPC;
  }
  size_t n = 0U;
  n = append(out, n, AURIS_CMB_ADC_SLAVE, COMMON, COUNT(COMMON), gain);
  n = append(out, n, AURIS_CMB_ADC_SLAVE, SLAVE, COUNT(SLAVE), gain);
  n = append(out, n, AURIS_CMB_ADC_MASTER, COMMON, COUNT(COMMON), gain);
  n = append(out, n, AURIS_CMB_ADC_MASTER, MASTER, COUNT(MASTER), gain);
  return (int)n;
}

int auris_pcm186x_status(const auris_i2c_bus_t *bus, uint8_t address,
                         auris_pcm186x_status_t *out) {
  if (bus == NULL || bus->read == NULL || out == NULL) {
    return -EINVAL;
  }
  const uint8_t regs[5] = {0x72U, 0x73U, 0x74U, 0x75U, 0x78U};
  uint8_t values[5];
  for (size_t i = 0U; i < 5U; ++i) {
    const int err = bus->read(bus->context, address, regs[i], &values[i]);
    if (err < 0) {
      return err;
    }
  }
  *out = (auris_pcm186x_status_t){values[0], values[1], values[2], values[3],
                                  values[4]};
  return 0;
}

static const uint8_t CHIPS[2] = {AURIS_CMB_ADC_MASTER, AURIS_CMB_ADC_SLAVE};

static int wait_for_run(const auris_i2c_bus_t *bus,
                        auris_cmb_adc_report_t *report) {
  for (unsigned int poll = 0U; poll < RUN_POLLS; ++poll) {
    bool running = true;
    for (size_t c = 0U; c < 2U; ++c) {
      const int err =
          bus->read(bus->context, CHIPS[c], 0x72U, &report->state[c]);
      if (err < 0) {
        return err;
      }
      running = running && (report->state[c] & 0x0FU) == AURIS_PCM186X_STATE_RUN;
    }
    if (running) {
      return 0;
    }
    bus->sleep_ms(bus->context, POLL_MS);
    report->run_ms += POLL_MS;
  }
  return -ETIMEDOUT;
}

/* With smoothing on, 0x01-0x04 read back the ramping gain, not the
   target, until the ramp finishes. A ramp that never settles is left
   for the read-back to report as a mismatch. */
static int wait_for_ramp(const auris_i2c_bus_t *bus, uint8_t gain,
                         auris_cmb_adc_report_t *report) {
  for (unsigned int poll = 0U; poll < RAMP_POLLS; ++poll) {
    bool settled = true;
    for (size_t c = 0U; c < 2U; ++c) {
      for (uint8_t reg = 0x01U; reg <= 0x04U; ++reg) {
        uint8_t value;
        const int err = bus->read(bus->context, CHIPS[c], reg, &value);
        if (err < 0) {
          return err;
        }
        settled = settled && value == gain;
      }
    }
    if (settled) {
      return 0;
    }
    bus->sleep_ms(bus->context, POLL_MS);
    report->ramp_ms += POLL_MS;
  }
  return 0;
}

int auris_cmb_adc_configure(const auris_i2c_bus_t *bus,
                            const auris_cmb_adc_config_t *config,
                            auris_cmb_adc_report_t *report) {
  auris_cmb_adc_report_t local;
  auris_cmb_adc_report_t *r = report != NULL ? report : &local;
  *r = (auris_cmb_adc_report_t){0};
  auris_pcm186x_write_t writes[AURIS_CMB_ADC_WRITES];
  if (bus == NULL || bus->write == NULL || bus->read == NULL ||
      bus->sleep_ms == NULL || config == NULL) {
    return -EINVAL;
  }
  const int count = auris_cmb_adc_sequence(config, writes, AURIS_CMB_ADC_WRITES);
  if (count < 0) {
    return count;
  }

  /* 1. Both chips must answer before anything is changed. */
  for (size_t c = 0U; c < 2U; ++c) {
    uint8_t page;
    if (bus->read(bus->context, CHIPS[c], 0x00U, &page) < 0) {
      return -ENODEV;
    }
  }

  /* 2. Register reset. The chip may NACK its own reset write. */
  (void)bus->write(bus->context, AURIS_CMB_ADC_SLAVE, 0x00U, 0xFFU);
  (void)bus->write(bus->context, AURIS_CMB_ADC_MASTER, 0x00U, 0xFFU);
  bus->sleep_ms(bus->context, RESET_SETTLE_MS);

  /* 3. Configuration; the last write starts the clocks. */
  for (int i = 0; i < count; ++i) {
    const int err = bus->write(bus->context, writes[i].address, writes[i].reg,
                               writes[i].value);
    if (err < 0) {
      return err;
    }
  }
  if (!config->verify) {
    return 0;
  }

  /* 4. Run state, gain ramp, then read every written register back. */
  int err = wait_for_run(bus, r);
  if (err < 0) {
    return err;
  }
  err = wait_for_ramp(bus, writes[1].value, r);
  if (err < 0) {
    return err;
  }
  for (int i = 0; i < count; ++i) {
    if (writes[i].mask == 0U) {
      continue;
    }
    uint8_t actual;
    err = bus->read(bus->context, writes[i].address, writes[i].reg, &actual);
    if (err < 0) {
      return err;
    }
    if ((actual & writes[i].mask) != (writes[i].value & writes[i].mask)) {
      if (r->mismatches == 0U) {
        r->first_mismatch = writes[i];
        r->first_actual = actual;
      }
      ++r->mismatches;
    }
  }
  return r->mismatches == 0U ? 0 : -EPROTO;
}
