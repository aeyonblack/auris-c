/* The TDA4VM's image processor through TI's OpenVX (TIOVX) libraries:
   VISS (raw to NV12, with TI's tuning) and MSC (scaling) in one graph,
   run once per frame, and TI's 2A on the statistics VISS gathers. Our own
   V4L2 capture gives the frames, so any mode the sensor offers can be used
   (TI's GStreamer elements cannot take 10-bit or row-padded raw frames). */
#include "auris_vpac.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <TI/j7.h>
#include <TI/tivx.h>
#include <app_init.h>
#include <ti_2a_wrapper.h>
#include <tiovx_multi_scaler_module.h>
#include <tiovx_sensor_module.h>
#include <tiovx_viss_module.h>

#define SENSOR_NAME "SENSOR_SONY_IMX219_RPI"
/* VISS white-balance gain for x1. */
#define WB_UNIT 512U

struct auris_vpac {
  auris_vpac_config_t config;
  bool app, kernels, viss_ready, msc_ready, aewb_ready;
  vx_context context;
  vx_graph graph;
  SensorObj sensor;
  TIOVXVISSModuleObj viss;
  TIOVXMultiScalerModuleObj msc;
  TI_2A_wrapper aewb;
  tivx_aewb_config_t aewb_config;
  sensor_config_get aewb_in;
  sensor_config_set aewb_out;
  uint8_t *aewb_tuning;
  vx_map_id luma_map, chroma_map;
  bool mapped;
  bool statistics; /* H3A connected, so TI's 2A runs */
};

static bool ok(vx_status status) { return status == (vx_status)VX_SUCCESS; }

/* Reads a whole tuning file into memory. */
static int read_file(const char *path, uint8_t **data, uint32_t *size) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return -errno;
  int err = -EINVAL;
  long length = -1;
  if (fseek(f, 0L, SEEK_END) == 0 && (length = ftell(f)) > 0 &&
      length < (1L << 24) && fseek(f, 0L, SEEK_SET) == 0) {
    *data = malloc((size_t)length);
    if (*data == NULL)
      err = -ENOMEM;
    else if (fread(*data, 1U, (size_t)length, f) == (size_t)length) {
      *size = (uint32_t)length;
      err = 0;
    } else {
      free(*data);
      *data = NULL;
    }
  }
  (void)fclose(f);
  return err;
}

static void unmap_picture(auris_vpac_t *s) {
  if (!s->mapped)
    return;
  vx_image out = s->msc.output[0].image_handle[0];
  (void)vxUnmapImagePatch(out, s->luma_map);
  (void)vxUnmapImagePatch(out, s->chroma_map);
  s->mapped = false;
}

void auris_vpac_close(auris_vpac_t *s) {
  if (s == NULL)
    return;
  unmap_picture(s);
  if (s->aewb_ready)
    (void)TI_2A_wrapper_delete(&s->aewb);
  free(s->aewb_tuning);
  if (s->msc_ready)
    (void)tiovx_multi_scaler_module_delete(&s->msc);
  if (s->viss_ready)
    (void)tiovx_viss_module_delete(&s->viss);
  if (s->graph != NULL)
    (void)vxReleaseGraph(&s->graph);
  if (s->msc_ready)
    (void)tiovx_multi_scaler_module_deinit(&s->msc);
  if (s->viss_ready)
    (void)tiovx_viss_module_deinit(&s->viss);
  if (s->viss_ready)
    tiovx_deinit_sensor(&s->sensor);
  if (s->kernels)
    tivxHwaUnLoadKernels(s->context);
  if (s->context != NULL)
    (void)vxReleaseContext(&s->context);
  if (s->app)
    (void)appDeInit();
  free(s);
}

