add_executable(auris_lesson_00 lesson00.c)
add_executable(auris_array_info array_info.c)
add_executable(capture_scope capture_scope.c)
add_executable(analysis_demo analysis_demo.c)
add_executable(gcc_scope gcc_scope.c)
add_executable(srp_scope srp_scope.c)
add_executable(heatmap_scope heatmap_scope.c)
add_executable(camera_scope camera_scope.c)

target_link_libraries(
  camera_scope
  PRIVATE
    auris_camera
    auris_image
    auris_output
)

target_link_libraries(
  heatmap_scope
  PRIVATE
    auris_audio
    auris_config
)

target_link_libraries(
  srp_scope
  PRIVATE
    auris_audio
    auris_config
)

target_link_libraries(
  gcc_scope
  PRIVATE
   auris_audio
   auris_config
)

target_link_libraries(
  auris_lesson_00
  PRIVATE
    auris_core
)

target_link_libraries(
    auris_array_info
    PRIVATE
        auris_core
        auris_config
)

target_link_libraries(
  capture_scope
  PRIVATE
    auris_audio
    auris_config
)

target_link_libraries(
  analysis_demo
  PRIVATE
    auris_audio
)

target_compile_definitions(gcc_scope PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(srp_scope PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(heatmap_scope PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(camera_scope PRIVATE _POSIX_C_SOURCE=200809L)

auris_set_warnings(auris_lesson_00)
auris_enable_sanitizers(auris_lesson_00)

auris_set_warnings(auris_array_info)
auris_enable_sanitizers(auris_array_info)

auris_set_warnings(capture_scope)
auris_enable_sanitizers(capture_scope)

auris_set_warnings(analysis_demo)
auris_enable_sanitizers(analysis_demo)

auris_set_warnings(gcc_scope)
auris_enable_sanitizers(gcc_scope)

auris_set_warnings(srp_scope)
auris_enable_sanitizers(srp_scope)

auris_set_warnings(heatmap_scope)
auris_enable_sanitizers(heatmap_scope)

auris_set_warnings(camera_scope)
auris_enable_sanitizers(camera_scope)

find_package(Threads REQUIRED)
add_library(auris_calibration_session STATIC imager_calibration.c)
target_include_directories(auris_calibration_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(auris_calibration_session PUBLIC auris_config auris_overlay)
auris_set_warnings(auris_calibration_session)
auris_enable_sanitizers(auris_calibration_session)

add_executable(imager_scope imager_scope.c)
target_link_libraries(imager_scope PRIVATE auris_audio auris_config
  auris_camera auris_calibration_session auris_output Threads::Threads)
target_compile_definitions(imager_scope PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(imager_scope)
auris_enable_sanitizers(imager_scope)
