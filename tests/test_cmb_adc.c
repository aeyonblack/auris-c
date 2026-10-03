#include "auris_i2c.h"
#include "auris_pcm186x.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

/* A simulated pair of PCM1865s. Index 0 = master (0x4A), 1 = slave (0x4B). */
typedef struct {
  uint8_t regs[2][256];
  bool present[2];
  bool clocks;              /* master 0x20 written with the master bit */
  unsigned int run_after;   /* 0x72 reads before Run is reported */
  unsigned int ramp_reads;  /* PGA reads that return 0 after clocks start */
  unsigned int state_reads, slept_ms, reads_after_clock, writes;
  int fail_reg;             /* write to this master register fails, or -1 */
  int corrupt_reg;          /* slave register that reads back wrong, or -1 */
  struct { uint8_t address, reg, value; } log[128];
  size_t logged;
} fake_t;

static int chip(uint8_t address) {
  return address == AURIS_CMB_ADC_MASTER ? 0
         : address == AURIS_CMB_ADC_SLAVE ? 1 : -1;
}

static int fake_write(void *context, uint8_t address, uint8_t reg,
                      uint8_t value) {
  fake_t *f = context;
  const int c = chip(address);
  if (f->logged < 128U) {
    f->log[f->logged].address = address;
    f->log[f->logged].reg = reg;
    f->log[f->logged].value = value;
    ++f->logged;
  }
  ++f->writes;
  if (c < 0 || !f->present[c]) return -ENXIO;
  if (reg == 0x00U && value == 0xFFU) { /* reset: clear, then NACK */
    memset(f->regs[c], 0, sizeof(f->regs[c]));
    if (c == 0) f->clocks = false;
    return -EIO;
  }
  if (c == 0 && (int)reg == f->fail_reg) return -EIO;
  f->regs[c][reg] = value;
  if (c == 0 && reg == 0x20U && (value & 0x10U) != 0U) {
    f->clocks = true;
    f->regs[0][0x20] = (uint8_t)(value | 0x02U); /* clock detector bit */
  }
  return 0;
}

static int fake_read(void *context, uint8_t address, uint8_t reg,
                     uint8_t *value) {
  fake_t *f = context;
  const int c = chip(address);
  if (c < 0 || !f->present[c]) return -ENXIO;
  if (f->clocks) ++f->reads_after_clock;
  if (reg == 0x72U) {
    ++f->state_reads;
    *value = f->clocks && f->state_reads > f->run_after * 2U ? 0x0FU : 0x01U;
    return 0;
  }
  if (reg >= 0x01U && reg <= 0x04U && f->clocks && f->ramp_reads > 0U) {
    --f->ramp_reads;
    *value = 0x00U;
    return 0;
  }
  *value = f->regs[c][reg];
  if (c == 1 && (int)reg == f->corrupt_reg) *value ^= 0x01U;
  return 0;
}

static void fake_sleep(void *context, unsigned int ms) {
  ((fake_t *)context)->slept_ms += ms;
}

static void fake_reset(fake_t *f) {
  memset(f, 0, sizeof(*f));
  f->present[0] = f->present[1] = true;
  f->fail_reg = -1;
  f->corrupt_reg = -1;
}

static int test_gain(void) {
  uint8_t code = 0U;
  CHECK(auris_pcm186x_gain_code(32.0F, &code) == 0 && code == 0x40U);
  CHECK(auris_pcm186x_gain_code(0.0F, &code) == 0 && code == 0x00U);
  CHECK(auris_pcm186x_gain_code(0.5F, &code) == 0 && code == 0x01U);
  CHECK(auris_pcm186x_gain_code(-1.0F, &code) == 0 && code == 0xFEU);
  CHECK(auris_pcm186x_gain_code(-0.5F, &code) == 0 && code == 0xFFU);
  CHECK(auris_pcm186x_gain_code(-12.0F, &code) == 0 && code == 0xE8U);
  CHECK(auris_pcm186x_gain_code(40.0F, &code) == 0 && code == 0x50U);
  CHECK(auris_pcm186x_gain_code(20.0F, &code) == 0 && code == 0x28U);
  CHECK(auris_pcm186x_gain_code(40.5F, &code) == -EINVAL);
  CHECK(auris_pcm186x_gain_code(-12.5F, &code) == -EINVAL);
  CHECK(auris_pcm186x_gain_code(32.25F, &code) == -EINVAL);
  CHECK(auris_pcm186x_gain_code(NAN, &code) == -EINVAL);
  CHECK(auris_pcm186x_gain_code(32.0F, NULL) == -EINVAL);
  return EXIT_SUCCESS;
}

