# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME KeySequenceTests
  DEPENDS gui
  SOURCE KeySequenceTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

# Compiles the edge-strip widget against litekvm, without the gui library
# (that target pulls platform). Covers the enable contract MainWindow relies on.
create_test(
  NAME EdgeFileDropTests
  DEPENDS litekvm
  SOURCE EdgeFileDropTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
  LIBS Qt6::Widgets
)
target_sources(EdgeFileDropTests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/lib/gui/widgets/EdgeFileDrop.cpp
)
target_include_directories(EdgeFileDropTests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/lib/gui
  ${CMAKE_SOURCE_DIR}/src/lib
)
