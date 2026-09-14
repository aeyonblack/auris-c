#include "auris_status.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const auris_status_t status = AURIS_OK;
  printf("Auris C foundation is running. \n");
  printf("Status: %s\n", auris_status_string(status));

  return EXIT_SUCCESS;
}