static int build_graph(auris_vpac_t *s) {
  const auris_vpac_config_t *c = &s->config;
  if (!ok(tiovx_querry_sensor(&s->sensor)) ||
      !ok(tiovx_init_sensor(&s->sensor, (char *)SENSOR_NAME)))
    return -EIO;

  TIOVXVISSModuleObj *v = &s->viss;
  snprintf(v->dcc_config_file_path, sizeof(v->dcc_config_file_path), "%s",
           c->isp_tuning);
  v->input.bufq_depth = 1;
  v->input.params.width = c->source_width;
  v->input.params.height = c->source_height;
  v->input.params.num_exposures = 1U;
  v->input.params.line_interleaved = vx_false_e;
  /* TI's IMX219 tuning is for 8-bit samples: 10-bit ones overrun it and
     come out washed out, so the processor is always given 8 bits. */
  v->input.params.format[0].pixel_container = (vx_uint32)TIVX_RAW_IMAGE_8_BIT;
  v->input.params.format[0].msb = 7U;
  v->input.params.meta_height_before = 0U;
  v->input.params.meta_height_after = 0U;
  v->ae_awb_result_bufq_depth = 1;
  v->h3a_stats_bufq_depth = 1;
  for (int i = 0; i < 5; ++i)
    v->output_select[i] = TIOVX_VISS_MODULE_OUTPUT_NA;
  v->output_select[2] = TIOVX_VISS_MODULE_OUTPUT_EN;
  v->output2.width = (vx_int32)c->source_width;
  v->output2.height = (vx_int32)c->source_height;
  v->output2.color_format = VX_DF_IMAGE_NV12;
  v->output2.bufq_depth = 1;
  if (!ok(tiovx_viss_module_init(s->context, v, &s->sensor)))
    return -EIO;
  s->viss_ready = true;

  TIOVXMultiScalerModuleObj *m = &s->msc;
  m->num_channels = 1;
  m->num_outputs = 1;
  m->color_format = VX_DF_IMAGE_NV12;
  m->interpolation_method = VX_INTERPOLATION_BILINEAR;
  m->input.width = (vx_int32)c->source_width;
  m->input.height = (vx_int32)c->source_height;
  m->input.color_format = VX_DF_IMAGE_NV12;
  m->input.bufq_depth = 1;
  m->output[0].width = (vx_int32)c->width;
  m->output[0].height = (vx_int32)c->height;
  m->output[0].color_format = VX_DF_IMAGE_NV12;
  m->output[0].bufq_depth = 1;
  if (!ok(tiovx_multi_scaler_module_init(s->context, m)))
    return -EIO;
  s->msc_ready = true;

  s->graph = vxCreateGraph(s->context);
  if (!ok(vxGetStatus((vx_reference)s->graph)))
    return -EIO;
  if (s->statistics) {
    if (!ok(tiovx_viss_module_create(s->graph, v, NULL, NULL,
                                     TIVX_TARGET_VPAC_VISS1)))
      return -EIO;
  } else {
    /* The same node without the H3A statistics output. */
    vx_reference raw = vxGetObjectArrayItem(v->input.arr[0], 0);
    vx_reference aewb = vxGetObjectArrayItem(v->ae_awb_result_arr[0], 0);
    vx_reference out2 = vxGetObjectArrayItem(v->output2.arr[0], 0);
    v->node = tivxVpacVissNode(s->graph, v->config,
                               (vx_user_data_object)aewb, v->dcc_config,
                               (tivx_raw_image)raw, NULL, NULL,
                               (vx_image)out2, NULL, NULL, NULL, NULL, NULL,
                               NULL);
    (void)vxReleaseReference(&raw);
    (void)vxReleaseReference(&aewb);
    (void)vxReleaseReference(&out2);
    if (!ok(vxGetStatus((vx_reference)v->node)) ||
        !ok(vxSetNodeTarget(v->node, VX_TARGET_STRING,
                            TIVX_TARGET_VPAC_VISS1)))
      return -EIO;
  }
  if (!ok(tiovx_multi_scaler_module_create(s->graph, m, v->output2.arr[0],
                                           TIVX_TARGET_VPAC_MSC1)) ||
      !ok(vxVerifyGraph(s->graph)) ||
      !ok(tiovx_multi_scaler_module_update_filter_coeffs(m)))
    return -EIO;

  /* Before any statistics: no 2A result, so VISS uses the tuning's own
     starting point. */
  tivx_ae_awb_params_t *start = NULL;
  vx_map_id id;
  if (!ok(vxMapUserDataObject(v->ae_awb_result_handle[0], 0,
                              sizeof(tivx_ae_awb_params_t), &id,
                              (void **)&start, VX_WRITE_ONLY,
                              VX_MEMORY_TYPE_HOST, 0)))
    return -EIO;
  memset(start, 0, sizeof(*start));
  start->h3a_source_data = TIVX_VPAC_VISS_H3A_IN_LSC;
  (void)vxUnmapUserDataObject(v->ae_awb_result_handle[0], id);
  return 0;
}

