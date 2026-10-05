#include "camera_chain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

static float gains[CAMERA_SHADING_MAX_NODES * 3U];

static int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL) return -1;
  fputs(text, f);
  return fclose(f);
}

int main(void) {
  char path[] = "/tmp/auris_shading_XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  (void)close(fd);
  uint32_t columns = 0U, rows = 0U;
  CHECK(write_file(path, "AURISSHADE1 3 2\n"
                         "1.5 1.2 1.6  1 1 1  1.4 1.2 1.7\n"
                         "1.5 1.2 1.6  1.0 1.0 1.0  1.4 1.2 1.7\n") == 0);
  CHECK(camera_chain_load_shading(path, &columns, &rows, gains) == 0);
  CHECK(columns == 3U && rows == 2U && gains[0] == 1.5f && gains[4] == 1.0f &&
        gains[17] == 1.7f);
  /* Too few values, too many, a bad header, a bad gain. */
  CHECK(write_file(path, "AURISSHADE1 3 2\n1 1 1 1 1 1\n") == 0);
  CHECK(camera_chain_load_shading(path, &columns, &rows, gains) == -EINVAL);
  CHECK(write_file(path, "AURISSHADE1 2 2\n1 1 1 1 1 1 1 1 1 1 1 1 1\n") == 0);
  CHECK(camera_chain_load_shading(path, &columns, &rows, gains) == -EINVAL);
  CHECK(write_file(path, "AURISCAL1 2 2\n1 1 1 1 1 1 1 1 1 1 1 1\n") == 0);
  CHECK(camera_chain_load_shading(path, &columns, &rows, gains) == -EINVAL);
  CHECK(write_file(path, "AURISSHADE1 2 2\n1 1 1 1 0 1 1 1 1 1 1 1\n") == 0);
  CHECK(camera_chain_load_shading(path, &columns, &rows, gains) == -EINVAL);
  CHECK(write_file(path, "AURISSHADE1 1 2\n1 1 1 1 1 1\n") == 0);
  CHECK(camera_chain_load_shading(path, &columns, &rows, gains) == -EINVAL);
  CHECK(unlink(path) == 0);
  CHECK(camera_chain_load_shading(path, &columns, &rows, gains) == -ENOENT);
  CHECK(camera_chain_load_shading(NULL, &columns, &rows, gains) == -EINVAL);
  /* The rest needs the camera; without one, nothing is left half open. */
  camera_chain_t *chain = NULL;
  CHECK(camera_chain_open(NULL, 640U, 360U, NULL) == -EINVAL);
  CHECK(camera_chain_format(NULL) == NULL && camera_chain_content(NULL) == NULL);
  CHECK(camera_chain_start(NULL) == -EINVAL);
  camera_chain_close(chain);
  puts("camera chain: ok");
  return EXIT_SUCCESS;
}
