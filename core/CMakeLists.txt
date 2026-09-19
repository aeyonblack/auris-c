add_library(
  auris_core
  STATIC
    src/auris_array.c
    src/auris_status.c
    src/auris_framer.c
    src/auris_preprocess.c
)

target_include_directories(
  auris_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(
  auris_core
  PUBLIC
    m
)

auris_set_warnings(auris_core)
auris_enable_sanitizers(auris_core)
