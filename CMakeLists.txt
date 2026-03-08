cmake_minimum_required(VERSION 3.10)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build: Debug, Release, RelWithDebInfo" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo)
endif()

message(STATUS "Mode: ${CMAKE_BUILD_TYPE}")

project(socketpp
    VERSION 0.1.0
    DESCRIPTION "Cross-platform non-blocking socket library"
    LANGUAGES CXX
)

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(STATUS "Found ccache package... Activating...")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

option(SOCKETPP_BUILD_EXAMPLES "Build examples" OFF)
if(DEFINED ENV{SOCKETPP_BUILD_EXAMPLES})
    set(SOCKETPP_BUILD_EXAMPLES "$ENV{SOCKETPP_BUILD_EXAMPLES}" CACHE BOOL "Build examples" FORCE)
endif()

option(SOCKETPP_BUILD_TESTS "Build tests" OFF)
if(DEFINED ENV{SOCKETPP_BUILD_TESTS})
    set(SOCKETPP_BUILD_TESTS "$ENV{SOCKETPP_BUILD_TESTS}" CACHE BOOL "Build tests" FORCE)
endif()


## This section describes our general CMake setup options
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_SKIP_INSTALL_RULES OFF)
    set(CMAKE_SKIP_PACKAGE_ALL_DEPENDENCY ON)
    set(CMAKE_SUPPRESS_REGENERATION ON)
endif()

