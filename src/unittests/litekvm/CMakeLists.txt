# SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
# SPDX-License-Identifier: GPL-2.0

create_test(
  NAME LiteKvmTests
  DEPENDS litekvm
  SOURCE LiteKvmTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/litekvm"
)

create_test(
  NAME PairingIntegrationTests
  DEPENDS litekvm
  SOURCE PairingIntegrationTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/litekvm"
)

create_test(
  NAME LayoutSyncTests
  DEPENDS litekvm
  SOURCE LayoutSyncTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/litekvm"
)
