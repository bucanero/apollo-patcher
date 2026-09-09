cmake_minimum_required(VERSION 3.16)
project(apollo_patcher C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# libapollo checkout.
#
# Two layouts are supported, because CI and local development differ:
#
#   ./apollo-lib    in-tree. What CI produces: actions/checkout's `path:` is
#                   relative to $GITHUB_WORKSPACE, which IS this repo's root.
#                   Also where a submodule would sit.
#   ../apollo-lib   a sibling clone, the usual local-dev layout and the same
#                   pattern the console apps follow.
#
# Or name it explicitly:
#
#   cmake -S . -B build -DAPOLLO_ROOT=/path/to/apollo-lib
# ---------------------------------------------------------------------------
set(APOLLO_ROOT "" CACHE PATH "Path to the apollo-lib checkout")

if(APOLLO_ROOT)
    set(_apollo_candidates "${APOLLO_ROOT}")
else()
    set(_apollo_candidates
        "${CMAKE_CURRENT_SOURCE_DIR}/apollo-lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/../apollo-lib")
endif()

set(_apollo_found "")
foreach(_dir IN LISTS _apollo_candidates)
    if(EXISTS "${_dir}/include/apollo.h")
        set(_apollo_found "${_dir}")
        break()
    endif()
endforeach()

if(NOT _apollo_found)
    string(REPLACE ";" "\n  " _apollo_tried "${_apollo_candidates}")
    message(FATAL_ERROR
        "apollo-lib not found. Looked for include/apollo.h under:\n"
        "  ${_apollo_tried}\n"
        "Clone it next to this repo:\n"
        "  git clone https://github.com/bucanero/apollo-lib ${CMAKE_CURRENT_SOURCE_DIR}/../apollo-lib\n"
        "or point at an existing checkout with -DAPOLLO_ROOT=/path/to/apollo-lib")
endif()

get_filename_component(APOLLO_ROOT "${_apollo_found}" ABSOLUTE)
message(STATUS "Using apollo-lib at ${APOLLO_ROOT}")

set(MBEDTLS_DIR "${APOLLO_ROOT}/mbedtls-2.16.12")

# ---------------------------------------------------------------------------
# apollo_engine: libapollo compiled from source, plus the apollo_ctrl facade.
#
# core/apollo_ctrl.c is the stdio-free controller shared by every front-end in
# this repo (the desktop GUI and the web build). It is the only place that
# adapts libapollo's data model for a UI; the engine sources stay untouched.
# ---------------------------------------------------------------------------
set(ENGINE_SRCS
    ${APOLLO_ROOT}/source/crc_util.c
    ${APOLLO_ROOT}/source/list.c
    ${APOLLO_ROOT}/source/offzip.c
    ${APOLLO_ROOT}/source/packzip.c
    ${APOLLO_ROOT}/source/patches.c
    ${APOLLO_ROOT}/source/decrypt.c
    ${APOLLO_ROOT}/source/loader.c
    ${APOLLO_ROOT}/source/wildcard.c
    ${APOLLO_ROOT}/source/upy.c)

add_library(apollo_engine STATIC ${ENGINE_SRCS} core/apollo_ctrl.c)
target_include_directories(apollo_engine PUBLIC
    ${APOLLO_ROOT}/include
    ${APOLLO_ROOT}/tools          # provides dbglogger.h prototype
    ${MBEDTLS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_compile_definitions(apollo_engine PRIVATE _GNU_SOURCE _USE_MBEDTLS)

# mbedcrypto: prefer the prebuilt static lib (built by CI), else expect a
# system/library target. Adjust to taste for your packaging.
find_library(MBEDCRYPTO_LIB
    NAMES mbedcrypto libmbedcrypto
    PATHS ${MBEDTLS_DIR}/build/library
    NO_DEFAULT_PATH)
if(NOT MBEDCRYPTO_LIB)
    find_library(MBEDCRYPTO_LIB NAMES mbedcrypto)
endif()
if(NOT MBEDCRYPTO_LIB)
    message(FATAL_ERROR "libmbedcrypto not found. Build it in the apollo-lib "
                        "checkout first (see CI: cd mbedtls-2.16.12/library && make static).")
endif()

# Link zlib directly (-lz) like the CLI Makefile does, rather than via
# find_package(ZLIB) — the latter fails under MSYS2/MinGW CI even though the
# compiler links -lz fine (same as the working CLI build). zlib.h is on the
# compiler's default include path everywhere the CLI builds.
target_link_libraries(apollo_engine PUBLIC ${MBEDCRYPTO_LIB} z m)

# ---------------------------------------------------------------------------
# Headless parity test (no GUI deps) — always buildable.
# ---------------------------------------------------------------------------
add_executable(apollo_ctrl_test core/test_ctrl.c)
target_link_libraries(apollo_ctrl_test PRIVATE apollo_engine)

# ---------------------------------------------------------------------------
# Front-ends.
#
# The web front-end is not built here: it is a wasm module with its own link
# flags, driven by web/Makefile (Emscripten). See web/README.md.
# ---------------------------------------------------------------------------
option(APOLLO_BUILD_GUI "Build the Dear ImGui GUI (needs OpenGL/GLFW)" ON)
if(APOLLO_BUILD_GUI)
    add_subdirectory(gui)
endif()