static int start_aewb(auris_vpac_t *s) {
  const auris_vpac_config_t *c = &s->config;
  uint32_t size = 0U;
  const int err = read_file(c->aewb_tuning, &s->aewb_tuning, &size);
  if (err < 0)
    return err;
  s->aewb_config = (tivx_aewb_config_t){
      .sensor_dcc_id = (uint16_t)s->sensor.sensorParams.dccId,
      .sensor_img_format = 0U, /* Bayer */
      .sensor_img_phase = 3U,  /* RGGB */
      .awb_mode = (uint8_t)ALGORITHMS_ISS_AWB_AUTO,
      .ae_mode = (uint8_t)ALGORITHMS_ISS_AE_AUTO,
      .awb_num_skip_frames = 0U,
      .ae_num_skip_frames = 0U,
      .channel_id = 0U};
  memset(&s->aewb, 0, sizeof(s->aewb));
  if (TI_2A_wrapper_create(&s->aewb, &s->aewb_config, s->aewb_tuning, size) !=
      0)
    return -EIO;
  s->aewb_ready = true;
  /* TI's IMX219 settings (from its GStreamer element), with the exposure
     and gain limits of the mode in use. */
  IssAeDynamicParams *p = &s->aewb_in.ae_dynPrms;
  memset(p, 0, sizeof(*p));
  p->targetBrightnessRange.min = 40U;
  p->targetBrightnessRange.max = 50U;
  p->targetBrightness = 45U;
  p->threshold = 1U;
  p->enableBlc = 1U;
  p->exposureTimeStepSize = 1U;
  p->exposureTimeRange[0].min = c->min_exposure_us;
  p->exposureTimeRange[0].max = c->max_exposure_us;
  p->analogGainRange[0].min = 1024U;
  p->analogGainRange[0].max = (uint32_t)(c->max_analogue * 1024.0f);
  p->digitalGainRange[0].min = 256U;
  p->digitalGainRange[0].max = 256U;
  p->numAeDynParams = 1U;
  return 0;
}

int auris_vpac_open(auris_vpac_t **out, const auris_vpac_config_t *c) {
  if (out == NULL || *out != NULL || c == NULL || c->isp_tuning == NULL ||
      (c->statistics && c->aewb_tuning == NULL) ||
      (c->bits != 8U && c->bits != 10U) ||
      c->source_width == 0U || c->source_height == 0U || c->width < 2U ||
      c->height < 2U || c->width % 2U != 0U || c->height % 2U != 0U ||
      c->source_stride <
          (size_t)c->source_width * (c->bits == 8U ? 1U : 2U) ||
      (c->statistics &&
       (c->min_exposure_us == 0U || c->max_exposure_us < c->min_exposure_us ||
        !(c->max_analogue >= 1.0f))))
    return -EINVAL;
  /* TI's start-up maps /dev/mem and aborts the process if it cannot. */
  if (geteuid() != 0)
    return -EPERM;
  if (access(c->isp_tuning, R_OK) != 0 ||
      (c->statistics && access(c->aewb_tuning, R_OK) != 0))
    return -ENOENT;
  auris_vpac_t *s = calloc(1U, sizeof(*s));
  if (s == NULL)
    return -ENOMEM;
  s->config = *c;
  int err = -EIO;
  if (appInit() != 0)
    goto fail;
  s->app = true;
  s->context = vxCreateContext();
  if (!ok(vxGetStatus((vx_reference)s->context)))
    goto fail;
  tivxHwaLoadKernels(s->context);
  s->kernels = true;
  s->statistics = c->statistics;
  err = build_graph(s);
  if (err == 0 && s->statistics)
    err = start_aewb(s);
  if (err < 0)
    goto fail;
  *out = s;
  return 0;
fail:
  auris_vpac_close(s);
  return err;
}

