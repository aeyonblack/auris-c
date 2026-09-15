add_executable(auris_lesson_00 lesson00.c)
add_executable(auris_array_info array_info.c)
add_executable(capture_scope capture_scope.c)

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

auris_set_warnings(auris_lesson_00)
auris_enable_sanitizers(auris_lesson_00)

auris_set_warnings(auris_array_info)
auris_enable_sanitizers(auris_array_info)

auris_set_warnings(capture_scope)
auris_enable_sanitizers(capture_scope)