static bool has(const auris_pcm186x_write_t *w, int n, size_t from, size_t to,
                uint8_t address, uint8_t reg, uint8_t value) {
  for (size_t i = from; i < to && i < (size_t)n; ++i) {
    if (w[i].address == address && w[i].reg == reg && w[i].value == value)
      return true;
  }
  return false;
}

static int test_sequence(void) {
  auris_cmb_adc_config_t config = auris_cmb_adc_defaults();
  CHECK(config.gain_db == 32.0F && config.verify);
  auris_pcm186x_write_t w[AURIS_CMB_ADC_WRITES];
  const int n = auris_cmb_adc_sequence(&config, w, AURIS_CMB_ADC_WRITES);
  CHECK(n == (int)AURIS_CMB_ADC_WRITES);
  CHECK(auris_cmb_adc_sequence(&config, w, AURIS_CMB_ADC_WRITES - 1U) ==
        -ENOSPC);

  /* Slave block first, master block second, clock start last. */
  size_t split = 0U;
  while (split < (size_t)n && w[split].address == AURIS_CMB_ADC_SLAVE) ++split;
  CHECK(split == 17U);
  for (size_t i = split; i < (size_t)n; ++i)
    CHECK(w[i].address == AURIS_CMB_ADC_MASTER);
  CHECK(w[0].reg == 0x00U && w[0].value == 0x00U && w[0].mask == 0x00U);
  CHECK(w[split].reg == 0x00U && w[split].value == 0x00U);
  CHECK(w[n - 1].reg == 0x20U && w[n - 1].value == 0x91U &&
        w[n - 1].mask == 0xF1U);
  CHECK(w[split - 1U].reg == 0x20U && w[split - 1U].value == 0x41U &&
        w[split - 1U].mask == 0xF1U);
  for (int i = 0; i < n - 1; ++i)
    CHECK(!(w[i].address == AURIS_CMB_ADC_MASTER && w[i].reg == 0x20U));

  /* Shared settings on both chips. */
  const uint8_t chips[2] = {AURIS_CMB_ADC_SLAVE, AURIS_CMB_ADC_MASTER};
  for (size_t c = 0U; c < 2U; ++c) {
    const size_t from = c == 0U ? 0U : split, to = c == 0U ? split : (size_t)n;
    for (uint8_t r = 0x01U; r <= 0x04U; ++r)
      CHECK(has(w, n, from, to, chips[c], r, 0x40U));
    CHECK(has(w, n, from, to, chips[c], 0x05U, 0x86U));
    CHECK(has(w, n, from, to, chips[c], 0x06U, 0x41U));
    CHECK(has(w, n, from, to, chips[c], 0x07U, 0x41U));
    CHECK(has(w, n, from, to, chips[c], 0x08U, 0x44U));
    CHECK(has(w, n, from, to, chips[c], 0x09U, 0x44U));
    CHECK(has(w, n, from, to, chips[c], 0x0AU, 0x00U));
    CHECK(has(w, n, from, to, chips[c], 0x0BU, 0x44U));
    CHECK(has(w, n, from, to, chips[c], 0x11U, 0x50U));
    CHECK(has(w, n, from, to, chips[c], 0x13U, 0x40U));
  }
  /* Chip-specific: GPIO0 and clock dividers. */
  CHECK(has(w, n, 0U, split, AURIS_CMB_ADC_SLAVE, 0x10U, 0x00U));
  CHECK(has(w, n, 0U, split, AURIS_CMB_ADC_SLAVE, 0x12U, 0x00U));
  CHECK(has(w, n, split, (size_t)n, AURIS_CMB_ADC_MASTER, 0x10U, 0x03U));
  CHECK(has(w, n, split, (size_t)n, AURIS_CMB_ADC_MASTER, 0x12U, 0x04U));
  CHECK(has(w, n, split, (size_t)n, AURIS_CMB_ADC_MASTER, 0x26U, 0x07U));
  CHECK(has(w, n, split, (size_t)n, AURIS_CMB_ADC_MASTER, 0x27U, 0x3FU));
  CHECK(!has(w, n, 0U, split, AURIS_CMB_ADC_SLAVE, 0x26U, 0x07U));

  config.gain_db = 20.0F;
  CHECK(auris_cmb_adc_sequence(&config, w, AURIS_CMB_ADC_WRITES) == n);
  CHECK(has(w, n, 0U, (size_t)n, AURIS_CMB_ADC_MASTER, 0x01U, 0x28U));
  config.gain_db = 41.0F;
  CHECK(auris_cmb_adc_sequence(&config, w, AURIS_CMB_ADC_WRITES) == -EINVAL);
  CHECK(auris_cmb_adc_sequence(NULL, w, AURIS_CMB_ADC_WRITES) == -EINVAL);
  return EXIT_SUCCESS;
}