/* The top 8 bits of a row of 10-bit samples. */
static void narrow_row(uint8_t *dst, const uint16_t *src, size_t count) {
  size_t i = 0U;
#if defined(__ARM_NEON)
  for (; i + 16U <= count; i += 16U) {
    const uint16x8_t a = vld1q_u16(src + i), b = vld1q_u16(src + i + 8U);
    vst1q_u8(dst + i, vcombine_u8(vqshrn_n_u16(a, 2), vqshrn_n_u16(b, 2)));
  }
#endif
  for (; i < count; ++i) {
    const uint16_t v = (uint16_t)(src[i] >> 2);
    dst[i] = v > 255U ? 255U : (uint8_t)v;
  }
}

static int copy_in(auris_vpac_t *s, const uint8_t *raw) {
  const auris_vpac_config_t *c = &s->config;
  tivx_raw_image image = s->viss.input.image_handle[0];
  vx_rectangle_t rect = {0U, 0U, c->source_width, c->source_height};
  vx_imagepatch_addressing_t addr;
  vx_map_id id;
  void *dst = NULL;
  if (!ok(tivxMapRawImagePatch(image, &rect, 0U, &id, &addr, &dst,
                               VX_WRITE_ONLY, VX_MEMORY_TYPE_HOST,
                               TIVX_RAW_IMAGE_PIXEL_BUFFER)))
    return -EIO;
  const size_t row = c->source_width; /* 8-bit samples in */
  if (addr.stride_y < 0 || (size_t)addr.stride_y < row) {
    (void)tivxUnmapRawImagePatch(image, id);
    return -EIO;
  }
  if (c->bits == 10U) {
    for (uint32_t y = 0U; y < c->source_height; ++y)
      narrow_row((uint8_t *)dst + (size_t)y * (size_t)addr.stride_y,
                 (const uint16_t *)(const void *)(raw +
                                                  (size_t)y * c->source_stride),
                 c->source_width);
  } else if ((size_t)addr.stride_y == c->source_stride) {
    memcpy(dst, raw, c->source_stride * (c->source_height - 1U) + row);
  } else {
    for (uint32_t y = 0U; y < c->source_height; ++y)
      memcpy((uint8_t *)dst + (size_t)y * (size_t)addr.stride_y,
             raw + (size_t)y * c->source_stride, row);
  }
  (void)tivxUnmapRawImagePatch(image, id);
  return 0;
}

/* TI's 2A on the frame's statistics; its result sets VISS's white balance
   for the next frame and says what exposure the sensor should use. */
static int run_aewb(auris_vpac_t *s, auris_vpac_result_t *result) {
  vx_user_data_object stats_object = s->viss.h3a_stats_handle[0];
  vx_user_data_object result_object = s->viss.ae_awb_result_handle[0];
  tivx_h3a_data_t *stats = NULL;
  tivx_ae_awb_params_t *aewb = NULL;
  vx_map_id stats_id, result_id;
  if (!ok(vxMapUserDataObject(stats_object, 0, sizeof(tivx_h3a_data_t),
                              &stats_id, (void **)&stats, VX_READ_ONLY,
                              VX_MEMORY_TYPE_HOST, 0)))
    return -EIO;
  if (!ok(vxMapUserDataObject(result_object, 0, sizeof(tivx_ae_awb_params_t),
                              &result_id, (void **)&aewb, VX_READ_AND_WRITE,
                              VX_MEMORY_TYPE_HOST, 0))) {
    (void)vxUnmapUserDataObject(stats_object, stats_id);
    return -EIO;
  }
  memset(&s->aewb_out, 0, sizeof(s->aewb_out));
  const int32_t status = TI_2A_wrapper_process(
      &s->aewb, &s->aewb_config, stats, &s->aewb_in, aewb, &s->aewb_out);
  *result = (auris_vpac_result_t){
      .exposure_us = s->aewb_out.aePrms.exposureTime[0],
      .analogue_q10 = s->aewb_out.aePrms.analogGain[0],
      .exposure_valid = status == 0 && s->aewb_out.aePrms.exposureTime[0] > 0U &&
                        s->aewb_out.aePrms.analogGain[0] > 0U,
      .colour_temperature = aewb->color_temperature};
  memcpy(result->wb_gains, aewb->wb_gains, sizeof(result->wb_gains));
  if (aewb->wb_gains[1] > 0U) {
    result->balance_red = (float)aewb->wb_gains[0] / (float)aewb->wb_gains[1];
    result->balance_blue = (float)aewb->wb_gains[3] / (float)aewb->wb_gains[1];
  }
  (void)vxUnmapUserDataObject(result_object, result_id);
  (void)vxUnmapUserDataObject(stats_object, stats_id);
  return status == 0 ? 0 : -EIO;
}

