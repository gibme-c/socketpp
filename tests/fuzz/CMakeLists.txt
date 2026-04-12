# libFuzzer harnesses for socketpp.
#
# Opt-in via -DSOCKETPP_BUILD_FUZZERS=ON at the top level.  Clang-only,
# Linux only (Windows Clang has libFuzzer + ASan CRT mismatch).

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SOCKETPP_BUILD_FUZZERS requires Clang (libFuzzer)")
endif()

if(WIN32)
    message(FATAL_ERROR "SOCKETPP_BUILD_FUZZERS is not supported on Windows")
endif()

set(_fuzz_cflags
    -fsanitize=fuzzer-no-link,address
    -fno-omit-frame-pointer
    -g
    -O1
)
set(_fuzz_lflags "-fsanitize=fuzzer,address")

set(SOCKETPP_FUZZ_TARGETS
    fuzz_inet4_parse
    fuzz_inet6_parse
    fuzz_inet4_roundtrip
    fuzz_inet6_roundtrip
    fuzz_sock_address_roundtrip
)

foreach(t ${SOCKETPP_FUZZ_TARGETS})
    add_executable(socketpp-${t} ${t}.cpp)
    set_target_properties(socketpp-${t} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_link_libraries(socketpp-${t} PRIVATE socketpp)
    target_compile_options(socketpp-${t} PRIVATE ${_fuzz_cflags})
    set_property(TARGET socketpp-${t} APPEND_STRING PROPERTY LINK_FLAGS " ${_fuzz_lflags}")
    socketpp_target_harden(socketpp-${t})
endforeach()

message(STATUS "SOCKETPP: libFuzzer harnesses: ${SOCKETPP_FUZZ_TARGETS}")
