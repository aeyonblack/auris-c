#include "auris_view.h"
#include "auris_wav.h"
#include "imager_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLUMNS IMAGER_COLUMNS
#define ROWS IMAGER_ROWS
#define DIRECTIONS IMAGER_DIRECTIONS
#define BLOB_CUTOFF 0.55f /* the overlay's default cutoff */

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s --file REC.wav [--array NAME] [--band LO HI]"
          " [--step 1|2|4]\n"
          "          [--no-gate] [--no-smooth] [--weight | --no-weight]\n"
          "          [--diffuse 0..1]   (0: no diffuse-field rejection)\n"
          "          [--height HZ KEEP SHARE]   (bins below HZ keep KEEP of"
          " their say on height once\n"
          "                                      the bins above weigh SHARE of"
          " them; 0 1 0: off)\n"
          "          [--coherence MIN] [--bias B] [--weight-open BINS]"
          " [--weight-close BINS]\n"
          "          [--no-track] [--kappa K] [--drift CELLS] [--jump RATE]"
          " [--blob CELLS] [--hold S]\n"
          "  NAME: %s (default cmb)\n"
          "  Replays a recording through the imager's DSP and prints one"
          " line per analysis frame.\n",
          program, auris_config_names());
  return EXIT_FAILURE;
}

/* Cells whose score lies above `cutoff` of the map's range: the area the
   overlay would colour. */
static size_t blob_cells(const float *scores, size_t count, float cutoff) {
  float low = INFINITY, high = -INFINITY;
  for (size_t q = 0U; q < count; ++q) {
    low = fminf(low, scores[q]);
    high = fmaxf(high, scores[q]);
  }
  if (!(high > low)) {
    return 0U;
  }
  const float threshold = low + cutoff * (high - low);
  size_t cells = 0U;
  for (size_t q = 0U; q < count; ++q) {
    cells += scores[q] >= threshold ? 1U : 0U;
  }
  return cells;
}

