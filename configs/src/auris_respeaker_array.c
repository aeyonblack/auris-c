#include "auris_array.h"
#include "auris_array_configs.h"

static const auris_array_t RESPEAKER_USB_4 = {
    .name = "Respeaker USB 4-Mic Array",
    .microphone_count = 4U,
    .positions =
        {{-0.02285F, 0.02285F, 0.000F},   /* MIC1: upper-left from behind*/
         {0.02285F, 0.02285F, 0.000F},    /* MIC2: upper-right from behind*/
         {0.02285F, -0.02285F, 0.000F},   /* MIC3: lower-right from behind*/
         {-0.02285F, -0.02285F, 0.000F}}, /* MIC4: lower-left from behind*/
    .capture_channels = {1U, 2U, 3U, 4U},
    .speed_of_sound_mps = 343.0F};

const auris_array_t *auris_config_respeaker_usb_4(void) {
  return &RESPEAKER_USB_4;
}
