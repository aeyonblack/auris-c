find_package(ALSA REQUIRED)

add_library(
  auris_audio
  STATIC
    src/auris_replay.c
    src/auris_alsa.c
)

target_include_directories(
  auris_audio
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${ALSA_INCLUDE_DIRS})

target_link_libraries(
  auris_audio
  PUBLIC
    auris_core
    ${ALSA_LIBRARIES}
)

target_compile_definitions(
  auris_audio
  PUBLIC
    _POSIX_C_SOURCE=200809L
)

auris_set_warnings(auris_audio)
auris_enable_sanitizers(auris_audio)
