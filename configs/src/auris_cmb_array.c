#include "auris_array.h"
#include "auris_array_configs.h"

#define CMB_R 0.052F
#define CMB_D 0.036769553F

static const auris_array_t CMB_8 = {
    .name = "Auris CMB 8-Mic Ring",
    .microphone_count = 8U,
    .positions = {{CMB_R, 0.0F, 0.0F},    /* MIC1:   0 deg, right from behind */
                  {CMB_D, CMB_D, 0.0F},   /* MIC2:  45 deg                    */
                  {0.0F, CMB_R, 0.0F},    /* MIC3:  90 deg, top               */
                  {-CMB_D, CMB_D, 0.0F},  /* MIC4: 135 deg                    */
                  {-CMB_R, 0.0F, 0.0F},   /* MIC5: 180 deg, left from behind  */
                  {-CMB_D, -CMB_D, 0.0F}, /* MIC6: 225 deg                    */
                  {0.0F, -CMB_R, 0.0F},   /* MIC7: 270 deg, bottom            */
                  {CMB_D, -CMB_D, 0.0F}}, /* MIC8: 315 deg                    */
    /* ALSA channel per mic; ALSA order is MIC1,4,5,8,2,3,6,7 (tap-tested). */
    .capture_channels = {0U, 4U, 5U, 1U, 2U, 6U, 7U, 3U},
    .speed_of_sound_mps = 343.0F};

const auris_array_t *auris_config_cmb_8(void) { return &CMB_8; }
