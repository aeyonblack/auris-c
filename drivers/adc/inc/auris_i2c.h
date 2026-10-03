#ifndef AURIS_I2C_H
#define AURIS_I2C_H

#include <stddef.h>
#include <stdint.h>

/* Register access to a 7-bit I2C device with 8-bit register addresses.
   Every function returns 0 or a negative errno value. sleep_ms lets
   drivers wait without depending on a particular clock, so tests can
   supply a bus that never really sleeps. */
typedef struct {
  void *context;
  int (*write)(void *context, uint8_t address, uint8_t reg, uint8_t value);
  int (*read)(void *context, uint8_t address, uint8_t reg, uint8_t *value);
  void (*sleep_ms)(void *context, unsigned int milliseconds);
} auris_i2c_bus_t;

/* Linux /dev/i2c-N adapter. Initialize with {.fd = -1}. */
typedef struct {
  int fd;
} auris_i2c_dev_t;

/* Find the /dev/i2c-N node whose device-tree node name is `controller`,
   for example "i2c@2020000", by scanning `sysfs_root` (normally
   "/sys/class/i2c-adapter"). Writes the path into `path`. */
int auris_i2c_find(const char *sysfs_root, const char *controller, char *path,
                   size_t capacity);
int auris_i2c_open(auris_i2c_dev_t *dev, const char *path);
void auris_i2c_close(auris_i2c_dev_t *dev);
/* A bus view of an open adapter. The adapter must outlive the view. */
auris_i2c_bus_t auris_i2c_bus(auris_i2c_dev_t *dev);

#endif
