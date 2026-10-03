#ifndef AURIS_ARRAY_CONFIGS_H
#define AURIS_ARRAY_CONFIGS_H

#include "auris_array.h"
#include "auris_audio.h"

#include <stdbool.h>

const auris_array_t *auris_config_respeaker_usb_4(void);
const auris_array_t *auris_config_cmb_8(void);

/* Capture devices that deliver each array. */
const auris_capture_profile_t *auris_capture_respeaker_usb(void);
const auris_capture_profile_t *auris_capture_cmb(void);

/* A named array together with the device that captures it. */
typedef struct {
  const char *name;
  const auris_array_t *array;
  const auris_capture_profile_t *capture;
} auris_config_t;

/* Look up a compiled configuration by its command-line name.
   Returns NULL for an unknown or NULL name. */
const auris_array_t *auris_config_find(const char *name);

/* Fills `out` and returns true for a known name. */
bool auris_config_lookup(const char *name, auris_config_t *out);

/* Names accepted by auris_config_find, for usage messages. */
const char *auris_config_names(void);

#endif
