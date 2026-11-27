// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <cstdlib>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"

namespace IOS::HLE::WD
{
struct WiiDSBridgeSettings
{
  bool enabled = false;
  bool diagnostics = false;
  std::string peer_ipv4;
};

inline std::string WiiDSBridgeSettingsPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "WiiDSBridge.ini";
}

inline WiiDSBridgeSettings LoadWiiDSBridgeSettings()
{
  Common::IniFile ini;
  ini.Load(WiiDSBridgeSettingsPath());
  const auto* section = ini.GetOrCreateSection("WiiDSBridge");
  WiiDSBridgeSettings settings;
  section->Get("Enabled", &settings.enabled, false);
  section->Get("Diagnostics", &settings.diagnostics, false);
  // Migrate old launcher setups only when no saved address exists. A saved
  // empty value deliberately selects loopback, even in an old launcher.
  const char* legacy_peer = std::getenv("WII_DS_PEER_IPV4");
  section->Get("PeerIPv4", &settings.peer_ipv4, std::string(legacy_peer ? legacy_peer : ""));
  return settings;
}

inline bool SaveWiiDSBridgeSettings(const WiiDSBridgeSettings& settings)
{
  Common::IniFile ini;
  ini.Load(WiiDSBridgeSettingsPath());
  auto* section = ini.GetOrCreateSection("WiiDSBridge");
  section->Set("Enabled", settings.enabled);
  section->Set("Diagnostics", settings.diagnostics);
  section->Set("PeerIPv4", settings.peer_ipv4);
  return ini.Save(WiiDSBridgeSettingsPath());
}
}  // namespace IOS::HLE::WD
