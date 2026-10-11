// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"

namespace IOS::HLE::WD
{
constexpr std::size_t WiiDSMPConfigSize = 16;
constexpr std::size_t WiiDSMPMaximumPayload = 2048;
constexpr u8 WiiDSMPConfiguredMessage = 7;

// WIIDSBRIDGE/1 opt-in extension, independent of the game's application bytes:
// WDSB0001 | type7 | BE16(16 + application length) | raw WD vector1 | application.
// The WD config is already big-endian guest data; never swap or reinterpret it
// on the sender. Legacy type4 remains byte-for-byte identical when disabled.
inline std::optional<std::vector<u8>> BuildWiiDSMPBridgePacket(std::span<const u8> application,
                                                               bool configured,
                                                               std::span<const u8> config = {})
{
  if (application.empty() || application.size() > WiiDSMPMaximumPayload ||
      (configured && config.size() != WiiDSMPConfigSize))
  {
    return std::nullopt;
  }

  constexpr std::array<u8, 8> magic{'W', 'D', 'S', 'B', '0', '0', '0', '1'};
  const std::size_t config_size = configured ? WiiDSMPConfigSize : 0;
  const std::size_t body_size = application.size() + config_size;
  std::vector<u8> packet(11 + body_size);
  std::copy(magic.begin(), magic.end(), packet.begin());
  packet[8] = configured ? WiiDSMPConfiguredMessage : 4;
  packet[9] = u8(body_size >> 8);
  packet[10] = u8(body_size);
  if (configured)
    std::copy(config.begin(), config.end(), packet.begin() + 11);
  std::copy(application.begin(), application.end(), packet.begin() + 11 + config_size);
  return packet;
}
}  // namespace IOS::HLE::WD
