function(auris_set_warnings target)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
    )
    if(AURIS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
