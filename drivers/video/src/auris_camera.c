#include "auris_camera.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>

#define BUFFERS 4U
struct auris_camera {
  int fd;
  bool streaming, held;
  uint32_t count, held_index;
  struct {
    void *data;
    size_t length;
  } buffers[BUFFERS];
  auris_camera_format_t format;
};

static int control(int fd, unsigned long request, void *arg) {
  int result;
  do {
    result = ioctl(fd, request, arg);
  } while (result < 0 && errno == EINTR);
  return result < 0 ? -errno : 0;
}

static int set_pad(const char *path, uint32_t width, uint32_t height) {
  const int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -errno;
  struct v4l2_subdev_format f = {.which = V4L2_SUBDEV_FORMAT_ACTIVE, .pad = 0U};
  f.format.width = width;
  f.format.height = height;
  f.format.code = MEDIA_BUS_FMT_SRGGB8_1X8;
  f.format.field = V4L2_FIELD_NONE;
  int err = control(fd, VIDIOC_SUBDEV_S_FMT, &f);
  if (err == 0)
    err = control(fd, VIDIOC_SUBDEV_G_FMT, &f);
  if (err == 0 && (f.format.width != width || f.format.height != height ||
                   f.format.code != MEDIA_BUS_FMT_SRGGB8_1X8))
    err = -ENOTSUP;
  (void)close(fd);
  return err;
}

static int queue(auris_camera_t *s, uint32_t index) {
  struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                          .memory = V4L2_MEMORY_MMAP,
                          .index = index};
  return control(s->fd, VIDIOC_QBUF, &b);
}

void auris_camera_close(auris_camera_t *s) {
  if (s == NULL)
    return;
  if (s->streaming) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)control(s->fd, VIDIOC_STREAMOFF, &type);
  }
  /* Closing also stops DMA if STREAMOFF failed. Mappings remain valid
     until munmap, but are no longer exposed to a consumer. */
  if (s->fd >= 0)
    (void)close(s->fd);
  for (uint32_t i = 0U; i < s->count; ++i) {
    if (s->buffers[i].length != 0U)
      (void)munmap(s->buffers[i].data, s->buffers[i].length);
  }
  free(s);
}

int auris_camera_open(auris_camera_t **out, const auris_camera_config_t *cfg) {
  if (out == NULL || *out != NULL || cfg == NULL || cfg->sensor == NULL ||
      cfg->bridge == NULL || cfg->device == NULL || cfg->width < 4U ||
      cfg->height < 4U || cfg->width > 4096U || cfg->height > 4096U)
    return -EINVAL;
  auris_camera_t *s = calloc(1U, sizeof(*s));
  if (s == NULL)
    return -ENOMEM;
  s->fd = open(cfg->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  int err = s->fd < 0 ? -errno : 0;
  if (err < 0)
    goto fail;
  struct v4l2_capability cap = {0};
  err = control(s->fd, VIDIOC_QUERYCAP, &cap);
  if (err < 0)
    goto fail;
  const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                            ? cap.device_caps
                            : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING) ||
      (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))) {
    err = -ENOTSUP;
    goto fail;
  }
  err = set_pad(cfg->sensor, cfg->width, cfg->height);
  if (err < 0)
    goto fail;
  err = set_pad(cfg->bridge, cfg->width, cfg->height);
  if (err < 0)
    goto fail;
  struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
  f.fmt.pix.width = cfg->width;
  f.fmt.pix.height = cfg->height;
  f.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB8;
  f.fmt.pix.field = V4L2_FIELD_NONE;
  err = control(s->fd, VIDIOC_S_FMT, &f);
  if (err == 0)
    err = control(s->fd, VIDIOC_G_FMT, &f);
  if (err < 0)
    goto fail;
  const struct v4l2_pix_format *p = &f.fmt.pix;
  if (p->width != cfg->width || p->height != cfg->height ||
      p->pixelformat != V4L2_PIX_FMT_SRGGB8 || p->field != V4L2_FIELD_NONE) {
    err = -ENOTSUP;
    goto fail;
  }
  s->format = (auris_camera_format_t){p->width, p->height, p->bytesperline,
                                      p->bytesperline, p->sizeimage};
  struct utsname kernel;
  if (cfg->ti_5_10_packed && uname(&kernel) == 0 &&
      strcmp(kernel.release, "5.10.168-ti-arm64-r108") == 0 &&
      strncmp((const char *)cap.driver, "j721e-csi2rx", sizeof(cap.driver)) ==
          0 &&
      p->width == 1640U && p->height == 1232U && p->bytesperline == 1648U &&
      p->sizeimage == 2020480U) {
    s->format.stride = 1640U;
  }
  const uint64_t required =
      (uint64_t)(p->height - 1U) * s->format.stride + p->width;
  if (s->format.stride < p->width || required > p->sizeimage) {
    err = -EPROTO;
    goto fail;
  }
  struct v4l2_requestbuffers request = {.count = BUFFERS,
                                        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                        .memory = V4L2_MEMORY_MMAP};
  err = control(s->fd, VIDIOC_REQBUFS, &request);
  if (err < 0)
    goto fail;
  if (request.count < 2U || request.count > BUFFERS) {
    err = -ENOBUFS;
    goto fail;
  }
  s->count = request.count;
  for (uint32_t i = 0U; i < s->count; ++i) {
    struct v4l2_buffer b = {
        .type = request.type, .memory = request.memory, .index = i};
    err = control(s->fd, VIDIOC_QUERYBUF, &b);
    if (err < 0)
      goto fail;
    if (b.length < p->sizeimage) {
      err = -EPROTO;
      goto fail;
    }
    void *data = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd,
                      b.m.offset);
    if (data == MAP_FAILED) {
      err = -errno;
      goto fail;
    }
    s->buffers[i].data = data;
    s->buffers[i].length = b.length;
  }
  *out = s;
  return 0;
