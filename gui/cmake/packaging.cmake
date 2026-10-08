# Install rules and installers for the desktop app. Included at the end of
# gui/CMakeLists.txt.
#
#   macOS    cpack -G DragNDrop   ->  ApolloSavePatcher-<ver>-macOS.dmg
#   Windows  cpack -G NSIS        ->  ApolloSavePatcher-<ver>-windows-<arch>-setup.exe
#   Linux    cmake --install build --prefix AppDir/usr, then linuxdeploy
#            ->  ApolloSavePatcher-<ver>-linux-x86_64.AppImage (CI does this;
#            CPack's own AppImage generator needs CMake 3.30)
#
# Run cpack from the build directory after a Release build. All three are
# unsigned: see docs/user-guide.md for what that means to the person
# installing it.
#
# What goes where is decided by where the app LOOKS, not by convention: the
# patch database and the font are found beside the executable, or in
# ../Resources for a .app (patchdb.c locate(), main.cpp load_font()). So on
# Windows and Linux they sit next to the binary, even in a bin/ directory.

set(_license "${CMAKE_SOURCE_DIR}/LICENSE")

if(APPLE)
    # The whole bundle, renamed on the way: the build tree's
    # apollo_patcher_gui.app is what every script and doc refers to, but the
    # name in /Applications is the one people see. The database and the font
    # are already inside it, copied in by the build.
    install(DIRECTORY "$<TARGET_BUNDLE_DIR:apollo_patcher_gui>/"
            DESTINATION "Apollo Save Patcher.app"
            USE_SOURCE_PERMISSIONS)
    install(FILES "${_license}" DESTINATION . RENAME LICENSE.txt)
    set(_bin_dest "")
elseif(WIN32)
    set(_bin_dest .)
    install(TARGETS apollo_patcher_gui RUNTIME DESTINATION ${_bin_dest} COMPONENT app)
    install(FILES "${_license}" DESTINATION . RENAME LICENSE.txt COMPONENT app)
else()
    include(GNUInstallDirs)
    set(_bin_dest ${CMAKE_INSTALL_BINDIR})
    install(TARGETS apollo_patcher_gui RUNTIME DESTINATION ${_bin_dest} COMPONENT app)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/io.github.bucanero.apollo_patcher.desktop"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT app)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/io.github.bucanero.apollo_patcher.appdata.xml"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo COMPONENT app)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-256.png"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME apollo-patcher.png COMPONENT app)
    install(FILES "${_license}"
            DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT app)
endif()

if(NOT APPLE)
    # From their sources rather than the copies beside the build's binary, so
    # these rules need no generator expressions.
    if(APOLLO_BUNDLE)
        install(FILES "${APOLLO_BUNDLE}" DESTINATION ${_bin_dest} COMPONENT app)
    endif()
    if(APOLLO_FONT)
        install(FILES "${APOLLO_FONT}" DESTINATION ${_bin_dest} COMPONENT app)
    endif()
    if(APOLLO_FONT_LICENSE)
        install(FILES "${APOLLO_FONT_LICENSE}" DESTINATION ${_bin_dest}
                RENAME OFL.txt COMPONENT app)
    endif()
endif()

# The Windows software OpenGL fallback (see the README's "Known limitations").
# Not built here: CI downloads Mesa's opengl32.dll for the right architecture
# and passes it in. It always lands in softgl\, where the error message the app
# shows when OpenGL fails tells people to look. The installer also offers it
# as an unticked component that puts it beside the .exe directly, because by
# the time someone needs it the app is in Program Files and copying a DLL
# there takes an administrator.
set(APOLLO_SOFTGL_DLL "" CACHE FILEPATH
    "Windows only: Mesa opengl32.dll to ship as the software OpenGL fallback")
if(WIN32 AND APOLLO_SOFTGL_DLL)
    install(FILES "${APOLLO_SOFTGL_DLL}" DESTINATION softgl
            RENAME opengl32.dll COMPONENT app)
    install(FILES "${APOLLO_SOFTGL_DLL}" DESTINATION .
            RENAME opengl32.dll COMPONENT softgl)
endif()

