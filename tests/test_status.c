#include "auris_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_string(const char *actual, const char *expected) {
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "Expected: %s, received: %s\n", expected, actual);
    return 0;
  }
  return 1;
}

int main(void) {
  if (!expect_string(auris_status_string(AURIS_OK), "ok")) {
    return EXIT_FAILURE;
  }

  if (!expect_string(auris_status_string(AURIS_ERROR_INVALID_ARGUMENT),
                     "invalid argument")) {
    return EXIT_FAILURE;
  }

  if (!expect_string(auris_status_string((auris_status_t)99),
                     "unknown error")) {
    return EXIT_FAILURE;
  }

  if (!expect_string(auris_status_string(AURIS_ERROR_CAPACITY),
                     "insufficient capacity")) {
    return EXIT_FAILURE;
  }

  printf("status tests passed\n");
  return EXIT_SUCCESS;
}
