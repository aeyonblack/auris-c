add_executable(test_status test_status.c)
add_executable(test_array test_array.c)
add_executable(test_audio test_audio.c)
add_executable(test_analysis test_analysis.c)
add_executable(test_spectrum test_spectrum.c)
add_executable(test_srp test_srp.c)
add_executable(test_view test_view.c)
add_executable(test_activity test_activity.c)
add_executable(test_smoothing test_smoothing.c)

target_link_libraries(
  test_activity
  PRIVATE
    auris_core
)

target_link_libraries(
  test_smoothing
  PRIVATE
    auris_core
    auris_config
)

target_link_libraries(
  test_view
  PRIVATE
    auris_core
    auris_config
)

target_link_libraries(
  test_srp
  PRIVATE
    auris_core
    auris_config
)

target_link_libraries(
  test_spectrum
  PRIVATE
    auris_core
)

target_link_libraries(
  test_analysis
  PRIVATE
    auris_audio
)

target_link_libraries(
  test_audio
  PRIVATE
    auris_audio
)

target_link_libraries(
  test_status
  PRIVATE
    auris_core
)

target_link_libraries(
    test_array
    PRIVATE
        auris_core
        auris_config
)

auris_set_warnings(test_status)
auris_enable_sanitizers(test_status)

auris_set_warnings(test_array)
auris_enable_sanitizers(test_array)

auris_set_warnings(test_audio)
auris_enable_sanitizers(test_audio)

auris_set_warnings(test_analysis)
auris_enable_sanitizers(test_analysis)

auris_set_warnings(test_spectrum)
auris_enable_sanitizers(test_spectrum)

auris_set_warnings(test_srp)
auris_enable_sanitizers(test_srp)

auris_set_warnings(test_view)
auris_enable_sanitizers(test_view)

auris_set_warnings(test_activity)
auris_enable_sanitizers(test_activity)

auris_set_warnings(test_smoothing)
auris_enable_sanitizers(test_smoothing)

add_test(NAME status COMMAND test_status)
add_test(NAME array COMMAND test_array)
add_test(NAME audio_contract COMMAND test_audio)
add_test(NAME analysis COMMAND test_analysis)
add_test(NAME spectrum COMMAND test_spectrum)
add_test(NAME srp COMMAND test_srp)
add_test(NAME view COMMAND test_view)
add_test(NAME activity COMMAND test_activity)
add_test(NAME smoothing COMMAND test_smoothing)

add_executable(test_grid_search test_grid_search.c)
target_link_libraries(test_grid_search PRIVATE auris_core auris_config)
auris_set_warnings(test_grid_search)
auris_enable_sanitizers(test_grid_search)
add_test(NAME grid_search COMMAND test_grid_search)
set_tests_properties(grid_search PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_camera test_camera.c)
if(TARGET auris_camera)
  target_link_libraries(test_camera PRIVATE auris_camera)
  target_compile_definitions(test_camera PRIVATE AURIS_CAMERA_AVAILABLE=1)
