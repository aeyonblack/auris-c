#ifndef AURIS_STREAM_H
#define AURIS_STREAM_H
#include "auris_output.h"
int auris_output_open_jpeg_stream(auris_output_t **out, int fd,
                                  uint32_t width, uint32_t height,
                                  unsigned int quality);
#endif
