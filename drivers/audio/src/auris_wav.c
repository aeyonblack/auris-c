#include "auris_wav.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define FORMAT_PCM 0x0001U
#define FORMAT_EXTENSIBLE 0xFFFEU
#define MAX_DATA_BYTES 0xFFFFFFFFULL /* RIFF sizes are 32-bit */

static void put16(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xFFU);
  p[1] = (unsigned char)((v >> 8U) & 0xFFU);
}

static void put32(unsigned char *p, uint32_t v) {
  put16(p, v & 0xFFFFU);
  put16(p + 2, v >> 16U);
}

static uint32_t get16(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U);
}

static uint32_t get32(const unsigned char *p) {
  return get16(p) | (get16(p + 2) << 16U);
}

/* The canonical 44-byte header: RIFF/WAVE, a 16-byte PCM fmt chunk, and
   the data chunk header. Sizes are filled in when the writer closes. */
static void make_header(unsigned char h[AURIS_WAV_HEADER_BYTES],
                        const auris_wav_info_t *info, size_t frame_bytes,
                        uint32_t data_bytes) {
  const uint32_t bits = (uint32_t)(auris_sample_bytes(info->format) * 8U);
  memcpy(h, "RIFF", 4U);
  put32(h + 4, 36U + data_bytes);
  memcpy(h + 8, "WAVEfmt ", 8U);
  put32(h + 16, 16U);
  put16(h + 20, FORMAT_PCM);
  put16(h + 22, (uint32_t)info->channels);
  put32(h + 24, info->sample_rate);
  put32(h + 28, info->sample_rate * (uint32_t)frame_bytes);
  put16(h + 32, (uint32_t)frame_bytes);
  put16(h + 34, bits);
  memcpy(h + 36, "data", 4U);
  put32(h + 40, data_bytes);
}

int auris_wav_writer_open(auris_wav_writer_t *w, const char *path,
                          const auris_capture_profile_t *profile) {
  if (w == NULL || path == NULL || profile == NULL || w->file != NULL ||
      profile->sample_rate == 0U || profile->raw_channels == 0U ||
      profile->raw_channels > AURIS_AUDIO_RAW_CHANNELS_MAX ||
      auris_sample_bytes(profile->format) == 0U) {
    return -EINVAL;
  }
  *w = (auris_wav_writer_t){
      .info = {.sample_rate = profile->sample_rate,
               .channels = profile->raw_channels,
               .format = profile->format,
               .frames = 0U},
      .frame_bytes =
          profile->raw_channels * auris_sample_bytes(profile->format)};
  w->file = fopen(path, "wb");
  if (w->file == NULL) {
    return -errno;
  }
  unsigned char h[AURIS_WAV_HEADER_BYTES];
  make_header(h, &w->info, w->frame_bytes, 0U);
  if (fwrite(h, 1U, sizeof(h), w->file) != sizeof(h)) {
    const int err = -errno;
    (void)fclose(w->file);
    w->file = NULL;
    return err != 0 ? err : -EIO;
  }
  return 0;
}

int auris_wav_writer_write(auris_wav_writer_t *w, const void *data,
                           size_t frames) {
  if (w == NULL || w->file == NULL || (data == NULL && frames != 0U)) {
    return -EINVAL;
  }
  const uint64_t bytes = (uint64_t)frames * w->frame_bytes;
  if (w->data_bytes + bytes > MAX_DATA_BYTES - 36U) {
    return -EFBIG;
  }
  if (frames != 0U && fwrite(data, w->frame_bytes, frames, w->file) != frames) {
    return -EIO;
  }
  w->data_bytes += bytes;
  w->info.frames += frames;
  return 0;
}

int auris_wav_writer_close(auris_wav_writer_t *w) {
  if (w == NULL || w->file == NULL) {
    return -EINVAL;
  }
  unsigned char h[AURIS_WAV_HEADER_BYTES];
  make_header(h, &w->info, w->frame_bytes, (uint32_t)w->data_bytes);
  int err = 0;
  if (fseek(w->file, 0L, SEEK_SET) != 0 ||
      fwrite(h, 1U, sizeof(h), w->file) != sizeof(h)) {
    err = -EIO;
  }
  if (fclose(w->file) != 0 && err == 0) {
    err = -EIO;
  }
  w->file = NULL;
  return err;
}

/* Reads exactly n bytes; false on end of file or error. */
static bool read_exact(FILE *f, unsigned char *p, size_t n) {
  return fread(p, 1U, n, f) == n;
}