# ---------------------------------------------------------------------------
# CPack
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "Apollo Save Patcher")
set(CPACK_PACKAGE_VENDOR "bucanero")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Apply Apollo save patches to PlayStation save data")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/bucanero/apollo-patcher")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Apollo Save Patcher")
# Values reach the generators unmangled -- the NSIS registry paths below are
# full of backslashes, which would otherwise need doubling twice over.
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_STRIP_FILES ON)

if(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_PACKAGE_FILE_NAME "ApolloSavePatcher-${PROJECT_VERSION}-macOS")
    set(CPACK_DMG_VOLUME_NAME "Apollo Save Patcher")
    # The bundle has resources copied in after the link, so the linker's
    # ad-hoc signature no longer covers it. Re-sign the staged copy, ad hoc:
    # Apple Silicon refuses to run unsigned code at all, and a bundle whose
    # signature does not match its contents is reported as "damaged" rather
    # than as merely unidentified.
    set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/macos-adhoc-sign.cmake")
elseif(WIN32)
    set(CPACK_GENERATOR NSIS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_arch x64)
        set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    else()
        set(_arch x86)
    endif()
    set(CPACK_PACKAGE_FILE_NAME
        "ApolloSavePatcher-${PROJECT_VERSION}-windows-${_arch}-setup")
    set(CPACK_RESOURCE_FILE_LICENSE "${_license}")

    set(CPACK_COMPONENTS_ALL app)
    set(CPACK_COMPONENT_APP_DISPLAY_NAME "Apollo Save Patcher")
    set(CPACK_COMPONENT_APP_REQUIRED ON)
    if(APOLLO_SOFTGL_DLL)
        list(APPEND CPACK_COMPONENTS_ALL softgl)
        set(CPACK_COMPONENT_SOFTGL_DISPLAY_NAME "Software OpenGL renderer")
        # One string, not two: set() would join them with a ';'.
        set(CPACK_COMPONENT_SOFTGL_DESCRIPTION "Only for a machine with no usable graphics driver, such as over Remote Desktop or in a virtual machine. Slower than the GPU.")
        set(CPACK_COMPONENT_SOFTGL_DISABLED ON)
    endif()

    set(CPACK_NSIS_PACKAGE_NAME "Apollo Save Patcher ${PROJECT_VERSION}")
    set(CPACK_NSIS_DISPLAY_NAME "Apollo Save Patcher")
    set(CPACK_NSIS_MUI_ICON   "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "apollo_patcher_gui.exe")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_PACKAGE_EXECUTABLES "apollo_patcher_gui" "Apollo Save Patcher")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "apollo_patcher_gui.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/bucanero/apollo-patcher")
    set(CPACK_NSIS_HELP_LINK "https://bucanero.github.io/apollo-patcher/guide.html")

    # .savepatch files open in the app, as they do from Finder on macOS (see
    # Info.plist.in). Saves themselves are not claimed: a save is any file.
    # SHCTX is HKLM for an all-users install and HKCU otherwise, following the
    # choice the CPack template already made.
    set(_cls "Software\\Classes\\ApolloSavePatcher.savepatch")
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "WriteRegStr SHCTX \"Software\\Classes\\.savepatch\" \"\" \"ApolloSavePatcher.savepatch\"
  WriteRegStr SHCTX \"${_cls}\" \"\" \"Apollo save patch\"
  WriteRegStr SHCTX \"${_cls}\\DefaultIcon\" \"\" \"$INSTDIR\\apollo_patcher_gui.exe,0\"
  WriteRegStr SHCTX \"${_cls}\\shell\\open\\command\" \"\" '\"$INSTDIR\\apollo_patcher_gui.exe\" \"%1\"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'")
    # .savepatch is only released if it still points at us -- another tool may
    # have taken it over since.
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "ReadRegStr $0 SHCTX \"Software\\Classes\\.savepatch\" \"\"
  StrCmp $0 \"ApolloSavePatcher.savepatch\" 0 +2
  DeleteRegKey SHCTX \"Software\\Classes\\.savepatch\"
  DeleteRegKey SHCTX \"${_cls}\"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'")
else()
    # Linux ships as an AppImage, assembled outside CPack; see the top.
    set(CPACK_GENERATOR TGZ)
    set(CPACK_PACKAGE_FILE_NAME "ApolloSavePatcher-${PROJECT_VERSION}-linux")
endif()

include(CPack)