# ---------------------------------------------------------------------------
# socketpp_target_harden(<target>)
# Apply security-hardening compile/link flags PRIVATE to a single target.
#
# MinGW ABI note: only use CMake's built-in MINGW (set for GCC with MinGW).
# Clang on Windows can target either MinGW or MSVC ABI, and the linker
# varies (GNU ld vs lld-link) — applying the wrong linker flags breaks the
# build. We let Clang on Windows rely on the OS defaults for DEP/ASLR
# (enabled by default on modern Windows) rather than guessing the linker
# style.
# ---------------------------------------------------------------------------
function(socketpp_target_harden TARGET_NAME)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${TARGET_NAME} PRIVATE
            -Wall -Wextra -Wpedantic -Wuninitialized -Wshadow
            -Wcast-align -Wundef
            -Wformat=2 -Wformat-security
            -fno-strict-overflow
        )

        # Stack protector (probe once, cache result)
        if(NOT DEFINED _SOCKETPP_HAS_STACK_PROTECTOR)
            cmake_push_check_state(RESET)
            set(CMAKE_REQUIRED_FLAGS "-fstack-protector-strong")
            check_cxx_source_compiles(
                "int main() { char buf[64]; buf[0] = 0; return buf[0]; }"
                _SOCKETPP_HAS_STACK_PROTECTOR)
            cmake_pop_check_state()
            set(_SOCKETPP_HAS_STACK_PROTECTOR ${_SOCKETPP_HAS_STACK_PROTECTOR} CACHE INTERNAL "")
        endif()
        if(_SOCKETPP_HAS_STACK_PROTECTOR)
            target_compile_options(${TARGET_NAME} PRIVATE -fstack-protector-strong)
        endif()

        # -fPIC is implied on Windows PE; only needed on ELF/Mach-O
        if(NOT WIN32)
            target_compile_options(${TARGET_NAME} PRIVATE -fPIC)
            # Stack clash protection (Linux only — not supported on macOS/Apple Clang)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_compile_options(${TARGET_NAME} PRIVATE -fstack-clash-protection)
            endif()
        endif()

        # Intel CET control-flow integrity (x86_64 only, probe once)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            if(NOT DEFINED _SOCKETPP_HAS_CF_PROTECTION)
                check_cxx_compiler_flag("-fcf-protection=full" _SOCKETPP_HAS_CF_PROTECTION)
                set(_SOCKETPP_HAS_CF_PROTECTION ${_SOCKETPP_HAS_CF_PROTECTION} CACHE INTERNAL "")
            endif()
            if(_SOCKETPP_HAS_CF_PROTECTION)
                target_compile_options(${TARGET_NAME} PRIVATE -fcf-protection=full)
            endif()
        endif()

        # GCC-only extras
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${TARGET_NAME} PRIVATE
                $<$<CONFIG:Debug>:-D_GLIBCXX_ASSERTIONS>
            )
        endif()

        # _FORTIFY_SOURCE (requires optimization; unreliable on MinGW)
        if(NOT WIN32)
            target_compile_options(${TARGET_NAME} PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
            )
        endif()

        # Linker hardening — Linux
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_property(TARGET ${TARGET_NAME} APPEND_STRING PROPERTY LINK_FLAGS
                " -Wl,-z,relro,-z,now -Wl,-z,noexecstack")
        endif()

        # Linker hardening — MinGW (DEP + ASLR)
        # Only for GCC+MinGW (MINGW is false for Clang targeting MSVC ABI)
        if(MINGW)
            set_property(TARGET ${TARGET_NAME} APPEND_STRING PROPERTY LINK_FLAGS
                " -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va")
        endif()

    elseif(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE
            /W4 /sdl /GS /guard:cf
            /wd4267 /wd4996
        )

        # Spectre mitigations (probe once)
        if(NOT DEFINED _SOCKETPP_HAS_QSPECTRE)
            check_cxx_compiler_flag("/Qspectre" _SOCKETPP_HAS_QSPECTRE)
            set(_SOCKETPP_HAS_QSPECTRE ${_SOCKETPP_HAS_QSPECTRE} CACHE INTERNAL "")
        endif()
        if(_SOCKETPP_HAS_QSPECTRE)
            target_compile_options(${TARGET_NAME} PRIVATE /Qspectre)
        endif()

        # DEP + ASLR + CET
        set_property(TARGET ${TARGET_NAME} APPEND_STRING PROPERTY LINK_FLAGS
            " /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# socketpp_configure_executable(<target>)
# Apply standard C++17 settings, optimization flags, static linking, and
# hardening to an executable target.
# ---------------------------------------------------------------------------
function(socketpp_configure_executable TARGET_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    )
    if(NOT MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE
            $<$<CONFIG:Debug>:-g3>
            $<$<CONFIG:Debug>:-Og>
            $<$<CONFIG:Release>:-O3>
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set_property(TARGET ${TARGET_NAME} APPEND_STRING PROPERTY
                LINK_FLAGS " -static-libgcc -static-libstdc++")
        endif()
        # PIE for full ASLR on ELF platforms (macOS defaults to PIE)
        if(NOT WIN32 AND NOT APPLE)
            target_compile_options(${TARGET_NAME} PRIVATE -fPIE)
            set_property(TARGET ${TARGET_NAME} APPEND_STRING PROPERTY
                LINK_FLAGS " -pie")
        endif()
    endif()
    socketpp_target_harden(${TARGET_NAME})
endfunction()

# ---------------------------------------------------------------------------
# socketpp static library
# ---------------------------------------------------------------------------
add_library(socketpp STATIC
    src/platform/error.cpp
    src/platform/wsa_init.cpp
    src/net/address.cpp
    src/net/inet4.cpp
    src/net/inet6.cpp
    src/socket/socket.cpp
    src/socket/options.cpp
    src/socket/tcp.cpp
    src/socket/tcp_listener.cpp
    src/socket/tcp_connector.cpp
    src/socket/tcp_accept_loop.cpp
    src/socket/tcp_connect_async.cpp
    src/socket/udp.cpp
    src/socket/udp_peer.cpp
    src/socket/udp_recv_async.cpp
    src/event/loop.cpp
    src/high/thread_pool.cpp
    src/high/stream.cpp
    src/high/dgram.cpp
)

# Platform-specific backend sources
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(socketpp PRIVATE src/event/epoll.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(socketpp PRIVATE src/event/kqueue.cpp)
elseif(WIN32)
    target_sources(socketpp PRIVATE src/event/iocp.cpp)
endif()

set_target_properties(socketpp PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(socketpp
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Consumers get our headers as -isystem (suppresses warnings from our headers in their builds)
target_include_directories(socketpp SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(socketpp PUBLIC cxx_std_17)

# Platform-specific link dependencies (PRIVATE — consumers don't link directly)
if(WIN32)
    target_link_libraries(socketpp PRIVATE ws2_32 mswsock)
endif()

socketpp_target_harden(socketpp)

# Examples and tests
if(SOCKETPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SOCKETPP_BUILD_TESTS)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        enable_testing()
    endif()
    add_subdirectory(tests)
endif()
