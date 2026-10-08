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
# Or its headers and static library end up in the installers (packaging.cmake).
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
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

# The version the About box prints, from project(VERSION) in the root
# CMakeLists. Passed in rather than written in main.cpp so there is one place
# to bump. A source file compiled WITHOUT this -- by hand, or by a build that
# forgot it -- falls back to a placeholder that is obviously not a release
# rather than to a number that might be last release's; see main.cpp.
target_compile_definitions(apollo_patcher_gui PRIVATE
    APOLLO_PATCHER_VERSION="${PROJECT_VERSION}")

if(MINGW)
    # -static: statically link the MinGW runtime (libgcc / libstdc++ /
    #   winpthread) so the .exe runs on stock Windows without shipping DLLs.
    #   Windows system DLLs (opengl32, gdi32, comdlg32, …) are still linked
    #   via their import libs, which is correct.
    # -mwindows: build as a GUI-subsystem app so no console window opens
    #   behind the GUI. MinGW's WinMain shim still calls our main().
    target_link_options(apollo_patcher_gui PRIVATE -static -mwindows)
endif()

if(WIN32)
    # The .exe's own icon and version block -- see app.rc.in. Without it,
    # Explorer and the installer's Start menu shortcut show a generic one.
    enable_language(RC)
    set(APOLLO_APP_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(apollo_patcher_gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
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

# The code-type icons, baked into a header by tools/make-type-icons.py.
#
# Optional, like the patch bundle: with no art and no Python the configure step
# says so and carries on, and the type column falls back to the coloured
# letters.
#
# CONFIGURE_DEPENDS on the glob, for the same reason the patch bundle uses it.
# An EXISTS check here is evaluated ONCE, at configure time, so dropping the
# art in and building did nothing at all until cmake happened to re-run -- and
# the symptom was an app that looked unchanged with nothing anywhere saying
# why. The glob makes adding a file re-trigger configure on its own.
set(_type_icon_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons")
file(GLOB _type_icon_src CONFIGURE_DEPENDS "${_type_icon_dir}/type-*.png")

set(_type_icon_needed
    "${_type_icon_dir}/type-savewizard.png"
    "${_type_icon_dir}/type-python.png"
    "${_type_icon_dir}/type-bsd.png")

set(_type_icons_present TRUE)
foreach(_f ${_type_icon_needed})
    if(NOT EXISTS "${_f}")
        set(_type_icons_present FALSE)
        get_filename_component(_missing "${_f}" NAME)
        message(STATUS "Code-type icons: ${_missing} is missing")
    endif()
endforeach()

if(Python3_EXECUTABLE)
    set(_type_icons_h "${CMAKE_CURRENT_SOURCE_DIR}/src/type_icons.h")
    add_custom_command(
        OUTPUT  "${_type_icons_h}"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/make-type-icons.py"
                "${_type_icon_dir}" "${_type_icons_h}"
        DEPENDS ${_type_icon_src} "${CMAKE_SOURCE_DIR}/tools/make-type-icons.py"
        COMMENT "Baking the code-type icons"
        VERBATIM)
    target_sources(apollo_patcher_gui PRIVATE "${_type_icons_h}")
    # Explicit, because main.cpp reaches the header through __has_include and
    # a compile that ran while it was absent recorded no dependency on it.
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
        PROPERTIES OBJECT_DEPENDS "${_type_icons_h}")
    if(_type_icons_present)
        message(STATUS "Code-type icons: baking from ${_type_icon_dir}")
    else()
        message(STATUS
            "Code-type icons: incomplete, so the type column shows SW/PY/BSD "
            "as text. See ${_type_icon_dir}/README.md")
    endif()
else()
    message(STATUS "Code-type icons: no Python3, so the type column stays text.")
endif()

# The font, put where load_font() looks for it -- the same two places as the
# patch database, for the same reason. Copied per file rather than declared as
# a bundle resource, so the non-Apple builds get it beside the executable too.
if(APOLLO_FONT)
    if(APPLE)
        set(_font_dest "$<TARGET_BUNDLE_CONTENT_DIR:apollo_patcher_gui>/Resources")
    else()
        set(_font_dest "$<TARGET_FILE_DIR:apollo_patcher_gui>")
    endif()

    get_filename_component(_font_name "${APOLLO_FONT}" NAME)
    set(_font_stamp "${CMAKE_CURRENT_BINARY_DIR}/font-installed.stamp")
    set(_font_copy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APOLLO_FONT}" "${_font_dest}/${_font_name}")
    if(APOLLO_FONT_LICENSE)
        list(APPEND _font_copy
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${APOLLO_FONT_LICENSE}" "${_font_dest}/OFL.txt")
    endif()

    add_custom_command(
        OUTPUT  "${_font_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_font_dest}"
        ${_font_copy}
        COMMAND ${CMAKE_COMMAND} -E touch "${_font_stamp}"
        DEPENDS "${APOLLO_FONT}" apollo_patcher_gui
        COMMENT "Installing the font into the app"
        VERBATIM)
    add_custom_target(apollo_patcher_gui_font ALL DEPENDS "${_font_stamp}")
endif()

if(APPLE)
    # Bundle the .icns so Finder/Dock/⌘-Tab show the app icon.
    set(APP_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns)
    target_sources(apollo_patcher_gui PRIVATE ${APP_ICNS})
    set_source_files_properties(${APP_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

    # Files opened from Finder. A .app is handed its documents through Apple
    # Events rather than argv, so this is what makes double-clicking a save,
    # dropping one on the Dock icon and "Open With" do anything at all. The
    # Info.plist template beside it is the other half: macOS sends those
    # events only for the types an app has claimed.
    #
    # Compiled as Objective-C++ by a per-source flag, NOT by
    # enable_language(OBJCXX). Enabling the language is project-wide, and with
    # OBJC left off CMake then picks OBJCXX for every .m it can see -- which
    # means GLFW's Cocoa backend, C code that has no business being compiled
    # as C++. This keeps the change to the one file that needs it.
    #
    target_sources(apollo_patcher_gui PRIVATE src/macos_open_docs.mm)
    set_source_files_properties(src/macos_open_docs.mm PROPERTIES
        LANGUAGE CXX
        COMPILE_OPTIONS "-x;objective-c++")

    set_target_properties(apollo_patcher_gui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Apollo Save Patcher"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.bucanero.apollo-patcher"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE icon.icns
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")

    target_link_libraries(apollo_patcher_gui PRIVATE "-framework AppKit")
endif()

include(cmake/packaging.cmake)
