# SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
# SPDX-License-Identifier: GPL-2.0

create_test(
  NAME LiteKvmTests
  DEPENDS litekvm
  SOURCE LiteKvmTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/litekvm"
)
