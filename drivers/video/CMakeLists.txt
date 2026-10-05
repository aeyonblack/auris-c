add_library(auris_camera STATIC src/auris_camera.c)
target_include_directories(auris_camera PUBLIC inc)
target_compile_definitions(auris_camera PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(auris_camera)
auris_enable_sanitizers(auris_camera)

target_link_libraries(auris_camera PUBLIC m)
