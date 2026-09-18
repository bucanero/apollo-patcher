# Dear ImGui + GLFW desktop GUI. Fetched at configure time (needs network once).
#
# The engine + apollo_ctrl facade come from the apollo_engine target defined by
# the top-level CMakeLists.txt; this file only builds the app itself.
#
# NOT a standalone CMake project. It is add_subdirectory()'d from the root and
# has no project() or cmake_minimum_required() of its own, so pointing CMake at
# this directory fetches GLFW, configures it, and only then fails with CMP0000's
# "No cmake_minimum_required command is present" — a minute of downloads before
# an error that names the wrong problem. Say so up front instead.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR
        "gui/ is not a standalone CMake project — it is built as part of the "
        "one in the repository root.\n"
        "Configure from there instead:\n"
        "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release\n"
        "  cmake --build build -j\n"
        "The GUI is on by default; -DAPOLLO_BUILD_GUI=OFF leaves it out.")
endif()

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# X11 only: GLFW 3.4 builds the Wayland backend too by default, which needs
# wayland-scanner + Wayland dev packages we don't install (and Wayland has no
# window-icon protocol anyway). Keeps the Linux build's deps to xorg-dev.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# File dialogs: portable-file-dialogs is vendored as a single header at
# src/portable-file-dialogs.h (found via main.cpp's own directory). On
# macOS/Linux it shells out to osascript/zenity (a separate process), which
# sidesteps the in-process NSOpenPanel breakage caused by GLFW's Cocoa init —
# so no custom helper binary is needed. On Windows it uses the Win32 API.

# Renderer backend: the fixed-function opengl2 backend everywhere — it needs
# only OpenGL 1.1, which runs on a real GPU's compat profile and on the
# software GL fallbacks present on every platform (see main.cpp). Must match
# the backend used in main.cpp.
#
# misc/cpp/imgui_stdlib.cpp is upstream's std::string binding for InputText.
# The code editor needs it: a code body can be 100KB (the largest in the
# database) and grows as it is typed into, so a fixed char[] is the wrong
# shape.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC glfw)

find_package(OpenGL REQUIRED)

add_executable(apollo_patcher_gui src/main.cpp)
target_link_libraries(apollo_patcher_gui PRIVATE apollo_engine imgui OpenGL::GL)

if(MINGW)
    # -static: statically link the MinGW runtime (libgcc / libstdc++ /
    #   winpthread) so the .exe runs on stock Windows without shipping DLLs.
    #   Windows system DLLs (opengl32, gdi32, comdlg32, …) are still linked
    #   via their import libs, which is correct.
    # -mwindows: build as a GUI-subsystem app so no console window opens
    #   behind the GUI. MinGW's WinMain shim still calls our main().
    target_link_options(apollo_patcher_gui PRIVATE -static -mwindows)
endif()

# The patch database, put where patchdb.c looks for it (see locate()): inside
# Contents/Resources for a macOS .app, and beside the executable everywhere
# else. Copied after the link rather than declared as a bundle resource,
# because the zip is generated in the TOP-LEVEL binary directory and
# cross-directory GENERATED sources need CMP0118 (CMake 3.20), above this
# project's floor.
#
# APOLLO_BUNDLE is empty when there is no apollo-patches checkout to build it
# from; the app is expected to cope, and says so in its own UI.
if(APOLLO_BUNDLE)
    if(APPLE)
        set(_bundle_dest "$<TARGET_BUNDLE_CONTENT_DIR:apollo_patcher_gui>/Resources")
    else()
        set(_bundle_dest "$<TARGET_FILE_DIR:apollo_patcher_gui>")
    endif()

    # Driven by a stamp rather than POST_BUILD on the app, which runs only when
    # the executable is relinked: pulling new patches rebuilds the zip without
    # touching the binary, and the app would keep the old copy — stale in
    # exactly the case the rebuild was for. This re-copies whenever either side
    # moves.
    set(_bundle_stamp "${CMAKE_CURRENT_BINARY_DIR}/bundle-installed.stamp")
    add_custom_command(
        OUTPUT  "${_bundle_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle_dest}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APOLLO_BUNDLE}" "${_bundle_dest}/apollo-patches.zip"
        COMMAND ${CMAKE_COMMAND} -E touch "${_bundle_stamp}"
        DEPENDS "${APOLLO_BUNDLE}" apollo_patcher_gui
        COMMENT "Installing the patch database into the app"
        VERBATIM)
    add_custom_target(apollo_patcher_gui_bundle ALL DEPENDS "${_bundle_stamp}")
    # The zip is produced by a custom command in the TOP-LEVEL directory, and a
    # Makefiles generator does not order across directories from a file-level
    # DEPENDS alone: without this, a run that rebuilt the zip could finish
    # before the copy noticed, leaving the app one build behind.
    add_dependencies(apollo_patcher_gui_bundle apollo_patches_bundle)
endif()

if(APPLE)
    # Bundle the .icns so Finder/Dock/⌘-Tab show the app icon.
    set(APP_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns)
    target_sources(apollo_patcher_gui PRIVATE ${APP_ICNS})
    set_source_files_properties(${APP_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(apollo_patcher_gui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Apollo Save Patcher"
        MACOSX_BUNDLE_ICON_FILE icon.icns)
endif()