static int test_configure(void) {
  fake_t f;
  auris_i2c_bus_t bus = {&f, fake_write, fake_read, fake_sleep};
  auris_cmb_adc_config_t config = auris_cmb_adc_defaults();
  auris_cmb_adc_report_t report;
  auris_pcm186x_write_t w[AURIS_CMB_ADC_WRITES];
  CHECK(auris_cmb_adc_sequence(&config, w, AURIS_CMB_ADC_WRITES) ==
        (int)AURIS_CMB_ADC_WRITES);

  /* Normal bring-up, including a NACKed reset and a short gain ramp. */
  fake_reset(&f);
  f.run_after = 3U;
  f.ramp_reads = 5U;
  CHECK(auris_cmb_adc_configure(&bus, &config, &report) == 0);
  CHECK(report.state[0] == 0x0FU && report.state[1] == 0x0FU);
  CHECK(report.mismatches == 0U);
  CHECK(report.run_ms == 300U && report.ramp_ms >= 100U);
  CHECK(f.slept_ms >= 100U + report.run_ms);
  CHECK(f.logged == 2U + AURIS_CMB_ADC_WRITES);
  CHECK(f.log[0].address == AURIS_CMB_ADC_SLAVE && f.log[0].reg == 0x00U &&
        f.log[0].value == 0xFFU);
  CHECK(f.log[1].address == AURIS_CMB_ADC_MASTER && f.log[1].reg == 0x00U &&
        f.log[1].value == 0xFFU);
  for (size_t i = 0U; i < AURIS_CMB_ADC_WRITES; ++i) {
    CHECK(f.log[2U + i].address == w[i].address &&
          f.log[2U + i].reg == w[i].reg && f.log[2U + i].value == w[i].value);
  }
  CHECK(f.regs[0][0x20] == 0x93U); /* masked read-back accepted 0x93 */

  /* Without verification nothing is read once the clocks start. */
  fake_reset(&f);
  config.verify = false;
  CHECK(auris_cmb_adc_configure(&bus, &config, NULL) == 0);
  CHECK(f.reads_after_clock == 0U && f.clocks);
  config.verify = true;

  /* A missing chip is reported before anything is written. */
  fake_reset(&f);
  f.present[1] = false;
  CHECK(auris_cmb_adc_configure(&bus, &config, &report) == -ENODEV);
  CHECK(f.writes == 0U);

  /* A failed configuration write stops before the clocks start. */
  fake_reset(&f);
  f.fail_reg = 0x26;
  CHECK(auris_cmb_adc_configure(&bus, &config, &report) == -EIO);
  CHECK(!f.clocks);

  /* No Run state within two seconds. */
  fake_reset(&f);
  f.run_after = 1000U;
  CHECK(auris_cmb_adc_configure(&bus, &config, &report) == -ETIMEDOUT);
  CHECK(report.state[0] == 0x01U);

  /* A register that reads back wrong is reported, with its location. */
  fake_reset(&f);
  f.corrupt_reg = 0x0B;
  CHECK(auris_cmb_adc_configure(&bus, &config, &report) == -EPROTO);
  CHECK(report.mismatches == 1U);
  CHECK(report.first_mismatch.address == AURIS_CMB_ADC_SLAVE &&
        report.first_mismatch.reg == 0x0BU &&
        report.first_mismatch.value == 0x44U && report.first_actual == 0x45U);

  /* Invalid arguments. */
  CHECK(auris_cmb_adc_configure(NULL, &config, &report) == -EINVAL);
  CHECK(auris_cmb_adc_configure(&bus, NULL, &report) == -EINVAL);
  config.gain_db = 100.0F;
  CHECK(auris_cmb_adc_configure(&bus, &config, &report) == -EINVAL);
  return EXIT_SUCCESS;
}

