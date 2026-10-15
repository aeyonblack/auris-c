add_library(auris_camera STATIC src/auris_camera.c)
target_include_directories(auris_camera PUBLIC inc)
target_compile_definitions(auris_camera PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(auris_camera)
auris_enable_sanitizers(auris_camera)

target_link_libraries(auris_camera PUBLIC m)

# The TDA4VM's image processor, through TI's vision libraries when they are
# installed (the BeagleBone's image); elsewhere a stand-in that reports it
# missing, so the camera chain falls back to software processing.
set(AURIS_TI_SDK /usr/include/processor_sdk)
find_library(AURIS_TIVISION_APPS tivision_apps)
find_library(AURIS_TIOVX_MODULES edgeai-tiovx-modules)
find_path(AURIS_TIOVX_INCLUDE TI/tivx.h PATHS ${AURIS_TI_SDK}/tiovx/include
          NO_DEFAULT_PATH)
if(AURIS_TIVISION_APPS AND AURIS_TIOVX_MODULES AND AURIS_TIOVX_INCLUDE)
  add_library(auris_vpac STATIC src/auris_vpac.c)
  target_include_directories(
    auris_vpac SYSTEM PRIVATE
      ${AURIS_TI_SDK}/tiovx/include
      ${AURIS_TI_SDK}/tiovx/kernels/include
      ${AURIS_TI_SDK}/tiovx/kernels_j7/include
      ${AURIS_TI_SDK}/tiovx/utils/include
      ${AURIS_TI_SDK}/vision_apps
      ${AURIS_TI_SDK}/vision_apps/kernels/img_proc/include
      ${AURIS_TI_SDK}/vision_apps/kernels/fileio/include
      ${AURIS_TI_SDK}/vision_apps/utils/app_init/include
      ${AURIS_TI_SDK}/imaging/kernels/include
      ${AURIS_TI_SDK}/imaging/algos/dcc/include
      ${AURIS_TI_SDK}/imaging/algos/ae/include
      ${AURIS_TI_SDK}/imaging/algos/awb/include
      ${AURIS_TI_SDK}/imaging/sensor_drv/include
      ${AURIS_TI_SDK}/imaging/ti_2a_wrapper/include
      ${AURIS_TI_SDK}/tidl_j721e_08_02_00_11/ti_dl/inc
      ${AURIS_TI_SDK}/ivision
      ${AURIS_TI_SDK}/imaging
      /usr/include/edgeai-tiovx-modules/include)
  target_compile_definitions(auris_vpac PRIVATE SOC_J721E J721E TARGET_LINUX
                                                _POSIX_C_SOURCE=200809L)
  target_link_libraries(auris_vpac PRIVATE ${AURIS_TIOVX_MODULES}
                                           ${AURIS_TIVISION_APPS})
  set(AURIS_VPAC_AVAILABLE ON CACHE INTERNAL "")
else()
  add_library(auris_vpac STATIC src/auris_vpac_none.c)
  set(AURIS_VPAC_AVAILABLE OFF CACHE INTERNAL "")
endif()
target_include_directories(auris_vpac PUBLIC inc)
target_link_libraries(auris_vpac PUBLIC auris_image)
auris_set_warnings(auris_vpac)
auris_enable_sanitizers(auris_vpac)
