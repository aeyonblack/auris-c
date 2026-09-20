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
target_link_libraries(test_output PRIVATE auris_output)
target_compile_definitions(test_output PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(test_output)
auris_enable_sanitizers(test_output)
add_test(NAME output COMMAND test_output)
