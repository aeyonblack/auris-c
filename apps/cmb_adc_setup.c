#include "auris_i2c.h"
#include "auris_pcm186x.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTROLLER "i2c@2020000" /* MAIN I2C2: P9.19 SCL, P9.20 SDA */

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--bus /dev/i2c-N] [--gain DB] [--no-verify] "
          "[--dry-run] [--status]\n",
          program);
  return EXIT_FAILURE;
}

static const char *state_name(uint8_t state) {
  switch (state & 0x0FU) {
  case 0x0U: return "power down";
  case 0x1U: return "wait clock stable";
  case 0x2U: return "release reset";
  case 0x3U: return "stand-by";
  case 0x4U: return "fade in";
  case 0x5U: return "fade out";
  case 0x9U: return "sleep";
  case 0xFU: return "run";
  default: return "reserved";
  }
}

static const char *rate_name(uint8_t rate) {
  static const char *names[8] = {"halted/low", "8 kHz",      "16 kHz",
                                 "32-48 kHz",  "88.2-96 kHz", "176.4-192 kHz",
                                 "high",       "invalid"};
  return names[rate & 0x07U];
}

static unsigned int bck_ratio(uint8_t ratios) {
  static const unsigned int values[8] = {0U, 32U, 48U, 64U, 256U, 0U, 0U, 0U};
  return values[(ratios >> 4U) & 0x07U];
}

static unsigned int sck_ratio(uint8_t ratios) {
  static const unsigned int values[8] = {0U, 128U, 256U, 384U, 512U, 768U, 0U,
                                         0U};
  return values[ratios & 0x07U];
}

static int print_status(const auris_i2c_bus_t *bus) {
  const uint8_t chips[2] = {AURIS_CMB_ADC_MASTER, AURIS_CMB_ADC_SLAVE};
  const char *labels[2] = {"U1 master", "U2 slave"};
  int result = 0;
  printf("chip          state               rate         BCK/LRCK  SCK/LRCK  "
         "clock errors  power\n");
  for (size_t c = 0U; c < 2U; ++c) {
    auris_pcm186x_status_t s;
    const int err = auris_pcm186x_status(bus, chips[c], &s);
    if (err < 0) {
      printf("%-9s 0x%02X  unreadable: %s\n", labels[c], chips[c],
             strerror(-err));
      result = err;
      continue;
    }
    printf("%-9s 0x%02X  %-18s  %-11s  %8u  %8u  0x%02X          0x%02X\n",
           labels[c], chips[c], state_name(s.state), rate_name(s.rate),
           bck_ratio(s.ratios), sck_ratio(s.ratios), s.clock_errors, s.power);
  }
  return result;
}

int main(int argc, char **argv) {
  auris_cmb_adc_config_t config = auris_cmb_adc_defaults();
  const char *bus_path = NULL;
  bool dry_run = false, status_only = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--bus") == 0 && i + 1 < argc) {
      bus_path = argv[++i];
    } else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
      char *end = NULL;
      errno = 0;
      config.gain_db = strtof(argv[++i], &end);
      if (errno != 0 || end == argv[i] || *end != '\0') {
        return usage(argv[0]);
      }
    } else if (strcmp(argv[i], "--no-verify") == 0) {
      config.verify = false;
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      dry_run = true;
    } else if (strcmp(argv[i], "--status") == 0) {
      status_only = true;
    } else {
      return usage(argv[0]);
    }
  }

  auris_pcm186x_write_t writes[AURIS_CMB_ADC_WRITES];
  const int count = auris_cmb_adc_sequence(&config, writes, AURIS_CMB_ADC_WRITES);
  if (count < 0) {
    fprintf(stderr, "gain must be -12..40 dB in 0.5 dB steps\n");
    return EXIT_FAILURE;
  }
  if (dry_run) {
    printf("reset   0x%02X reg 0x00 <- 0xFF\n", AURIS_CMB_ADC_SLAVE);
    printf("reset   0x%02X reg 0x00 <- 0xFF\n", AURIS_CMB_ADC_MASTER);
    for (int i = 0; i < count; ++i) {
      printf("write   0x%02X reg 0x%02X <- 0x%02X  (read-back mask 0x%02X)\n",
             writes[i].address, writes[i].reg, writes[i].value, writes[i].mask);
    }
    return EXIT_SUCCESS;
  }

  char found[64];
  if (bus_path == NULL) {
    const int err =
        auris_i2c_find("/sys/class/i2c-adapter", CONTROLLER, found, sizeof(found));
    if (err < 0) {
      fprintf(stderr, "cannot find %s: %s (is BONE-I2C2.dtbo loaded?)\n",
              CONTROLLER, strerror(-err));
      return EXIT_FAILURE;
    }
    bus_path = found;
  }
  auris_i2c_dev_t dev = {.fd = -1};
  int err = auris_i2c_open(&dev, bus_path);
  if (err < 0) {
    fprintf(stderr, "open %s: %s\n", bus_path, strerror(-err));
    return EXIT_FAILURE;
  }
  const auris_i2c_bus_t bus = auris_i2c_bus(&dev);
  int result = EXIT_SUCCESS;

  if (status_only) {
    printf("bus %s\n", bus_path);
    result = print_status(&bus) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    auris_i2c_close(&dev);
    return result;
  }

  printf("bus %s, PGA gain %.1f dB, %s\n", bus_path, (double)config.gain_db,
         config.verify ? "verify on" : "verify off");
  auris_cmb_adc_report_t report;
  err = auris_cmb_adc_configure(&bus, &config, &report);
  if (config.verify && err != -ENODEV) {
    printf("Run after %u ms, gain ramp settled after %u ms, %zu mismatches\n",
           report.run_ms, report.ramp_ms, report.mismatches);
    if (report.mismatches != 0U) {
      printf("first mismatch: chip 0x%02X reg 0x%02X expected 0x%02X "
             "(mask 0x%02X), read 0x%02X\n",
             report.first_mismatch.address, report.first_mismatch.reg,
             report.first_mismatch.value, report.first_mismatch.mask,
             report.first_actual);
    }
    (void)print_status(&bus);
  }
  if (err < 0) {
    fprintf(stderr, "RESULT: FAIL (%s)\n", strerror(-err));
    result = EXIT_FAILURE;
  } else {
    printf("RESULT: OK\n");
  }
  auris_i2c_close(&dev);
  return result;
}
