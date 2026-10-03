#include "auris_array_configs.h"
#include "auris_wav.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

static void put_le(unsigned char *p, uint64_t v, unsigned int bytes) {
  for (unsigned int i = 0U; i < bytes; ++i) {
    p[i] = (unsigned char)((v >> (8U * i)) & 0xFFU);
  }
}

static uint32_t get_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}

/* Device frame n, channel k: marker ((k + 1) * 1000 + n) << 8 (S32). */
static void cmb_frames(unsigned char *out, size_t first, size_t count) {
  for (size_t n = 0U; n < count; ++n) {
    for (size_t k = 0U; k < 8U; ++k) {
      const int64_t value = ((int64_t)(k + 1U) * 1000 + (int64_t)(first + n)) * 256;
      put_le(&out[(n * 8U + k) * 4U], (uint64_t)value & 0xFFFFFFFFU, 4U);
    }
  }
}

static float cmb_expected(size_t device_channel, uint64_t frame) {
  return (float)(((double)(device_channel + 1U) * 1000.0 + (double)frame) *
                 256.0 / 2147483648.0);
}

static int write_file(const char *path, const unsigned char *bytes, size_t n) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) return -1;
  const size_t written = fwrite(bytes, 1U, n, f);
  return fclose(f) == 0 && written == n ? 0 : -1;
}

static int test_roundtrip(const char *path) {
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  const auris_array_t *array = auris_config_cmb_8();
  enum { TOTAL = 300 }; /* 128 + 128 + 44: a short final block */
  static unsigned char frames[TOTAL * 32];
  cmb_frames(frames, 0U, TOTAL);

  auris_wav_writer_t w = {0};
  CHECK(auris_wav_writer_open(&w, path, cmb) == 0);
  CHECK(auris_wav_writer_write(&w, frames, 100U) == 0);
  CHECK(auris_wav_writer_write(&w, &frames[100 * 32], TOTAL - 100U) == 0);
  CHECK(auris_wav_writer_write(&w, frames, 0U) == 0);
  CHECK(auris_wav_writer_close(&w) == 0);
  CHECK(w.file == NULL);

  /* The header is a standard 44-byte PCM WAV header. */
  unsigned char h[AURIS_WAV_HEADER_BYTES];
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  CHECK(fread(h, 1U, sizeof(h), f) == sizeof(h));
  CHECK(fseek(f, 0L, SEEK_END) == 0);
  const long size = ftell(f);
  (void)fclose(f);
  CHECK(size == (long)(AURIS_WAV_HEADER_BYTES + TOTAL * 32U));
  CHECK(memcmp(h, "RIFF", 4U) == 0 && memcmp(&h[8], "WAVEfmt ", 8U) == 0);
  CHECK(get_le32(&h[4]) == 36U + TOTAL * 32U);
  CHECK(get_le32(&h[16]) == 16U);
  CHECK((get_le32(&h[20]) & 0xFFFFU) == 1U);          /* PCM */
  CHECK((get_le32(&h[20]) >> 16U) == 8U);             /* channels */
  CHECK(get_le32(&h[24]) == 48000U);                  /* sample rate */
  CHECK(get_le32(&h[28]) == 48000U * 32U);            /* byte rate */
  CHECK((get_le32(&h[32]) & 0xFFFFU) == 32U);         /* block align */
  CHECK((get_le32(&h[32]) >> 16U) == 32U);            /* bits per sample */
  CHECK(memcmp(&h[36], "data", 4U) == 0 && get_le32(&h[40]) == TOTAL * 32U);

  /* Replay through the same converter and map as the live device. */
  auris_wav_reader_t r = {0};
  CHECK(auris_wav_open(&r, path, cmb, array->capture_channels, 8U) == 0);
  CHECK(r.info.sample_rate == 48000U && r.info.channels == 8U &&
        r.info.format == AURIS_SAMPLE_S32_LE && r.info.frames == TOTAL);
  auris_audio_source_t source = {&r, auris_wav_read};
  auris_audio_block_t block;
  const size_t expect[3] = {128U, 128U, 44U};
  uint64_t first = 0U;
  for (size_t b = 0U; b < 3U; ++b) {
    CHECK(source.read(source.context, &block) == AURIS_OK);
    CHECK(block.frames == expect[b] && block.first_frame == first);
    CHECK(block.sample_rate == 48000U && block.channels == 8U);
    for (size_t m = 0U; m < 8U; ++m) {
      for (size_t n = 0U; n < block.frames; ++n) {
        CHECK(block.samples[m][n] ==
              cmb_expected(array->capture_channels[m], first + n));
      }
    }
    first += block.frames;
  }
  for (size_t i = 0U; i < 2U; ++i) {
    CHECK(source.read(source.context, &block) == AURIS_OK);
    CHECK(block.frames == 0U && block.first_frame == TOTAL);
  }
  auris_wav_close(&r);
  CHECK(r.file == NULL);
  auris_wav_close(&r); /* closing twice is harmless */

  /* The recording belongs to the CMB: other profiles are refused. */
  const auris_capture_profile_t *rs = auris_capture_respeaker_usb();
  const size_t rs_map[4] = {1U, 2U, 3U, 4U};
  CHECK(auris_wav_open(&r, path, rs, rs_map, 4U) == -EINVAL);
  auris_capture_profile_t other = *cmb;
  other.sample_rate = 16000U;
  CHECK(auris_wav_open(&r, path, &other, array->capture_channels, 8U) == -EINVAL);
  const size_t bad_map[2] = {0U, 8U};
  CHECK(auris_wav_open(&r, path, cmb, bad_map, 2U) == -EINVAL);
  CHECK(r.file == NULL);
  return EXIT_SUCCESS;
}

