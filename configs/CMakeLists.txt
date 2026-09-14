add_library(
  auris_config
  STATIC
    src/auris_respeaker_array.c
)

target_include_directories(
  auris_config
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(
  auris_config
  PUBLIC
    auris_core
)

auris_set_warnings(auris_config)
auris_enable_sanitizers(auris_config)
