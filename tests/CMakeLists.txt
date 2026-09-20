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
