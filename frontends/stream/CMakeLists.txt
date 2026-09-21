find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)

add_library(auris_stream STATIC src/auris_stream.c)
target_include_directories(auris_stream PUBLIC inc)
target_link_libraries(auris_stream PUBLIC auris_output PRIVATE JPEG::JPEG)
target_compile_definitions(auris_stream PRIVATE _POSIX_C_SOURCE=200809L)
auris_set_warnings(auris_stream)
auris_enable_sanitizers(auris_stream)
