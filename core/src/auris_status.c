#include "auris_status.h"

const char *auris_status_string(auris_status_t status) {
  switch (status) {
  case AURIS_OK:
    return "ok";
  case AURIS_ERROR_INVALID_ARGUMENT:
    return "invalid argument";
  case AURIS_ERROR_INTERNAL:
    return "internal error";
  case AURIS_ERROR_NOT_READY:
    return "not ready";
  case AURIS_ERROR_CAPACITY:
    return "insufficient capacity";
  default:
    return "unknown error";
  }
}
