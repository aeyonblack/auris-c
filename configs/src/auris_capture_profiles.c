#include "auris_array_configs.h"

/* XVF3000 6-channel firmware: processed audio, 4 raw mics, playback. */
static const auris_capture_profile_t RESPEAKER_USB = {
    .device = "hw:CARD=ArrayUAC10,DEV=0",
    .sample_rate = 16000U,
    .raw_channels = 6U,
    .format = AURIS_SAMPLE_S16_LE,
    .frame_size = 512U, /* 32 ms */
    .frame_hop = 256U,
    .gcc_upsample = 8U, /* 7.8 us steps */
    .band_low_hz = 500.0f,
    .band_high_hz = 3000.0f,
    .gate_minimum_dbfs = -65.0f,
    .snr_weighting = false,
    .weighted_low_hz = 500.0f,
    .coherence_min = 0.0f};

/* McASP6 receiving four I2S lines from two PCM1865s (CMB is clock master):
   2 slots x 32 bits per frame, 24-bit samples left-justified. */
static const auris_capture_profile_t CMB = {
    .device = "hw:AurisCMB",
    .sample_rate = 48000U,
    .raw_channels = 8U,
    .format = AURIS_SAMPLE_S32_LE,
    .frame_size = 1024U, /* 21.3 ms, 46.875 Hz bins */
    .frame_hop = 512U,
    .gcc_upsample = 4U, /* 5.2 us steps */
    /* Below 500 Hz the room rumble is coherent and arrives with almost no
       delay; 4 kHz stays under c / 2d for neighbouring mics (4.3 kHz). */
    .band_low_hz = 500.0f,
    .band_high_hz = 4000.0f,
    /* Quiet room: -75.6 dBFS in band. -65 would clamp open (+10 dB) and
       close (+6 dB) to the same level; -70 keeps both margins. */
    .gate_minimum_dbfs = -70.0f,
    /* Bins are weighted by their SNR against the learned noise, so the
       band can reach down to where voices are strongest: the rumble below
       500 Hz is in the noise estimate and gets no vote on its own. */
    .snr_weighting = true,
    .weighted_low_hz = 200.0f,
    /* Unrelated noise on eight microphones scores below 0.1; the weakest
       real source in the room1 test set scored 0.26. */
    .coherence_min = 0.15f};

const auris_capture_profile_t *auris_capture_respeaker_usb(void) {
  return &RESPEAKER_USB;
}

const auris_capture_profile_t *auris_capture_cmb(void) { return &CMB; }
