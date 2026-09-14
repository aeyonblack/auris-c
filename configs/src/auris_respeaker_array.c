#include "auris_array.h"
#include "auris_array_configs.h"

static const auris_array_t RESPEAKER_USB_4 = {
    .name = "Respeaker USB 4-Mic Array",
    .microphone_count = 4U,
    .positions = {{-0.032F, 0.000F, 0.000F},
                  {0.000F, -0.032F, 0.000F},
                  {0.032F, 0.000F, 0.000F},
                  {0.000F, 0.032F, 0.000F}},
    .capture_channels = {2U, 3U, 4U, 5U},
    .speed_of_sound_mps = 343.0F};

const auris_array_t *auris_config_respeaker_usb_4(void) {
  return &RESPEAKER_USB_4;
}
