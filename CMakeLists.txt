cmake_minimum_required(VERSION 3.16)
project(apollo_patcher C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# libapollo checkout.
#
# Defaults to a sibling clone of apollo-lib, which is the layout CI uses and
# the same pattern the console apps follow (they check apollo-lib out next to
# themselves). Override for a checkout somewhere else:
#
#   cmake -S . -B build -DAPOLLO_ROOT=/path/to/apollo-lib
# ---------------------------------------------------------------------------
set(APOLLO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../apollo-lib"
    CACHE PATH "Path to the apollo-lib checkout")
get_filename_component(APOLLO_ROOT "${APOLLO_ROOT}" ABSOLUTE)

if(NOT EXISTS "${APOLLO_ROOT}/include/apollo.h")
    message(FATAL_ERROR
        "apollo-lib not found at ${APOLLO_ROOT}\n"
        "Clone it next to this repo:\n"
        "  git clone https://github.com/bucanero/apollo-lib ${APOLLO_ROOT}\n"
        "or point at an existing checkout with -DAPOLLO_ROOT=/path/to/apollo-lib")
endif()

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
