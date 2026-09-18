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
#
# core/patchdb.c reads the bundled patch database (apollo-patches.zip). The web
# build does not use it — the page fetches patches over the network instead —
# but it costs nothing to carry in the shared library.
#
# core/psp/ and core/ps3/ are the consoles' own savedata encryption — the layer
# BELOW anything a .savepatch does, and the reason a save taken straight off a
# Memory Stick or a PS3 hard drive comes out of the patch engine as garbage.
# Derived from apollo-psp and pfdtool respectively; see the note at the top of
# core/psp/psp_savedata.c for why they live here rather than in apollo-lib, and
# what would move them there. They need only mbedTLS's AES and SHA1, both of
# which the engine already links.
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

set(PSP_SRCS
    core/psp/kirk_engine.c
    core/psp/psp_savedata.c)

set(PS3_SRCS
    core/ps3/pfd_savedata.c)

add_library(apollo_engine STATIC ${ENGINE_SRCS} ${PSP_SRCS} ${PS3_SRCS}
                                 core/apollo_ctrl.c core/patchdb.c)
target_include_directories(apollo_engine PUBLIC
    ${APOLLO_ROOT}/include
    ${APOLLO_ROOT}/tools          # provides dbglogger.h prototype
    ${MBEDTLS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/psp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ps3)
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
# apollo-patches.zip — the patch database the desktop app browses.
#
# Built here rather than only in CI, because the failure when it is missing is
# silent: the app starts, "Find a game..." lists nothing, and PSP saves ask for
# a game key that is sitting in the database nobody built. The zip is also what
# `apollo_ctrl_test --db` reads, and it lands beside that binary, so the test
# needs no $APOLLO_PATCHES_ZIP either.
#
# Optional on purpose. The patch database is a separate checkout that is NOT
# needed to compile anything, so a missing one (or a missing Python) says so
# and carries on — it must never be the reason a build fails.
#
# Found the same two ways apollo-lib is, and overridable the same way:
#   -DAPOLLO_PATCHES=/path/to/apollo-patches
# ---------------------------------------------------------------------------
set(APOLLO_PATCHES "" CACHE PATH "Path to the apollo-patches checkout")

if(APOLLO_PATCHES)
    set(_patches_candidates "${APOLLO_PATCHES}")
else()
    set(_patches_candidates
        "${CMAKE_CURRENT_SOURCE_DIR}/apollo-patches"
        "${CMAKE_CURRENT_SOURCE_DIR}/../apollo-patches")
endif()

set(_patches_found "")
foreach(_dir IN LISTS _patches_candidates)
    if(EXISTS "${_dir}/PSP/gamekeys.txt")
        get_filename_component(_patches_found "${_dir}" ABSOLUTE)
        break()
    endif()
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)

set(APOLLO_BUNDLE "")
if(_patches_found AND Python3_Interpreter_FOUND)
    set(APOLLO_BUNDLE "${CMAKE_BINARY_DIR}/apollo-patches.zip")

    # CONFIGURE_DEPENDS so a `git pull` in the patch database is noticed: the
    # glob is re-run at build time and a changed list triggers a reconfigure.
    # It costs one directory sweep per build over ~2250 files.
    #
    # The platform list mirrors PLATFORMS in tools/make-bundle.py; a console
    # added there and not here means its patches stop triggering a rebuild,
    # not that they are left out of the zip.
    file(GLOB APOLLO_BUNDLE_INPUTS CONFIGURE_DEPENDS
        "${_patches_found}/PS2/*.savepatch"
        "${_patches_found}/PS3/*.savepatch"
        "${_patches_found}/PS4/*.savepatch"
        "${_patches_found}/PSP/*.savepatch"
        "${_patches_found}/PSV/*.savepatch"
        "${_patches_found}/python/*.py")

    add_custom_command(
        OUTPUT  "${APOLLO_BUNDLE}"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-bundle.py"
                "${_patches_found}" "${APOLLO_BUNDLE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-bundle.py"
                "${_patches_found}/PSP/gamekeys.txt"
                ${APOLLO_BUNDLE_INPUTS}
        COMMENT "Building the patch database (apollo-patches.zip)"
        VERBATIM)

    add_custom_target(apollo_patches_bundle ALL DEPENDS "${APOLLO_BUNDLE}")
    message(STATUS "Patch database from ${_patches_found}")
elseif(NOT _patches_found)
    string(REPLACE ";" "\n    " _patches_tried "${_patches_candidates}")
    message(STATUS
        "apollo-patches not found, so apollo-patches.zip will not be built.\n"
        "  Looked for PSP/gamekeys.txt under:\n    ${_patches_tried}\n"
        "  The app runs without it, but the game browser is empty and PSP saves\n"
        "  cannot look up their game key. Clone it beside this repo, or pass\n"
        "  -DAPOLLO_PATCHES=/path/to/apollo-patches.")
else()
    message(STATUS
        "No Python interpreter, so apollo-patches.zip will not be built "
        "(tools/make-bundle.py needs one). The app runs without it; the game "
        "browser will be empty.")
endif()

# ---------------------------------------------------------------------------
# Headless parity test (no GUI deps) — always buildable.
# ---------------------------------------------------------------------------
add_executable(apollo_ctrl_test core/test_ctrl.c)
target_link_libraries(apollo_ctrl_test PRIVATE apollo_engine)

# PSP savedata: bounds checks on the PARAM.SFO parser, and known-answer vectors
# taken from the unmodified apollo-psp implementation, so a pass says the
# vendored copy is byte-faithful to what ships on the console rather than
# merely self-consistent. Needs no sample save; `--save DIR FILE KEY` runs a
# real one when you have it.
add_executable(apollo_psp_test core/test_psp.c)
target_link_libraries(apollo_psp_test PRIVATE apollo_engine)

# PS3 savedata: bounds checks on the PARAM.PFD parser, known-answer vectors for
# the file cipher, and the games.conf lookup rules. `--corpus DIR` walks a tree
# of real saves and checks every hash the console wrote against one recomputed
# here, which is what says the implementation is right rather than merely
# self-consistent.
add_executable(apollo_ps3_test core/test_ps3.c)
target_link_libraries(apollo_ps3_test PRIVATE apollo_engine)

# ---------------------------------------------------------------------------
# Front-ends.
#
# The web front-end is not built here: it is a wasm module with its own link
# flags, driven by web/Makefile (Emscripten). See web/README.md.
# ---------------------------------------------------------------------------
option(APOLLO_BUILD_GUI "Build the Dear ImGui GUI (needs OpenGL/GLFW)" ON)
if(APOLLO_BUILD_GUI)
    add_subdirectory(gui)
else()
    # Said out loud because the option is STICKY: a build directory once
    # configured with -DAPOLLO_BUILD_GUI=OFF keeps skipping the app, and a
    # later `cmake --build build` reports success for the tests while the app
    # binary sits there untouched from whenever it was last built. The failure
    # looks like "my change did not take effect", which is the hardest kind to
    # trace. Re-enable with -DAPOLLO_BUILD_GUI=ON.
    message(STATUS "APOLLO_BUILD_GUI=OFF: the desktop app will NOT be built "
                   "or updated by this build directory")
endif()
