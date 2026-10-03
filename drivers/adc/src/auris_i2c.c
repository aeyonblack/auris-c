#include "auris_i2c.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int auris_i2c_find(const char *sysfs_root, const char *controller, char *path,
                   size_t capacity) {
  if (sysfs_root == NULL || controller == NULL || path == NULL ||
      capacity == 0U) {
    return -EINVAL;
  }
  DIR *dir = opendir(sysfs_root);
  if (dir == NULL) {
    return -errno;
  }
  int result = -ENODEV;
  const struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "i2c-", 4U) != 0) {
      continue;
    }
    char link[512], target[512];
    const int n = snprintf(link, sizeof(link), "%s/%s/of_node", sysfs_root,
                           entry->d_name);
    if (n < 0 || (size_t)n >= sizeof(link)) {
      continue;
    }
    const ssize_t length = readlink(link, target, sizeof(target) - 1U);
    if (length <= 0) {
      continue;
    }
    target[length] = '\0';
    const char *slash = strrchr(target, '/');
    const char *name = slash == NULL ? target : slash + 1;
    if (strcmp(name, controller) != 0) {
      continue;
    }
    const int written = snprintf(path, capacity, "/dev/%s", entry->d_name);
    result = written < 0 || (size_t)written >= capacity ? -ENAMETOOLONG : 0;
    break;
  }
  (void)closedir(dir);
  return result;
}

int auris_i2c_open(auris_i2c_dev_t *dev, const char *path) {
  if (dev == NULL || path == NULL) {
    return -EINVAL;
  }
  dev->fd = open(path, O_RDWR | O_CLOEXEC);
  return dev->fd < 0 ? -errno : 0;
}

void auris_i2c_close(auris_i2c_dev_t *dev) {
  if (dev != NULL && dev->fd >= 0) {
    (void)close(dev->fd);
    dev->fd = -1;
  }
}

static int transfer(int fd, struct i2c_msg *messages, unsigned int count) {
  struct i2c_rdwr_ioctl_data data = {.msgs = messages, .nmsgs = count};
  int result;
  do {
    result = ioctl(fd, I2C_RDWR, &data);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    return -errno;
  }
  return result == (int)count ? 0 : -EIO;
}

static int dev_write(void *context, uint8_t address, uint8_t reg,
                     uint8_t value) {
  const auris_i2c_dev_t *dev = context;
  uint8_t bytes[2] = {reg, value};
  struct i2c_msg message = {.addr = address, .flags = 0U, .len = 2U,
                            .buf = bytes};
  return transfer(dev->fd, &message, 1U);
}

static int dev_read(void *context, uint8_t address, uint8_t reg,
                    uint8_t *value) {
  const auris_i2c_dev_t *dev = context;
  struct i2c_msg messages[2] = {
      {.addr = address, .flags = 0U, .len = 1U, .buf = &reg},
      {.addr = address, .flags = I2C_M_RD, .len = 1U, .buf = value}};
  return transfer(dev->fd, messages, 2U);
}

static void dev_sleep(void *context, unsigned int milliseconds) {
  (void)context;
  struct timespec remaining = {.tv_sec = (time_t)(milliseconds / 1000U),
                               .tv_nsec = (long)(milliseconds % 1000U) *
                                          1000000L};
  while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
  }
}

auris_i2c_bus_t auris_i2c_bus(auris_i2c_dev_t *dev) {
  return (auris_i2c_bus_t){dev, dev_write, dev_read, dev_sleep};
}