int auris_vpac_set_balance(auris_vpac_t *s, float red, float blue,
                           uint32_t exposure_us, float analogue,
                           uint32_t colour_temperature) {
  if (s == NULL || !(red > 0.0f) || !(blue > 0.0f) || !(analogue >= 1.0f))
    return -EINVAL;
  vx_user_data_object object = s->viss.ae_awb_result_handle[0];
  tivx_ae_awb_params_t *p = NULL;
  vx_map_id id;
  if (!ok(vxMapUserDataObject(object, 0, sizeof(*p), &id, (void **)&p,
                              VX_WRITE_ONLY, VX_MEMORY_TYPE_HOST, 0)))
    return -EIO;
  const float unit = (float)WB_UNIT;
  *p = (tivx_ae_awb_params_t){
      .h3a_source_data = TIVX_VPAC_VISS_H3A_IN_LSC,
      .exposure_time = exposure_us,
      .analog_gain = (uint32_t)(analogue * 1024.0f),
      .ae_valid = 1U,
      .ae_converged = 1U,
      .digital_gain = 256U,
      .wb_gains = {(uint32_t)(red * unit + 0.5f), WB_UNIT, WB_UNIT,
                   (uint32_t)(blue * unit + 0.5f)},
      .color_temperature = colour_temperature,
      .awb_valid = 1U,
      .awb_converged = 1U};
  (void)vxUnmapUserDataObject(object, id);
  return 0;
}

static int map_picture(auris_vpac_t *s, auris_nv12_t *picture) {
  vx_image out = s->msc.output[0].image_handle[0];
  vx_rectangle_t rect = {0U, 0U, s->config.width, s->config.height};
  vx_imagepatch_addressing_t luma, chroma;
  void *y = NULL, *uv = NULL;
  if (!ok(vxMapImagePatch(out, &rect, 0U, &s->luma_map, &luma, &y,
                          VX_READ_ONLY, VX_MEMORY_TYPE_HOST, VX_NOGAP_X)))
    return -EIO;
  if (!ok(vxMapImagePatch(out, &rect, 1U, &s->chroma_map, &chroma, &uv,
                          VX_READ_ONLY, VX_MEMORY_TYPE_HOST, VX_NOGAP_X))) {
    (void)vxUnmapImagePatch(out, s->luma_map);
    return -EIO;
  }
  s->mapped = true;
  *picture = (auris_nv12_t){y,
                            uv,
                            s->config.width,
                            s->config.height,
                            (size_t)luma.stride_y,
                            (size_t)chroma.stride_y};
  return 0;
}

int auris_vpac_process(auris_vpac_t *s, const void *raw, size_t bytes,
                       auris_nv12_t *picture, auris_vpac_result_t *result) {
  if (s == NULL || raw == NULL || picture == NULL || result == NULL)
    return -EINVAL;
  const auris_vpac_config_t *c = &s->config;
  if (bytes < c->source_stride * (c->source_height - 1U) +
                  (size_t)c->source_width * (c->bits == 8U ? 1U : 2U))
    return -EINVAL;
  unmap_picture(s);
  int err = copy_in(s, raw);
  if (err < 0)
    return err;
  if (!ok(vxProcessGraph(s->graph)))
    return -EIO;
  *result = (auris_vpac_result_t){0};
  if (s->statistics) {
    err = run_aewb(s, result);
    if (err < 0)
      return err;
  }
  return map_picture(s, picture);
}
