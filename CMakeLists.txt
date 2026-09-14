cmake_minimum_required(VERSION 3.16)

project(
  auris_c
  VERSION 0.1.0
  DESCRIPTION "Embedded acoustic imaging system"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(AURIS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(AURIS_ENABLE_SANITIZERS "Enable debug sanitizers" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(CTest)

add_subdirectory(core)
add_subdirectory(configs)
add_subdirectory(apps)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
