#ifndef AURIS_WAV_H
#define AURIS_WAV_H

#include "auris_audio.h"
#include "auris_status.h"

#include <stdint.h>
#include <stdio.h>

#define AURIS_WAV_HEADER_BYTES 44U
#define AURIS_WAV_UNKNOWN_FRAMES UINT64_MAX

/* A WAV recording of raw device frames: exactly the interleaved bytes the
   capture device delivered, in device channel order, before mapping. */
typedef struct {
  unsigned int sample_rate;
  size_t channels;
  auris_sample_format_t format;
  uint64_t frames; /* AURIS_WAV_UNKNOWN_FRAMES if the header has no size */
} auris_wav_info_t;

typedef struct {
  FILE *file;
  auris_wav_info_t info;
  size_t frame_bytes;
  uint64_t data_bytes;
} auris_wav_writer_t;

/* Creates `path` for the profile's channels, format and rate. Initialize
   the writer with {0}. Returns 0 or a negative errno value. */
int auris_wav_writer_open(auris_wav_writer_t *writer, const char *path,
                          const auris_capture_profile_t *profile);
/* Appends `frames` complete device frames. -EFBIG past the 4 GiB limit. */
int auris_wav_writer_write(auris_wav_writer_t *writer, const void *data,
                           size_t frames);
/* Writes the final sizes into the header and closes the file. */
int auris_wav_writer_close(auris_wav_writer_t *writer);

typedef struct {
  FILE *file;
  auris_wav_info_t info;
  size_t frame_bytes;
  uint64_t remaining; /* data bytes left; UINT64_MAX = read to end of file */
  uint64_t position;  /* frames delivered so far */
  size_t channels;
  size_t map[AURIS_AUDIO_CHANNELS];
  unsigned char raw[AURIS_AUDIO_FRAMES * AURIS_AUDIO_RAW_CHANNELS_MAX * 4U];
} auris_wav_reader_t;

/* Opens a recording made with `profile` and maps it like the live device.
   -EPROTO: not a WAV file this reader supports.
   -EINVAL: valid WAV, but its channels, format or rate differ from the
   profile, or the map is invalid. */
int auris_wav_open(auris_wav_reader_t *reader, const char *path,
                   const auris_capture_profile_t *profile, const size_t *map,
                   size_t channels);
/* auris_audio_source_t read callback. Zero frames means end of recording. */
auris_status_t auris_wav_read(void *context, auris_audio_block_t *block);
void auris_wav_close(auris_wav_reader_t *reader);

#endif
