# CPACK_PRE_BUILD_SCRIPTS hook for the macOS .dmg (see packaging.cmake): sign
# every staged .app ad hoc, so the bundle's signature covers the resources the
# build copied in after the link. Runs on CPack's staging copy, never on the
# build tree.
file(GLOB _apps LIST_DIRECTORIES true "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*.app")
if(NOT _apps)
    message(FATAL_ERROR "macos-adhoc-sign: no .app under ${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
endif()
foreach(_app IN LISTS _apps)
    execute_process(
        COMMAND codesign --force --sign - "${_app}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "macos-adhoc-sign: codesign failed on ${_app}")
    endif()
    execute_process(
        COMMAND codesign --verify --strict "${_app}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "macos-adhoc-sign: ${_app} does not verify")
    endif()
endforeach()
