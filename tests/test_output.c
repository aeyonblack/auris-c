#include "auris_stream.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expression) do { if (!(expression)) return 1; } while (0)

int main(void) {
  int fds[2];
  CHECK(pipe(fds) == 0);
  auris_output_t *output = NULL;
  CHECK(auris_output_open_jpeg_stream(&output, fds[1], 16U, 8U, 80U) == 0);
  uint8_t pixels[16U * 8U * 3U];
  for (size_t i = 0U; i < sizeof(pixels); ++i) pixels[i] = (uint8_t)i;
  const auris_rgb24_t image = {pixels, 16U, 8U, 16U * 3U, sizeof(pixels)};
  const auris_output_frame_t frame = {
      .sequence = 7U, .camera_us = 100U, .flags = 0x2001U};
  CHECK(auris_output_submit(output, &image, &frame) == 0);
  CHECK(auris_output_flush(output, 1000U) == 0);
  auris_output_stats_t stats;
  auris_output_stats(output, &stats);
  CHECK(stats.submitted == 1U && stats.sent == 1U && stats.dropped == 0U);
  CHECK(stats.encoded_bytes > 4U);
  auris_output_close(output);
  CHECK(close(fds[1]) == 0);

  uint8_t packet[65536];
  size_t used = 0U;
  ssize_t count;
  while ((count = read(fds[0], packet + used, sizeof(packet) - used)) > 0)
    used += (size_t)count;
  CHECK(count == 0 && close(fds[0]) == 0);
  const uint8_t *first = memchr(packet, '\n', used);
  CHECK(first != NULL);
  const size_t remaining = used - (size_t)(first + 1U - packet);
  const uint8_t *second = memchr(first + 1U, '\n', remaining);
  CHECK(second != NULL);
  CHECK((size_t)(first - packet) == strlen("AURISJPEG1 16 8"));
  CHECK(memcmp(packet, "AURISJPEG1 16 8", (size_t)(first - packet)) == 0);
  CHECK((size_t)(second - (first + 1U)) >= 6U);
  CHECK(memcmp(first + 1U, "FRAME ", 6U) == 0);
  const uint8_t *jpeg = second + 1U;
  const size_t jpeg_bytes = used - (size_t)(jpeg - packet);
  CHECK(jpeg_bytes == stats.encoded_bytes);
  CHECK(jpeg[0] == 0xffU && jpeg[1] == 0xd8U);
  CHECK(jpeg[jpeg_bytes - 2U] == 0xffU && jpeg[jpeg_bytes - 1U] == 0xd9U);
  return 0;
}
