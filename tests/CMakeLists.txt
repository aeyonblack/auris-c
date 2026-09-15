add_executable(test_status test_status.c)
add_executable(test_array test_array.c)
add_executable(test_audio test_audio.c)

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

add_test(NAME status COMMAND test_status)
add_test(NAME array COMMAND test_array)
add_test(NAME audio_contract COMMAND test_audio)