fail:
  auris_camera_close(s);
  return err;
}

const auris_camera_format_t *auris_camera_format(const auris_camera_t *s) {
  return s == NULL ? NULL : &s->format;
}

int auris_camera_start(auris_camera_t *s) {
  if (s == NULL)
    return -EINVAL;
  if (s->streaming)
    return -EBUSY;
  for (uint32_t i = 0U; i < s->count; ++i) {
    const int err = queue(s, i);
    if (err < 0)
      return err; /* Caller closes after any start failure. */
  }
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  const int err = control(s->fd, VIDIOC_STREAMON, &type);
  if (err == 0)
    s->streaming = true;
  return err;
}

int auris_camera_release(auris_camera_t *s) {
  if (s == NULL || !s->held)
    return -EINVAL;
  const int err = queue(s, s->held_index);
  if (err == 0)
    s->held = false;
  return err;
}

int auris_camera_acquire(auris_camera_t *s, auris_camera_frame_t *out,
                         int timeout_ms) {
  if (s == NULL || out == NULL || timeout_ms < 0)
    return -EINVAL;
  *out = (auris_camera_frame_t){0};
  if (!s->streaming)
    return -EPIPE;
  if (s->held)
    return -EBUSY;
  struct pollfd p = {.fd = s->fd, .events = POLLIN};
  const int ready = poll(&p, 1U, timeout_ms);
  if (ready < 0)
    return -errno; /* EINTR returns control to the app. */
  if (ready == 0)
    return -ETIMEDOUT;
  if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
    return -EIO;
  if (!(p.revents & POLLIN))
    return -EAGAIN;
  struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                          .memory = V4L2_MEMORY_MMAP};
  const int err = control(s->fd, VIDIOC_DQBUF, &b);
  if (err < 0)
    return err;
  if (b.index >= s->count)
    return -EPROTO; /* Caller must close. */
  s->held = true;
  s->held_index = b.index;
  const uint64_t required =
      (uint64_t)(s->format.height - 1U) * s->format.stride + s->format.width;
  if ((b.flags & V4L2_BUF_FLAG_ERROR) || b.bytesused < required ||
      b.bytesused > s->buffers[b.index].length || b.timestamp.tv_sec < 0 ||
      b.timestamp.tv_usec < 0 || b.timestamp.tv_usec >= 1000000) {
    const int released = auris_camera_release(s);
    return released < 0 ? released : -EBADMSG;
  }
  *out = (auris_camera_frame_t){
      s->buffers[b.index].data, b.bytesused, b.sequence, b.flags,
      (uint64_t)b.timestamp.tv_sec * 1000000U + (uint64_t)b.timestamp.tv_usec};
  return 0;
}
