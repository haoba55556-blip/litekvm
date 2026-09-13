# SPDX-FileCopyrightText: (C) 2024 - 2025 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-FileCopyrightText: (C) 2024 Symless Ltd
# SPDX-License-Identifier: MIT

if(APPLE)
  set(target Deskflow)
else()
  set(target deskflow)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(WIN32)
  set(EXE_DESCRIPTION "${CMAKE_PROJECT_PROPER_NAME} GUI configuration tool")
  set(EXE_ICON "IDI_ICON1 ICON DISCARDABLE \"${CMAKE_SOURCE_DIR}/src/apps/res/deskflow.ico\" ")
  configure_file(${CMAKE_SOURCE_DIR}/src/apps/res/windows.rc.in deskflow.rc)
  set(platform_extra deskflow.rc)
elseif(BUILD_OSX_BUNDLE)
  #setup our bundle plist file
  set(BUNDLE_EXECUTABLE_NAME "${target}")
  set(BUNDLE_BUNDLE_NAME "${target}")
  set(BUNDLE_DISPLAY_NAME "${target}")
  set(BUNDLE_GUI_IDENTIFIER "${CMAKE_PROJECT_REV_FQDN}")
  set(BUNDLE_ICON_FILE ${target}.icns)
  set(BUNDLE_INFO_STRING "${CMAKE_PROJECT_DESCRIPTION}")
  set(BUNDLE_COPYRIGHT "${CMAKE_PROJECT_COPYRIGHT}")

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../res/deskflow.plist.in
    ${CMAKE_CURRENT_BINARY_DIR}/deskflow.plist
    @ONLY
  )

  set(platform_extra ../res/Deskflow.icns)
  set_source_files_properties(${platform_extra} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

add_executable(${target} WIN32 MACOSX_BUNDLE
  ${platform_extra}
  ../res/deskflow.qrc
  deskflow-gui.cpp
)

# LiteKVM: wrap in --start-group for MinGW so circular static-lib deps
# (client<->net etc.) resolve on ld; MSVC doesn't need/understand this
if(MINGW)
  target_link_libraries(
    ${target}
    -Wl,--start-group
    gui
    common
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    -Wl,--end-group)
else()
  target_link_libraries(
    ${target}
    gui
    common
    Qt6::Core
    Qt6::Widgets
    Qt6::Network)
endif()

install(
  TARGETS ${target}
  RUNTIME_DEPENDENCY_SET guiDeps
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  BUNDLE DESTINATION .
)

if(WIN32)
  # LiteKVM: /NODEFAULTLIB:LIBCMT is MSVC-only (avoids CRT mismatch); MinGW
  # links libc via gcc driver and would choke on the flag
  if(MSVC)
    set_target_properties(${target} PROPERTIES LINK_FLAGS "/NODEFAULTLIB:LIBCMT")
  endif()

  install(RUNTIME_DEPENDENCY_SET guiDeps
    ${WIN32_RUNTIME_DEPENDENCY_SET_ARGS}
    PRE_EXCLUDE_REGEXES ${WIN32_PRE_EXCLUDE_REGEXES}
    POST_EXCLUDE_REGEXES ${WIN32_POST_EXCLUDE_REGEXES}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )

  # LiteKVM (MinGW): ship the GCC runtime next to the binaries.
  # Neither upstream path does it for a MinGW build:
  #   * InstallRequiredSystemLibraries (deploy/windows/deploy.cmake) only knows the
  #     MSVC redistributable,
  #   * windeployqt is called with --no-compiler-runtime below (upstream's MSVC
  #     convention, where the CRT comes from the installer).
  # Result without this block: the packaged app aborts on launch with
  # 0xC0000135 / "libstdc++-6.dll was not found". Resolve the DLLs through the
  # compiler driver so they always match the toolchain that built the binaries.
  if(MINGW)
    get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_mingw_dll libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll)
      execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${_mingw_dll}
        OUTPUT_VARIABLE _mingw_dll_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(NOT EXISTS "${_mingw_dll_path}" AND EXISTS "${_mingw_bin_dir}/${_mingw_dll}")
        set(_mingw_dll_path "${_mingw_bin_dir}/${_mingw_dll}")
      endif()
      if(EXISTS "${_mingw_dll_path}")
        install(PROGRAMS "${_mingw_dll_path}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
        add_custom_command(
          TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_mingw_dll_path}"
            "$<TARGET_FILE_DIR:${target}>"
          COMMENT "LiteKVM: staging MinGW runtime ${_mingw_dll}"
        )
      else()
        message(WARNING "LiteKVM: could not locate MinGW runtime DLL: ${_mingw_dll}")
      endif()
    endforeach()
  endif()

  set(QT_DEPENDS_DIR ${CMAKE_BINARY_DIR}/qt-depends)

  add_custom_command(
    TARGET ${target} POST_BUILD
    COMMAND ${DEPLOYQT}
      --no-compiler-runtime
      --no-system-d3d-compiler
      --no-opengl-sw
      --no-quick-import
      --dir "${QT_DEPENDS_DIR}"
      --plugindir "${QT_DEPENDS_DIR}/plugins"
      $<TARGET_FILE:${target}>
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
      ${QT_DEPENDS_DIR}
      $<TARGET_FILE_DIR:${target}>
  )

  install(
    DIRECTORY ${QT_DEPENDS_DIR}/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PATTERN "dx*.dll" EXCLUDE
    PATTERN "icu*.dll" EXCLUDE
  )

elseif(APPLE)
  if (BUILD_OSX_BUNDLE)
    set_target_properties(${target} PROPERTIES
      INSTALL_RPATH "@loader_path/../Libraries;@loader_path/../Frameworks"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/deskflow.plist"
    )
    if (APPLE_CODESIGN_DEV)
      configure_mac_codesign(${target})
    endif()
  else()
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE FALSE)
  endif()
else()
  generate_app_man(${target} "${CMAKE_PROJECT_DESCRIPTION} \\(GUI\\)")
endif()
