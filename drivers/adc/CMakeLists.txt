add_library(auris_adc STATIC
  src/auris_i2c.c
  src/auris_pcm186x.c
)
target_include_directories(auris_adc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(auris_adc PUBLIC m)
target_compile_definitions(auris_adc PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(auris_adc)
auris_enable_sanitizers(auris_adc)
