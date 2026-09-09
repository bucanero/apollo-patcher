# Dear ImGui + GLFW desktop GUI. Fetched at configure time (needs network once).
#
# The engine + apollo_ctrl facade come from the apollo_engine target defined by
# the top-level CMakeLists.txt; this file only builds the app itself.

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
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
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

if(APPLE)
    # Bundle the .icns so Finder/Dock/⌘-Tab show the app icon.
    set(APP_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns)
    target_sources(apollo_patcher_gui PRIVATE ${APP_ICNS})
    set_source_files_properties(${APP_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(apollo_patcher_gui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Apollo Patcher"
        MACOSX_BUNDLE_ICON_FILE icon.icns)
endif()
