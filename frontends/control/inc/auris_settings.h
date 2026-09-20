#ifndef AURIS_SETTINGS_H
#define AURIS_SETTINGS_H
#include <stdbool.h>
/* Values, not driver handles. Copy across the audio mutex at frame boundaries. */
typedef struct {
  bool overlay, gating, smoothing;
  float cutoff, opacity;
  unsigned int fps;
} auris_settings_t;
auris_settings_t auris_settings_defaults(void);
bool auris_settings_valid(const auris_settings_t *settings);
/* All-or-nothing parse; a missing/invalid file never changes *settings. */
int auris_settings_load(const char *path, auris_settings_t *settings);
/* Dedicated worker: disk writes never block capture/rendering. */
typedef struct auris_settings_writer auris_settings_writer_t;
int auris_settings_writer_open(auris_settings_writer_t **out, const char *path);
int auris_settings_save(auris_settings_writer_t *writer, const auris_settings_t *settings);
/* 1 while busy, 0 success, negative errno on failure. */
int auris_settings_save_status(auris_settings_writer_t *writer);
void auris_settings_writer_close(auris_settings_writer_t *writer);
#endif
