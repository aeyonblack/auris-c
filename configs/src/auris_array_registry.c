#include "auris_array.h"
#include "auris_array_configs.h"

#include <stddef.h>
#include <string.h>

typedef struct {
  const char *name;
  const auris_array_t *(*array)(void);
  const auris_capture_profile_t *(*capture)(void);
} entry_t;

static const entry_t ENTRIES[] = {
    {"respeaker", auris_config_respeaker_usb_4, auris_capture_respeaker_usb},
    {"cmb", auris_config_cmb_8, auris_capture_cmb},
};

bool auris_config_lookup(const char *name, auris_config_t *out) {
  if (name == NULL || out == NULL) {
    return false;
  }
  for (size_t i = 0U; i < sizeof(ENTRIES) / sizeof(ENTRIES[0]); ++i) {
    if (strcmp(name, ENTRIES[i].name) == 0) {
      *out = (auris_config_t){ENTRIES[i].name, ENTRIES[i].array(),
                              ENTRIES[i].capture()};
      return true;
    }
  }
  return false;
}

const auris_array_t *auris_config_find(const char *name) {
  auris_config_t config;
  return auris_config_lookup(name, &config) ? config.array : NULL;
}

const char *auris_config_names(void) { return "respeaker, cmb"; }