static int test_s16(const char *path) {
  /* ReSpeaker layout: 6 S16 channels at 16 kHz, mics on 1..4. */
  const auris_capture_profile_t *rs = auris_capture_respeaker_usb();
  const auris_array_t *array = auris_config_respeaker_usb_4();
  static unsigned char frames[200 * 12];
  for (size_t n = 0U; n < 200U; ++n) {
    for (size_t k = 0U; k < 6U; ++k) {
      const int32_t v = (int32_t)(k * 1000U + n) * (k % 2U == 0U ? 1 : -1);
      put_le(&frames[(n * 6U + k) * 2U], (uint64_t)(uint32_t)v & 0xFFFFU, 2U);
    }
  }
  auris_wav_writer_t w = {0};
  CHECK(auris_wav_writer_open(&w, path, rs) == 0);
  CHECK(auris_wav_writer_write(&w, frames, 200U) == 0);
  CHECK(auris_wav_writer_close(&w) == 0);
  auris_wav_reader_t r = {0};
  CHECK(auris_wav_open(&r, path, rs, array->capture_channels, 4U) == 0);
  CHECK(r.info.format == AURIS_SAMPLE_S16_LE && r.info.frames == 200U);
  auris_audio_block_t block;
  CHECK(auris_wav_read(&r, &block) == AURIS_OK && block.frames == 128U);
  CHECK(block.sample_rate == 16000U && block.channels == 4U);
  for (size_t m = 0U; m < 4U; ++m) {
    const size_t k = array->capture_channels[m];
    for (size_t n = 0U; n < 128U; ++n) {
      const int32_t v = (int32_t)(k * 1000U + n) * (k % 2U == 0U ? 1 : -1);
      CHECK(block.samples[m][n] == (float)v / 32768.0f);
    }
  }
  auris_wav_close(&r);
  return EXIT_SUCCESS;
}

static int test_foreign_headers(const char *path) {
  const auris_capture_profile_t *cmb = auris_capture_cmb();
  const size_t *map = auris_config_cmb_8()->capture_channels;
  auris_wav_reader_t r = {0};
  auris_audio_block_t block;
  enum { FRAMES = 10 };
  unsigned char data[FRAMES * 32];
  cmb_frames(data, 0U, FRAMES);

  /* arecord-style: WAVE_FORMAT_EXTENSIBLE, an extra LIST chunk before the
     data, and a data size of 0xFFFFFFFF because it streamed to a pipe. */
  unsigned char file[12 + 8 + 40 + 8 + 4 + 8 + FRAMES * 32];
  size_t at = 0U;
  memcpy(&file[at], "RIFF", 4U); put_le(&file[at + 4U], 0xFFFFFFFFU, 4U);
  memcpy(&file[at + 8U], "WAVE", 4U); at += 12U;
  memcpy(&file[at], "fmt ", 4U); put_le(&file[at + 4U], 40U, 4U); at += 8U;
  put_le(&file[at], 0xFFFEU, 2U); put_le(&file[at + 2U], 8U, 2U);
  put_le(&file[at + 4U], 48000U, 4U); put_le(&file[at + 8U], 48000U * 32U, 4U);
  put_le(&file[at + 12U], 32U, 2U); put_le(&file[at + 14U], 32U, 2U);
  put_le(&file[at + 16U], 22U, 2U); put_le(&file[at + 18U], 24U, 2U);
  put_le(&file[at + 20U], 0U, 4U);
  put_le(&file[at + 24U], 1U, 2U); /* KSDATAFORMAT_SUBTYPE_PCM begins 01 00 */
  memset(&file[at + 26U], 0, 14U); at += 40U;
  memcpy(&file[at], "LIST", 4U); put_le(&file[at + 4U], 4U, 4U);
  memcpy(&file[at + 8U], "INFO", 4U); at += 12U;
  memcpy(&file[at], "data", 4U); put_le(&file[at + 4U], 0xFFFFFFFFU, 4U); at += 8U;
  memcpy(&file[at], data, sizeof(data)); at += sizeof(data);
  CHECK(write_file(path, file, at) == 0);
  CHECK(auris_wav_open(&r, path, cmb, map, 8U) == 0);
  CHECK(r.info.frames == AURIS_WAV_UNKNOWN_FRAMES);
  CHECK(auris_wav_read(&r, &block) == AURIS_OK && block.frames == FRAMES);
  CHECK(block.samples[0][9] == cmb_expected(map[0], 9U));
  CHECK(auris_wav_read(&r, &block) == AURIS_OK && block.frames == 0U);
  auris_wav_close(&r);

  /* Not a WAV file at all, and a truncated header. */
  CHECK(write_file(path, (const unsigned char *)"hello, world", 12U) == 0);
  CHECK(auris_wav_open(&r, path, cmb, map, 8U) == -EPROTO);
  CHECK(write_file(path, file, 30U) == 0);
  CHECK(auris_wav_open(&r, path, cmb, map, 8U) == -EPROTO);
  CHECK(auris_wav_open(&r, "/nonexistent/x.wav", cmb, map, 8U) == -ENOENT);
  CHECK(r.file == NULL);
  return EXIT_SUCCESS;
}

int main(void) {
  char path[] = "/tmp/auris_wav_testXXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return EXIT_FAILURE;
  (void)close(fd);
  const int result = test_roundtrip(path) == EXIT_SUCCESS &&
                             test_s16(path) == EXIT_SUCCESS &&
                             test_foreign_headers(path) == EXIT_SUCCESS
                         ? EXIT_SUCCESS
                         : EXIT_FAILURE;
  (void)unlink(path);
  if (result == EXIT_SUCCESS) puts("WAV record/replay tests: ok");
  return result;
}
