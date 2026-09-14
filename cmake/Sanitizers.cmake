function(auris_enable_sanitizers target)
    if(AURIS_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            ${target}
            PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )

        target_link_options(
            ${target}
            PRIVATE
                -fsanitize=address,undefined
        )
    endif()
endfunction()
