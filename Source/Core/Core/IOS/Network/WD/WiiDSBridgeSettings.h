// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"

namespace IOS::HLE::WD
{
struct WiiDSBridgeSettings
{
  bool enabled = false;
  bool diagnostics = false;
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
  return settings;
}

inline bool SaveWiiDSBridgeSettings(const WiiDSBridgeSettings& settings)
{
  Common::IniFile ini;
  ini.Load(WiiDSBridgeSettingsPath());
  auto* section = ini.GetOrCreateSection("WiiDSBridge");
  section->Set("Enabled", settings.enabled);
  section->Set("Diagnostics", settings.diagnostics);
  return ini.Save(WiiDSBridgeSettingsPath());
}
}  // namespace IOS::HLE::WD
