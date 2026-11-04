// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace IOS::HLE::WD
{
// Older companions only know this fixed BSSID. P1 companions receive the
// actual Wii radio identity atomically with each native Nintendo beacon.
inline constexpr std::array<u8, 6> WiiDSBridgeLegacyParentMAC{
    0x00, 0x17, 0xab, 0x4d, 0xb9, 0xe0};
inline constexpr std::size_t WiiDSBridgeNativeBeaconSize = 2 + 128;
inline constexpr u8 WiiDSBridgeLegacyBeaconMessage = 2;
inline constexpr u8 WiiDSBridgeParentBeaconMessage = 12;

inline bool WiiDSBridgeSupportsParentIdentity(std::string_view ack)
{
  constexpr std::string_view prefix = "WIIDSBRIDGE/1 MELONDS ACK ";
  if (!ack.starts_with(prefix))
    return false;
  auto tokens = ack.substr(prefix.size());
  const auto nonce_end = tokens.find(' ');
  if (nonce_end != 16)
    return false;
  if (!std::all_of(tokens.begin(), tokens.begin() + nonce_end, [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F') ||
               (value >= 'a' && value <= 'f');
      }))
    return false;
  tokens.remove_prefix(nonce_end + 1);
  while (!tokens.empty())
  {
    const auto end = tokens.find(' ');
    if (tokens.substr(0, end) == "P1")
      return true;
    if (end == std::string_view::npos)
      break;
    tokens.remove_prefix(end + 1);
  }
  return false;
}

inline bool CanAdvertiseWiiDSBridgeParent(const std::array<u8, 6>& parent,
                                         bool parent_identity)
{
  return parent_identity || parent == WiiDSBridgeLegacyParentMAC;
}

inline std::optional<std::vector<u8>>
BuildWiiDSBridgeBeaconPacket(const std::array<u8, 6>& parent, std::span<const u8> native_beacon,
                            bool parent_identity)
{
  if (native_beacon.size() != WiiDSBridgeNativeBeaconSize ||
      !CanAdvertiseWiiDSBridgeParent(parent, parent_identity))
    return std::nullopt;
  constexpr std::array<u8, 8> magic{'W', 'D', 'S', 'B', '0', '0', '0', '1'};
  const std::size_t identity_size = parent_identity ? parent.size() : 0;
  const std::size_t payload_size = identity_size + native_beacon.size();
  std::vector<u8> packet(11 + payload_size);
  std::copy(magic.begin(), magic.end(), packet.begin());
  packet[8] = parent_identity ? WiiDSBridgeParentBeaconMessage : WiiDSBridgeLegacyBeaconMessage;
  packet[9] = u8(payload_size >> 8);
  packet[10] = u8(payload_size);
  if (parent_identity)
    std::copy(parent.begin(), parent.end(), packet.begin() + 11);
  std::copy(native_beacon.begin(), native_beacon.end(), packet.begin() + 11 + identity_size);
  return packet;
}
}  // namespace IOS::HLE::WD