/* Walks the RIFF chunks until "data", filling info from "fmt ". */
static int parse(auris_wav_reader_t *r) {
  unsigned char h[40];
  if (!read_exact(r->file, h, 12U) || memcmp(h, "RIFF", 4U) != 0 ||
      memcmp(h + 8, "WAVE", 4U) != 0) {
    return -EPROTO;
  }
  bool have_format = false;
  for (;;) {
    if (!read_exact(r->file, h, 8U)) {
      return -EPROTO;
    }
    const uint32_t size = get32(h + 4);
    if (memcmp(h, "fmt ", 4U) == 0) {
      if (size < 16U || size > sizeof(h) || !read_exact(r->file, h, size)) {
        return -EPROTO;
      }
      uint32_t tag = get16(h);
      if (tag == FORMAT_EXTENSIBLE) {
        if (size < 40U || get16(h + 16) < 22U) {
          return -EPROTO;
        }
        tag = get16(h + 24); /* first two bytes of the sub-format GUID */
      }
      const uint32_t bits = get16(h + 14);
      if (tag != FORMAT_PCM || (bits != 16U && bits != 32U)) {
        return -EPROTO;
      }
      r->info.channels = get16(h + 2);
      r->info.sample_rate = get32(h + 4);
      r->info.format = bits == 16U ? AURIS_SAMPLE_S16_LE : AURIS_SAMPLE_S32_LE;
      r->frame_bytes = r->info.channels * (bits / 8U);
      if (r->info.channels == 0U || get16(h + 12) != r->frame_bytes) {
        return -EPROTO;
      }
      if ((size & 1U) != 0U && fseek(r->file, 1L, SEEK_CUR) != 0) {
        return -EPROTO;
      }
      have_format = true;
    } else if (memcmp(h, "data", 4U) == 0) {
      if (!have_format) {
        return -EPROTO;
      }
      /* A streamed recording cannot go back to write its size. */
      r->remaining = size == 0U || size == 0xFFFFFFFFU ? UINT64_MAX : size;
      r->info.frames = r->remaining == UINT64_MAX
                           ? AURIS_WAV_UNKNOWN_FRAMES
                           : r->remaining / r->frame_bytes;
      return 0;
    } else {
      /* Any other chunk (LIST, fact, ...) is skipped, with its pad byte. */
      if (fseek(r->file, (long)size + (long)(size & 1U), SEEK_CUR) != 0) {
        return -EPROTO;
      }
    }
  }
}

int auris_wav_open(auris_wav_reader_t *r, const char *path,
                   const auris_capture_profile_t *profile, const size_t *map,
                   size_t channels) {
  if (r == NULL || path == NULL || profile == NULL || map == NULL ||
      channels == 0U || channels > AURIS_AUDIO_CHANNELS) {
    return -EINVAL;
  }
  *r = (auris_wav_reader_t){0};
  r->file = fopen(path, "rb");
  if (r->file == NULL) {
    return -errno;
  }
  int err = parse(r);
  if (err == 0 && (r->info.channels != profile->raw_channels ||
                   r->info.format != profile->format ||
                   r->info.sample_rate != profile->sample_rate)) {
    err = -EINVAL;
  }
  for (size_t c = 0U; err == 0 && c < channels; ++c) {
    if (map[c] >= r->info.channels) {
      err = -EINVAL;
    }
    r->map[c] = map[c];
  }
  if (err != 0) {
    auris_wav_close(r);
    return err;
  }
  r->channels = channels;
  return 0;
}

auris_status_t auris_wav_read(void *context, auris_audio_block_t *block) {
  auris_wav_reader_t *r = context;
  if (block == NULL) {
    return AURIS_ERROR_INVALID_ARGUMENT;
  }
  block->frames = 0U;
  if (r == NULL || r->file == NULL) {
    return AURIS_ERROR_NOT_READY;
  }
  size_t want = AURIS_AUDIO_FRAMES;
  if (r->remaining != UINT64_MAX && r->remaining / r->frame_bytes < want) {
    want = (size_t)(r->remaining / r->frame_bytes);
  }
  const size_t got = fread(r->raw, r->frame_bytes, want, r->file);
  if (got < want && ferror(r->file)) {
    return AURIS_ERROR_INTERNAL;
  }
  if (r->remaining != UINT64_MAX) {
    r->remaining -= (uint64_t)got * r->frame_bytes;
  }
  block->sample_rate = r->info.sample_rate;
  block->first_frame = r->position;
  block->channels = r->channels;
  if (got == 0U) {
    return AURIS_OK; /* end of recording */
  }
  const auris_status_t status =
      auris_audio_deinterleave(r->raw, r->info.format, r->info.channels, got,
                               r->map, r->channels, block);
  if (status != AURIS_OK) {
    block->frames = 0U;
    return status;
  }
  r->position += got;
  return AURIS_OK;
}

void auris_wav_close(auris_wav_reader_t *r) {
  if (r != NULL && r->file != NULL) {
    (void)fclose(r->file);
    r->file = NULL;
  }
}