endif()
target_compile_definitions(test_camera PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(test_camera)
auris_enable_sanitizers(test_camera)
add_test(NAME camera COMMAND test_camera)
set_tests_properties(camera PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_image test_image.c)
if(TARGET auris_image)
  target_link_libraries(test_image PRIVATE auris_image)
  target_compile_definitions(test_image PRIVATE AURIS_IMAGE_AVAILABLE=1)
endif()
auris_set_warnings(test_image)
auris_enable_sanitizers(test_image)
add_test(NAME image COMMAND test_image)
set_tests_properties(image PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_projection test_projection.c)
if(EXISTS "${CMAKE_SOURCE_DIR}/core/inc/auris_projection.h" AND
   EXISTS "${CMAKE_SOURCE_DIR}/core/src/auris_projection.c")
  target_link_libraries(test_projection PRIVATE auris_core)
  target_compile_definitions(test_projection
    PRIVATE AURIS_PROJECTION_AVAILABLE=1)
endif()
auris_set_warnings(test_projection)
auris_enable_sanitizers(test_projection)
add_test(NAME projection COMMAND test_projection)
set_tests_properties(projection PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_overlay test_overlay.c)
if(TARGET auris_overlay)
  target_link_libraries(test_overlay PRIVATE auris_overlay)
  target_compile_definitions(test_overlay PRIVATE AURIS_OVERLAY_AVAILABLE=1)
endif()
auris_set_warnings(test_overlay)
auris_enable_sanitizers(test_overlay)
add_test(NAME overlay COMMAND test_overlay)
set_tests_properties(overlay PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_calibration test_calibration.c)
target_link_libraries(test_calibration PRIVATE auris_calibration_session)
target_compile_definitions(test_calibration PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(test_calibration)
auris_enable_sanitizers(test_calibration)
add_test(NAME calibration COMMAND test_calibration)

add_executable(test_timeline test_timeline.c)
target_link_libraries(test_timeline PRIVATE auris_core)
auris_set_warnings(test_timeline)
auris_enable_sanitizers(test_timeline)
add_test(NAME timeline COMMAND test_timeline)

add_executable(test_output test_output.c)
target_link_libraries(test_output PRIVATE auris_stream)
target_compile_definitions(test_output PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(test_output)
auris_enable_sanitizers(test_output)
add_test(NAME output COMMAND test_output)

add_executable(test_display test_display.c)
target_link_libraries(test_display PRIVATE auris_display auris_outputs)
auris_set_warnings(test_display)
auris_enable_sanitizers(test_display)
add_test(NAME display COMMAND test_display)

add_executable(test_output_worker test_output_worker.c)
target_link_libraries(test_output_worker PRIVATE auris_output)
target_compile_definitions(test_output_worker PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(test_output_worker)
auris_enable_sanitizers(test_output_worker)
add_test(NAME output_worker COMMAND test_output_worker)

add_executable(test_controls test_controls.c)
target_link_libraries(test_controls PRIVATE auris_control auris_display)
target_compile_definitions(test_controls PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(test_controls)
auris_enable_sanitizers(test_controls)
add_test(NAME controls COMMAND test_controls)

add_executable(test_cmb_array test_cmb_array.c)
target_link_libraries(test_cmb_array PRIVATE auris_core auris_config)
auris_set_warnings(test_cmb_array)
auris_enable_sanitizers(test_cmb_array)
add_test(NAME cmb_array COMMAND test_cmb_array)

# Module 17: fails until drivers/adc (auris_adc) exists.
if(TARGET auris_adc)
  add_executable(test_cmb_adc test_cmb_adc.c)
  target_link_libraries(test_cmb_adc PRIVATE auris_adc)
  target_compile_definitions(test_cmb_adc PRIVATE _POSIX_C_SOURCE=200809L)
  auris_set_warnings(test_cmb_adc)
  auris_enable_sanitizers(test_cmb_adc)
  add_test(NAME cmb_adc COMMAND test_cmb_adc)
else()
  add_test(NAME cmb_adc COMMAND ${CMAKE_COMMAND} -E false)
endif()

# Module 18: capture formats, profiles and the profile-based ALSA driver.
# Fails until core/src/auris_audio.c and configs/src/auris_capture_profiles.c exist.
if(EXISTS ${PROJECT_SOURCE_DIR}/core/src/auris_audio.c AND
   EXISTS ${PROJECT_SOURCE_DIR}/configs/src/auris_capture_profiles.c)
  add_executable(test_capture_format test_capture_format.c)
  target_link_libraries(test_capture_format PRIVATE auris_audio auris_config)
  auris_set_warnings(test_capture_format)
  auris_enable_sanitizers(test_capture_format)
  add_test(NAME capture_format COMMAND test_capture_format)
else()
  add_test(NAME capture_format COMMAND ${CMAKE_COMMAND} -E false)
endif()

# Module 19: WAV recording and replay of raw device frames.
if(EXISTS ${PROJECT_SOURCE_DIR}/drivers/audio/src/auris_wav.c)
  add_executable(test_wav test_wav.c)
  target_link_libraries(test_wav PRIVATE auris_audio auris_config)
  auris_set_warnings(test_wav)
  auris_enable_sanitizers(test_wav)
  add_test(NAME wav COMMAND test_wav)
else()
  add_test(NAME wav COMMAND ${CMAKE_COMMAND} -E false)
endif()

# Module 20: analysis frames sized at run time.
add_executable(test_framing test_framing.c)
target_link_libraries(test_framing PRIVATE auris_core auris_config)
auris_set_warnings(test_framing)
auris_enable_sanitizers(test_framing)
add_test(NAME framing COMMAND test_framing)

# Module 21: spectrum and GCC-PHAT sized at run time.
add_executable(test_gcc_runtime test_gcc_runtime.c)
target_link_libraries(test_gcc_runtime PRIVATE auris_core auris_config)
auris_set_warnings(test_gcc_runtime)
auris_enable_sanitizers(test_gcc_runtime)
add_test(NAME gcc_runtime COMMAND test_gcc_runtime)

# Module 22: SRP-PHAT at run-time frame, hop and refinement.
add_executable(test_srp_runtime test_srp_runtime.c)
target_link_libraries(test_srp_runtime PRIVATE auris_core auris_config)
auris_set_warnings(test_srp_runtime)
auris_enable_sanitizers(test_srp_runtime)
add_test(NAME srp_runtime COMMAND test_srp_runtime)

# Module 23: the front-facing heatmap with the CMB.
add_executable(test_heatmap_cmb test_heatmap_cmb.c)
target_link_libraries(test_heatmap_cmb PRIVATE auris_core auris_config)
auris_set_warnings(test_heatmap_cmb)
auris_enable_sanitizers(test_heatmap_cmb)
add_test(NAME heatmap_cmb COMMAND test_heatmap_cmb)

# Module 24: the activity gate at run-time sizes and per-profile bands.
add_executable(test_activity_runtime test_activity_runtime.c)
target_link_libraries(test_activity_runtime PRIVATE auris_core auris_config)
auris_set_warnings(test_activity_runtime)
auris_enable_sanitizers(test_activity_runtime)
add_test(NAME activity_runtime COMMAND test_activity_runtime)

# Module 25: the imager's audio side on any array.
add_executable(test_imager_audio test_imager_audio.c)
target_link_libraries(test_imager_audio PRIVATE auris_imager_audio)
auris_set_warnings(test_imager_audio)
auris_enable_sanitizers(test_imager_audio)
add_test(NAME imager_audio COMMAND test_imager_audio)

# Module 27: SNR-weighted PHAT, the weight-based gate and SRP coherence.
add_executable(test_weighting test_weighting.c)
target_link_libraries(test_weighting PRIVATE auris_imager_audio)
auris_set_warnings(test_weighting)
auris_enable_sanitizers(test_weighting)
add_test(NAME weighting COMMAND test_weighting)

# Module 28: the Bayesian direction tracker and the overlay's strength and
# sub-cell marker.
add_executable(test_track test_track.c)
target_link_libraries(test_track PRIVATE auris_core)
auris_set_warnings(test_track)
auris_enable_sanitizers(test_track)
add_test(NAME track COMMAND test_track)

add_executable(test_overlay_draw test_overlay_draw.c)
target_link_libraries(test_overlay_draw PRIVATE auris_overlay)
auris_set_warnings(test_overlay_draw)
auris_enable_sanitizers(test_overlay_draw)
add_test(NAME overlay_draw COMMAND test_overlay_draw)
