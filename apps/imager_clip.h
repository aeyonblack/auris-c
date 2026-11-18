#ifndef IMAGER_CLIP_H
#define IMAGER_CLIP_H

#include "auris_audio.h"
#include "auris_image.h"
#include <stdbool.h>
#include <stdint.h>

/* One recorded test clip. While it records everything stays in memory, so no
   disk write can disturb the live imager; a background thread compresses the
   pictures as they come and writes the folder when the clip ends:

     meta.json  what was tested, the settings, and what was measured
     audio.wav  the raw device frames, playable by eval_replay
     log.csv    one row per analysis frame
     frames.csv one row per displayed frame, with its picture number
     camera/N.jpg   the picture without the heat map
     overlay/N.jpg  the same picture as drawn on screen */

typedef struct {
  uint64_t centre_us, published_us, first_frame;
  float level_dbfs, noise_dbfs, weight_sum;
  float column, row, x, y, z, strength, confidence, contrast;
  uint32_t dsp_us;
  bool ready, active, evidence, valid, fresh;
} imager_clip_row_t;

typedef struct {
  uint32_t sequence;
  uint64_t camera_us, audio_us, composed_us, replaced;
  float strength;
  bool visible;
  int32_t picture; /* -1: no picture was saved of this frame */
} imager_clip_frame_t;

typedef struct {
  float shown_pct, source_dbfs, quiet_dbfs, video_fps, longest_gap_ms;
  uint64_t audio_frames, lost_frames, replaced;
  uint32_t pictures, pictures_lost, rows, frames;
} imager_clip_summary_t;

typedef struct imager_clip imager_clip_t;

/* Starts the idle background thread. Memory is taken only while recording. */
int imager_clip_open(imager_clip_t **out, const auris_capture_profile_t *profile,
                     unsigned int picture_w, unsigned int picture_h);
/* `folder` is created when the clip is saved. `meta` is the body of a JSON
   object without its braces. Room is made for `seconds` of recording. */
int imager_clip_begin(imager_clip_t *clip, const char *folder, const char *meta,
                      double seconds, bool pictures);
/* The source starts now: splits the quiet part from the source part. */
void imager_clip_source(imager_clip_t *clip, uint64_t now_us);
bool imager_clip_recording(const imager_clip_t *clip);

/* From the audio thread. They take a short lock and never touch the disk. */
void imager_clip_audio(imager_clip_t *clip, const void *raw, size_t frames,
                       uint64_t first_frame);
void imager_clip_row(imager_clip_t *clip, const imager_clip_row_t *row);

/* From the display thread. A new number for the camera and overlay pictures
   of one frame, or -1 if this clip saves no pictures. */
int32_t imager_clip_next_picture(imager_clip_t *clip);
/* Copies the picture for compression. false (and counted) if the background
   thread has fallen behind: a picture is lost rather than the display held. */
bool imager_clip_picture(imager_clip_t *clip, const auris_rgb24_t *canvas,
                         unsigned int x, unsigned int y, int32_t number, bool overlay);
void imager_clip_frame(imager_clip_t *clip, const imager_clip_frame_t *frame);

/* Stops recording and starts saving. */
int imager_clip_end(imager_clip_t *clip);
/* Stops recording and forgets it. */
void imager_clip_cancel(imager_clip_t *clip);
/* 1 while saving, 0 once saved, negative errno if the save failed. */
int imager_clip_poll(imager_clip_t *clip, imager_clip_summary_t *summary);
void imager_clip_close(imager_clip_t *clip);

/* Moves `folder` to the first free name folder.tagN. Returns 0 or -errno;
   0 if there is no such folder. A clip that was set aside is never deleted:
   it may be a real result of the system failing. */
int imager_clip_set_aside(const char *folder, const char *tag);

#endif
