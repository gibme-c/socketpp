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

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(STATUS "Found ccache package... Activating...")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

option(SOCKETPP_BUILD_EXAMPLES "Build examples" OFF)
if(DEFINED ENV{SOCKETPP_BUILD_EXAMPLES})
    set(SOCKETPP_BUILD_EXAMPLES $ENV{SOCKETPP_BUILD_EXAMPLES})
endif()

option(SOCKETPP_BUILD_TESTS "Build tests" OFF)
if(DEFINED ENV{SOCKETPP_BUILD_TESTS})
    set(SOCKETPP_BUILD_TESTS $ENV{SOCKETPP_BUILD_TESTS})
endif()


## This section describes our general CMake setup options
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_SKIP_INSTALL_RULES OFF FORCE)
    set(CMAKE_SKIP_PACKAGE_ALL_DEPENDENCY ON FORCE)
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
            set(CMAKE_REQUIRED_FLAGS "-fstack-protector-strong")
            check_cxx_source_compiles(
                "int main() { char buf[64]; buf[0] = 0; return buf[0]; }"
                _SOCKETPP_HAS_STACK_PROTECTOR)
            unset(CMAKE_REQUIRED_FLAGS)
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
        # PIE for full ASLR on ELF platforms
        if(NOT WIN32)
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
    src/high/tcp_connection.cpp
    src/high/tcp_server.cpp
    src/high/tcp_client.cpp
    src/high/udp_server.cpp
)

add_library(socketpp::socketpp ALIAS socketpp)

# Platform-specific backend sources
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(socketpp PRIVATE src/event/epoll.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(socketpp PRIVATE src/event/kqueue.cpp)
elseif(WIN32)
    target_sources(socketpp PRIVATE src/event/iocp.cpp)
endif()

set_target_properties(socketpp PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(socketpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Consumers treat socketpp headers as system headers (suppresses their -Werror warnings)
target_include_directories(socketpp SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(socketpp PUBLIC cxx_std_17)

# Platform-specific link dependencies (PRIVATE — consumers don't link directly)
if(WIN32)
    target_link_libraries(socketpp PRIVATE ws2_32 mswsock)
endif()

socketpp_target_harden(socketpp)

# Install
include(GNUInstallDirs)
install(TARGETS socketpp EXPORT socketppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Install public headers only (exclude detail/ and internal headers)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
    PATTERN "detail" EXCLUDE
)

install(EXPORT socketppTargets
    FILE socketppTargets.cmake
    NAMESPACE socketpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/socketpp
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/socketppConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/socketppConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/socketpp
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/socketppConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/socketppConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/socketppConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/socketpp
)

# Examples and tests
if(SOCKETPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SOCKETPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