int main(int argc, char **argv) {
  imager_audio_options_t options = {.array = "cmb", .step = 1U};
  const char *file = NULL;
  bool gating = true, smoothing = true;
  for (int i = 1; i < argc; ++i) {
    char *end = NULL, *end2 = NULL;
    if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
      file = argv[++i];
    } else if (strcmp(argv[i], "--array") == 0 && i + 1 < argc) {
      options.array = argv[++i];
    } else if (strcmp(argv[i], "--band") == 0 && i + 2 < argc) {
      options.band_low_hz = strtof(argv[i + 1], &end);
      options.band_high_hz = strtof(argv[i + 2], &end2);
      options.band_given = true;
      if (end == argv[i + 1] || *end != '\0' || end2 == argv[i + 2] ||
          *end2 != '\0') {
        return usage(argv[0]);
      }
      i += 2;
    } else if (strcmp(argv[i], "--step") == 0 && i + 1 < argc) {
      options.step = strtoul(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--no-weight") == 0) {
      options.weighting = -1;
    } else if (strcmp(argv[i], "--weight") == 0) {
      options.weighting = 1;
    } else if (strcmp(argv[i], "--coherence") == 0 && i + 1 < argc) {
      options.coherence_min = strtof(argv[++i], &end);
      options.coherence_given = true;
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--bias") == 0 && i + 1 < argc) {
      options.noise_bias = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--weight-open") == 0 && i + 1 < argc) {
      options.weight_open = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--weight-close") == 0 && i + 1 < argc) {
      options.weight_close = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--diffuse") == 0 && i + 1 < argc) {
      options.diffuse = strtof(argv[++i], &end);
      options.diffuse_given = true;
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--height") == 0 && i + 3 < argc) {
      char *end3 = NULL;
      options.height_split_hz = strtof(argv[i + 1], &end);
      options.height_keep = strtof(argv[i + 2], &end2);
      options.height_share = strtof(argv[i + 3], &end3);
      options.height_given = true;
      if (end == argv[i + 1] || *end != '\0' || end2 == argv[i + 2] ||
          *end2 != '\0' || end3 == argv[i + 3] || *end3 != '\0') {
        return usage(argv[0]);
      }
      i += 3;
    } else if (strcmp(argv[i], "--no-track") == 0) {
      options.tracking = -1;
    } else if (strcmp(argv[i], "--kappa") == 0 && i + 1 < argc) {
      options.track_kappa = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--drift") == 0 && i + 1 < argc) {
      options.track_drift = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--jump") == 0 && i + 1 < argc) {
      options.track_jump = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--blob") == 0 && i + 1 < argc) {
      options.track_display = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
      options.hold_s = strtof(argv[++i], &end);
      if (end == argv[i] || *end != '\0') return usage(argv[0]);
    } else if (strcmp(argv[i], "--no-gate") == 0) {
      gating = false;
    } else if (strcmp(argv[i], "--no-smooth") == 0) {
      smoothing = false;
    } else {
      return usage(argv[0]);
    }
  }
  imager_audio_plan_t plan;
  if (file == NULL || imager_audio_plan(&options, &plan) < 0) {
    return usage(argv[0]);
  }
  const auris_array_t *array = plan.config.array;

  const auris_view_t view = imager_search_view();
  static auris_vec3f_t grid[DIRECTIONS];
  static imager_dsp_t dsp;
  static auris_framer_t framer;
  static auris_analysis_frame_t analysis;
  static auris_srp_result_t result;
  auris_audio_block_t block;
  auris_wav_reader_t recording = {0};
  int status = EXIT_FAILURE;
  if (auris_view_grid(&view, COLUMNS, ROWS, grid, DIRECTIONS) != AURIS_OK ||
      imager_dsp_open(&dsp, &plan, grid, COLUMNS, ROWS, gating, smoothing) < 0 ||
      auris_framer_init(&framer, plan.frame_size, plan.frame_hop) != AURIS_OK) {
    fprintf(stderr, "DSP setup failed\n");
    goto done;
  }
  const int err = auris_wav_open(&recording, file, plan.config.capture,
                                 array->capture_channels,
                                 array->microphone_count);
  if (err < 0) {
    fprintf(stderr, "%s: %s\n", file,
            err == -EINVAL ? "recording does not match this array's device"
                           : strerror(-err));
    goto done;
  }
  /* Header, then one line per analysis frame. Times are frame starts. */
  printf("AURIS9 %u %zu %zu %u %u %.9g %.9g %d %d\n", plan.rate,
         plan.frame_size, plan.frame_hop, COLUMNS, ROWS,
         (double)plan.band_low_hz, (double)plan.band_high_hz, (int)gating,
         (int)smoothing);
  puts("# time_s level_dbfs noise_dbfs open_dbfs ready active evidence valid "
       "column row x y z peak contrast blob_cells weight_bins coherence "
       "fresh strength confidence");
  for (;;) {
    if (auris_wav_read(&recording, &block) != AURIS_OK) {
      fprintf(stderr, "read failed\n");
      goto done;
    }
    if (block.frames == 0U) {
      break;
    }
    bool ready = false;
    if (auris_framer_push(&framer, &block, &analysis, &ready) != AURIS_OK) {
      fprintf(stderr, "invalid or discontinuous audio\n");
      goto done;
    }
    if (!ready) {
      continue;
    }
    const double time_s = (double)analysis.first_frame / (double)plan.rate;
    auris_activity_result_t gate;
    imager_view_t shown;
    if (imager_dsp_process(&dsp, &analysis, &gate, &result, &shown) !=
        AURIS_OK) {
      fprintf(stderr, "DSP failed\n");
      goto done;
    }
    /* `valid` is what the imager would draw this frame: with the tracker
       that includes held estimates; `fresh` says the frame added evidence. */
    const bool mapped = gate.active && gate.evidence;
    const bool fresh = shown.valid && shown.fresh;
    auris_vec3f_t ray = {0};
    if (shown.valid) {
      const float u = shown.column / (float)(COLUMNS - 1U) *
                      (float)(view.width - 1U);
      const float v =
          shown.row / (float)(ROWS - 1U) * (float)(view.height - 1U);
      if (auris_view_ray(&view, u, v, &ray) != AURIS_OK) {
        fprintf(stderr, "estimate outside the view\n");
        goto done;
      }
    }
    printf("%.5f %.2f %.2f %.2f %d %d %d %d %.3f %.3f %.6f %.6f %.6f %.6f "
           "%.5f %zu %.3f %.4f %d %.3f %.3f\n",
           time_s, (double)gate.level_dbfs, (double)gate.noise_dbfs,
           (double)gate.open_dbfs, (int)gate.ready, (int)gate.active,
           (int)gate.evidence, (int)shown.valid,
           shown.valid ? (double)shown.column : -1.0,
           shown.valid ? (double)shown.row : -1.0, (double)ray.x_m,
           (double)ray.y_m, (double)ray.z_m, mapped ? (double)result.peak : 0.0,
           mapped ? (double)result.contrast : 0.0,
           shown.valid ? blob_cells(shown.map, DIRECTIONS, BLOB_CUTOFF) : 0U,
           (double)gate.weight_sum, mapped ? (double)result.coherence : 0.0,
           (int)fresh, (double)shown.strength, (double)shown.confidence);
  }
  status = fflush(stdout) == EOF ? EXIT_FAILURE : EXIT_SUCCESS;
done:
  auris_wav_close(&recording);
  imager_dsp_close(&dsp);
  return status;
}
