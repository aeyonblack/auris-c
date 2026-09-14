#ifndef AURIS_STATUS_H
#define AURIS_STATUS_H

typedef enum {
  AURIS_OK = 0,
  AURIS_ERROR_INVALID_ARGUMENT = -1,
  AURIS_ERROR_INTERNAL = -2,
  AURIS_ERROR_NOT_READY = -3,
  AURIS_ERROR_CAPACITY = -4
} auris_status_t;

const char *auris_status_string(auris_status_t status);

#endif
