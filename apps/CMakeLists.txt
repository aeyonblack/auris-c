add_executable(auris_lesson_00 lesson00.c)
add_executable(auris_array_info array_info.c)

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

auris_set_warnings(auris_lesson_00)
auris_enable_sanitizers(auris_lesson_00)

auris_set_warnings(auris_array_info)
auris_enable_sanitizers(auris_array_info)