static int test_status(void) {
  fake_t f;
  fake_reset(&f);
  auris_i2c_bus_t bus = {&f, fake_write, fake_read, fake_sleep};
  f.clocks = true;
  f.run_after = 0U;
  f.regs[0][0x73] = 0x03U;
  f.regs[0][0x74] = 0x34U;
  f.regs[0][0x75] = 0x00U;
  f.regs[0][0x78] = 0x07U;
  auris_pcm186x_status_t s;
  CHECK(auris_pcm186x_status(&bus, AURIS_CMB_ADC_MASTER, &s) == 0);
  CHECK(s.state == 0x0FU && s.rate == 0x03U && s.ratios == 0x34U &&
        s.clock_errors == 0x00U && s.power == 0x07U);
  f.present[0] = false;
  CHECK(auris_pcm186x_status(&bus, AURIS_CMB_ADC_MASTER, &s) == -ENXIO);
  CHECK(auris_pcm186x_status(&bus, AURIS_CMB_ADC_MASTER, NULL) == -EINVAL);
  return EXIT_SUCCESS;
}

static int test_find(void) {
  char root[] = "/tmp/auris_i2c_testXXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char a[64], b[64], node[64], link[96], path[64];
  (void)snprintf(a, sizeof(a), "%s/i2c-4", root);
  (void)snprintf(b, sizeof(b), "%s/i2c-5", root);
  CHECK(mkdir(a, 0700) == 0 && mkdir(b, 0700) == 0);
  (void)snprintf(node, sizeof(node), "%s/i2c@2020000", root);
  CHECK(mkdir(node, 0700) == 0);
  (void)snprintf(link, sizeof(link), "%s/of_node", b);
  CHECK(symlink(node, link) == 0);
  CHECK(auris_i2c_find(root, "i2c@2020000", path, sizeof(path)) == 0);
  CHECK(strcmp(path, "/dev/i2c-5") == 0);
  CHECK(auris_i2c_find(root, "i2c@2030000", path, sizeof(path)) == -ENODEV);
  CHECK(auris_i2c_find(root, "i2c@2020000", path, 5U) == -ENAMETOOLONG);
  CHECK(auris_i2c_find(NULL, "i2c@2020000", path, sizeof(path)) == -EINVAL);
  (void)unlink(link);
  (void)rmdir(node);
  (void)rmdir(a);
  (void)rmdir(b);
  (void)rmdir(root);

  auris_i2c_dev_t dev = {.fd = -1};
  CHECK(auris_i2c_open(&dev, "/nonexistent/i2c-99") < 0 && dev.fd == -1);
  auris_i2c_close(&dev);
  return EXIT_SUCCESS;
}

int main(void) {
  if (test_gain() != EXIT_SUCCESS || test_sequence() != EXIT_SUCCESS ||
      test_configure() != EXIT_SUCCESS || test_status() != EXIT_SUCCESS ||
      test_find() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  puts("CMB ADC tests: ok");
  return EXIT_SUCCESS;
}
