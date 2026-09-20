#include "auris_display.h"
#include <errno.h>
int auris_output_open_display_info(auris_output_t **out,uint32_t width,uint32_t height,
                              const auris_display_config_t *config,auris_display_info_t *info) {
  (void)info;
  return auris_output_open_display(out,width,height,config);
}
int auris_output_open_display(auris_output_t **out,uint32_t width,uint32_t height,
                              const auris_display_config_t *config) {
  (void)out; (void)width; (void)height; (void)config;
  return -ENOTSUP;
}
