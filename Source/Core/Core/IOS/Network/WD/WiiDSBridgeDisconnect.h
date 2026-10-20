// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>

#include "Common/CommonTypes.h"

namespace IOS::HLE::WD
{
struct WiiDSBridgeClient
{
  std::array<u8, 6> mac{};
  bool associated = false;
  u16 aid = 1;
};

// Consume only a real, unprotected management frame from the current peer.
// frame_size includes the FCS, as in the bridge's DS->Wii wire format.
inline std::optional<std::array<u8, 0x94>>
ConsumeWiiDSBridgeDisconnect(const u8* frame, std::size_t frame_size,
                             const std::array<u8, 6>& wii_mac, bool enabled,
                             WiiDSBridgeClient& client)
{
  if (!enabled || !client.associated || !frame || frame_size < 24 + 2 + 4 ||
      (frame[0] != 0xc0 && frame[0] != 0xa0) || (frame[1] & 0x43) != 0 ||
      std::memcmp(frame + 4, wii_mac.data(), 6) != 0 ||
      std::memcmp(frame + 10, client.mac.data(), 6) != 0 ||
      std::memcmp(frame + 16, wii_mac.data(), 6) != 0)
  {
    return std::nullopt;
  }

  // Revolution SDK HandleNotification: type 1 -> disconnected; MAC at +8,
  // AID (BE16) at +0x0e, reason (BE16) at +0x10. The on-air reason is LE16.
  // This reports the observed radio event, not a fabricated game-state change.
  std::array<u8, 0x94> notification{};
  notification[3] = 1;
  std::copy_n(frame + 10, 6, notification.begin() + 8);
  notification[0x0e] = u8(client.aid >> 8);
  notification[0x0f] = u8(client.aid);
  notification[0x10] = frame[25];
  notification[0x11] = frame[24];
  client.associated = false;  // Suppress retries and duplicate disconnects.
  return notification;
}
}  // namespace IOS::HLE::WD
