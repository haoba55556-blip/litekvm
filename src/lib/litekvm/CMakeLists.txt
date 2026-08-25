# SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
# SPDX-License-Identifier: GPL-2.0

# LiteKVM zero-config layer: device identity, trust store, pairing crypto,
# mDNS discovery/advertising, PIN pairing protocol.

find_package(OpenSSL ${REQUIRED_OPENSSL_VERSION} REQUIRED COMPONENTS Crypto)

add_library(litekvm STATIC
  DeviceIdentity.cpp
  DeviceIdentity.h
  PairingCrypto.cpp
  PairingCrypto.h
  TrustStore.cpp
  TrustStore.h
  DiscoveryService.cpp
  DiscoveryService.h
  MdnsAdvertiser.cpp
  MdnsAdvertiser.h
  PairingService.cpp
  PairingService.h
)

target_include_directories(litekvm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(litekvm
  PUBLIC common OpenSSL::Crypto Qt6::Core Qt6::Network
)
