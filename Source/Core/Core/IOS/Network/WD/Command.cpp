// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/WD/Command.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Network/MACUtils.h"
#include "Core/IOS/Network/WD/WiiDSBridgeMPConfig.h"
#include "Core/IOS/Network/WD/WiiDSBridgeSettings.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// clang-format off
//                           Channel:   FEDC BA98 7654 3210
constexpr u16 LegalChannelMask =      0b0111'1111'1111'1110u;
constexpr u16 LegalNitroChannelMask = 0b0011'1111'1111'1110u;
constexpr unsigned short WiiDSBridgePort = 7065;
// The bridge is a single-parent virtual radio. Keep WD_GetInfo and raw
// management replies consistent with the BSSID used by the melonDS radio.
constexpr Common::MACAddress WiiDSBridgeMAC{0x00, 0x17, 0xab, 0x4d, 0xb9, 0xe0};
constexpr std::string_view WiiDSBridgeHello = "WIIDSBRIDGE/1 DOLPHIN HELLO";
constexpr std::string_view WiiDSBridgeAck = "WIIDSBRIDGE/1 MELONDS ACK";
constexpr std::array<u8, 8> WiiDSBridgeBinaryMagic{'W', 'D', 'S', 'B', '0', '0', '0', '1'};
constexpr u8 WiiDSBridgeBeaconMessage = 2;
constexpr u8 WiiDSBridgeDSFrameMessage = 3;
constexpr u8 WiiDSBridgeWiiRawFrameMessage = 5;
constexpr u8 WiiDSBridgeBeaconSentMessage = 6;
constexpr std::size_t WiiDSRadioMetadataSize = 12;
constexpr std::size_t IEEE80211HeaderSize = 24;
constexpr std::size_t IEEE80211FCSSize = 4;
constexpr std::size_t WDReceiveHeaderSize = 0x12;
constexpr u64 WiiDSDiagnosticMaxFileSize = 8 * 1024 * 1024;
// clang-format on

u16 SelectWifiChannel(u16 enabled_channels_mask, u16 current_channel)
{
  const Common::BitSet<u16> enabled_channels(enabled_channels_mask & LegalChannelMask);
  u16 next_channel = current_channel;
  for (int i = 0; i < 16; ++i)
  {
    next_channel = (next_channel + 3) % 16;
    if (enabled_channels[next_channel])
      return next_channel;
  }
  // This does not make a lot of sense, but it is what WD does.
  return u16(enabled_channels[next_channel]);
}

u16 MakeNitroAllowedChannelMask(u16 enabled_channels_mask, u16 nitro_mask)
{
  nitro_mask &= LegalNitroChannelMask;
  // TODO: WD's version of this function has some complicated logic to determine the actual mask.
  return enabled_channels_mask & nitro_mask;
}

bool DiagnosticSample(u64 count)
{
  return count <= 16 || count % 256 == 0;
}
}  // namespace

NetWDCommandDevice::Status NetWDCommandDevice::GetTargetStatusForMode(WD::Mode mode)
{
  switch (mode)
  {
  case WD::Mode::DSCommunications:
    return Status::ScanningForDS;
  case WD::Mode::AOSSAccessPointScan:
    return Status::ScanningForAOSSAccessPoint;
  default:
    return Status::Idle;
  }
}

NetWDCommandDevice::NetWDCommandDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
  // TODO: use the MPCH setting in setting.txt to determine this value.
  m_nitro_enabled_channels = LegalNitroChannelMask;

  // TODO: Set the version string here. This is exposed to the PPC.
  m_info.mac = Net::GetMACAddress();
  m_info.enabled_channels = 0xfffe;
  m_info.channel = SelectWifiChannel(m_info.enabled_channels, 0);
  // The country code is supposed to be null terminated as it is logged with printf in WD.
  std::strncpy(m_info.country_code.data(), "US", m_info.country_code.size());
  m_info.nitro_allowed_channels =
      MakeNitroAllowedChannelMask(m_info.enabled_channels, m_nitro_enabled_channels);
  m_info.initialised = true;
  m_wii_ds_bridge_socket.setBlocking(false);
  const auto settings = WD::LoadWiiDSBridgeSettings();
  m_wii_ds_bridge_enabled = settings.enabled;
  m_wii_ds_bridge_diagnostics = settings.diagnostics;
  // These are part of the native backend, not optional protocol variants.
  m_wii_ds_native_disconnect_enabled = true;
  m_wii_ds_mp_config_enabled = true;
  if (m_wii_ds_bridge_enabled)
  {
    // This is scoped to the virtual WD device, never the user's ordinary
    // networking MAC or Config::MAIN_WIRELESS_MAC.
    m_info.mac = WiiDSBridgeMAC;
    const auto status = m_wii_ds_bridge_socket.bind(sf::Socket::AnyPort, sf::IpAddress::LocalHost);
    m_wii_ds_bridge_last_bind = std::chrono::steady_clock::now();
    WriteBridgeDiagnostic(fmt::format("event=DEVICE_READY bindStatus={} port={}",
                                      static_cast<int>(status),
                                      m_wii_ds_bridge_socket.getLocalPort()));
  }
}

NetWDCommandDevice::~NetWDCommandDevice()
{
  StopBridgeSession();
}

void NetWDCommandDevice::WriteBridgeDiagnostic(std::string_view message) const
{
  if (!m_wii_ds_bridge_enabled || !m_wii_ds_bridge_diagnostics)
    return;
  const auto path = File::GetUserPath(D_LOGS_IDX) + "wii-ds-bridge.log";
  if (!File::CreateFullPath(path))
    return;
  File::IOFile log(path, "ab");
  if (!log || log.GetSize() >= WiiDSDiagnosticMaxFileSize)
    return;
  // Only bounded scalar records are written here, never game payloads or guest-memory probes.
  const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  std::string body(message.substr(0, 384));
  std::replace(body.begin(), body.end(), '\n', ' ');
  std::replace(body.begin(), body.end(), '\r', ' ');
  const auto record = fmt::format("unixMs={} {}\n", time, body);
  if (log.GetSize() + record.size() <= WiiDSDiagnosticMaxFileSize)
    log.WriteString(record);
}

void NetWDCommandDevice::Update()
{
  Device::Update();
  ProcessRecvRequests();
  HandleStateChange();

  if (m_wii_ds_bridge_enabled && m_mode == WD::Mode::DSCommunications &&
      m_status == Status::ScanningForDS)
  {
    const auto now = std::chrono::steady_clock::now();
    if (m_wii_ds_bridge_socket.getLocalPort() == 0)
    {
      // Never let send() implicitly create an all-interface socket after a
      // failed explicit loopback bind. Retry without blocking the guest.
      if (now - m_wii_ds_bridge_last_bind < std::chrono::seconds(1))
        return;
      m_wii_ds_bridge_last_bind = now;
      const auto status = m_wii_ds_bridge_socket.bind(sf::Socket::AnyPort, sf::IpAddress::LocalHost);
      WriteBridgeDiagnostic(fmt::format("event=BIND_RETRY status={} port={}",
                                        static_cast<int>(status),
                                        m_wii_ds_bridge_socket.getLocalPort()));
      if (status != sf::Socket::Status::Done)
        return;
    }
    if (!m_wii_ds_bridge_announced ||
        now - m_wii_ds_bridge_last_hello >= std::chrono::milliseconds(500))
    {
      const auto status =
          m_wii_ds_bridge_socket.send(WiiDSBridgeHello.data(), WiiDSBridgeHello.size(),
                                      sf::IpAddress::LocalHost, WiiDSBridgePort);
      if (DiagnosticSample(++m_wii_ds_diagnostic_hellos))
        WriteBridgeDiagnostic(fmt::format("event=HELLO n={} status={}", m_wii_ds_diagnostic_hellos,
                                          static_cast<int>(status)));
      m_wii_ds_bridge_announced = true;
      m_wii_ds_bridge_last_hello = now;
    }
    if (m_wii_ds_bridge_connected && now - m_wii_ds_bridge_last_ack >= std::chrono::seconds(3))
    {
      ClearBridgePeer();
      WriteBridgeDiagnostic("event=PEER_EXPIRED");
      if (m_wii_ds_bridge_diagnostics)
        INFO_LOG_FMT(IOS_NET, "Wii-DS bridge: peer heartbeat expired");
    }

    // Bound work per IOS tick, but drain batches to avoid accumulating stale radio packets.
    for (unsigned int packet_count = 0; packet_count < 64; ++packet_count)
    {
      std::array<char, 4096> reply{};
      std::size_t received = 0;
      std::optional<sf::IpAddress> sender;
      unsigned short sender_port = 0;
      if (m_wii_ds_bridge_socket.receive(reply.data(), reply.size(), received, sender,
                                         sender_port) != sf::Socket::Status::Done)
        break;
      if (sender != sf::IpAddress::LocalHost || sender_port != WiiDSBridgePort)
        continue;
      const std::string_view message(reply.data(), received);
      if (message.size() <= 128 &&
          (message == WiiDSBridgeAck ||
           (message.starts_with(WiiDSBridgeAck) && message.size() > WiiDSBridgeAck.size() &&
            message[WiiDSBridgeAck.size()] == ' ')))
      {
        const bool changed = !m_wii_ds_bridge_connected || message != m_wii_ds_bridge_peer_identity;
        if (changed)
        {
          ClearBridgePeer();
          m_wii_ds_bridge_peer_identity = message;
          WriteBridgeDiagnostic("event=PEER_READY");
          if (m_wii_ds_bridge_diagnostics)
            INFO_LOG_FMT(IOS_NET, "Wii-DS bridge: melonDS peer ready ({})", message);
        }
        m_wii_ds_bridge_connected = true;
        m_wii_ds_bridge_last_ack = now;
        if (changed && !m_wii_ds_bridge_beacon.empty())
        {
          const auto status = m_wii_ds_bridge_socket.send(
              m_wii_ds_bridge_beacon.data(), m_wii_ds_bridge_beacon.size(),
              sf::IpAddress::LocalHost, WiiDSBridgePort);
          WriteBridgeDiagnostic(fmt::format("event=BEACON_REPLAY bytes={} status={}",
                                            m_wii_ds_bridge_beacon.size(),
                                            static_cast<int>(status)));
        }
      }
      else if (m_wii_ds_bridge_connected && received == 11 && sender == sf::IpAddress::LocalHost &&
               sender_port == WiiDSBridgePort &&
               std::equal(WiiDSBridgeBinaryMagic.begin(), WiiDSBridgeBinaryMagic.end(),
                          reply.begin()) &&
               u8(reply[8]) == WiiDSBridgeBeaconSentMessage && reply[9] == 0 && reply[10] == 0)
      {
        // Completion comes from the virtual radio's beacon emission, not the
        // ChangeBeacon IPC return. The SDK can arm its waiting request later.
        // Each event completes one pending SDK beacon-update request. Preserve
        // event order with association notifications; later emissions also
        // service requests armed after an earlier beacon was already reported.
        std::array<u8, 0x94> notification{};
        notification[3] = 3;  // WD beacon-sent event type, big-endian u32.
        if (m_wii_ds_bridge_notifications.size() < 256)
          m_wii_ds_bridge_notifications.emplace_back(notification);
        ++m_wii_ds_beacon_sent_events;
        if (DiagnosticSample(m_wii_ds_beacon_sent_events))
          WriteBridgeDiagnostic(fmt::format("event=BEACON_SENT n={}", m_wii_ds_beacon_sent_events));
        if (m_wii_ds_beacon_sent_events <= 4 || m_wii_ds_beacon_sent_events % 256 == 0)
          if (m_wii_ds_bridge_diagnostics)
            INFO_LOG_FMT(IOS_NET, "Wii-DS bridge: virtual radio reported beacon sent n={}",
                         m_wii_ds_beacon_sent_events);
      }
      else if (m_wii_ds_bridge_connected && received >= 23 &&
               std::equal(WiiDSBridgeBinaryMagic.begin(), WiiDSBridgeBinaryMagic.end(),
                          reply.begin()) &&
               u8(reply[8]) == WiiDSBridgeDSFrameMessage)
      {
        const std::size_t length = (u8(reply[9]) << 8) | u8(reply[10]);
        if (received == 11 + length && length >= WiiDSRadioMetadataSize + IEEE80211HeaderSize)
        {
          const auto* metadata = reinterpret_cast<const u8*>(reply.data() + 11);
          const auto* frame = metadata + WiiDSRadioMetadataSize;
          const u8 channel = metadata[9];
          ++m_wii_ds_diagnostic_rx_frames;
          if ((frame[0] & 0x0c) == 0 || DiagnosticSample(m_wii_ds_diagnostic_rx_frames))
            WriteBridgeDiagnostic(fmt::format(
                "event=DS_FRAME n={} bytes={} fc={:02x}{:02x} channel={}",
                m_wii_ds_diagnostic_rx_frames, length - 12, frame[0], frame[1], channel));
          if (m_wii_ds_bridge_diagnostics)
            INFO_LOG_FMT(IOS_NET,
                         "Wii-DS bridge: DS->Wii frame len={} fc={:02x}{:02x} "
                         "dst={:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                         length - 12, frame[0], frame[1], frame[4], frame[5], frame[6], frame[7],
                         frame[8], frame[9]);
          const u8 frame_control = frame[0];
          const u8 frame_type = frame_control & 0x0c;
          // This adapter represents one parent. Do not answer management frames
          // for another BSSID or protected/non-local radio exchanges.
          const bool parent_management =
              frame_type == 0 && channel >= 1 && channel <= 13 && (frame[1] & 0x43) == 0 &&
              std::memcmp(frame + 4, m_info.mac.data(), m_info.mac.size()) == 0 &&
              std::memcmp(frame + 16, m_info.mac.data(), m_info.mac.size()) == 0 &&
              (frame[10] & 1) == 0;
          if (parent_management && frame_control == 0xb0 &&
              length >= WiiDSRadioMetadataSize + IEEE80211HeaderSize + 6 + IEEE80211FCSSize &&
              frame[24] == 0 && frame[25] == 0 && frame[26] == 1 && frame[27] == 0 &&
              frame[28] == 0 && frame[29] == 0)
          {
            SendBridgeManagementResponse(frame, length - WiiDSRadioMetadataSize, 0xb0, channel);
            if (m_wii_ds_bridge_diagnostics)
              INFO_LOG_FMT(IOS_NET,
                           "Wii-DS bridge: answered DS open-system authentication request");
          }
          else if (parent_management && frame_control == 0x00 &&
                   length >= WiiDSRadioMetadataSize + IEEE80211HeaderSize + 4 + IEEE80211FCSSize)
          {
            SendBridgeManagementResponse(frame, length - WiiDSRadioMetadataSize, 0x10, channel);

            // The real WD driver reports a client only after association has completed.
            std::array<u8, 0x94> notification{};
            std::copy_n(frame + 10, 6, notification.begin() + 8);  // DS source MAC
            notification[0x0e] = 0;
            notification[0x0f] = 1;  // WD logical client channel/AID 1, big endian
            if (m_wii_ds_bridge_notifications.size() < 256)
              m_wii_ds_bridge_notifications.emplace_back(notification);
            std::copy_n(frame + 10, 6, m_wii_ds_bridge_client.mac.begin());
            m_wii_ds_bridge_client.associated = true;
            WriteBridgeDiagnostic("event=ASSOCIATED aid=1");
            if (m_wii_ds_bridge_diagnostics)
              INFO_LOG_FMT(IOS_NET,
                           "Wii-DS bridge: answered association and queued connected notification "
                           "for DS {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} (AID 1)",
                           frame[10], frame[11], frame[12], frame[13], frame[14], frame[15]);
          }
          else if (frame_type == 0 && (frame_control == 0xc0 || frame_control == 0xa0))
          {
            const bool trusted_peer =
                sender == sf::IpAddress::LocalHost && sender_port == WiiDSBridgePort;
            if (auto notification = WD::ConsumeWiiDSBridgeDisconnect(
                    frame, length - WiiDSRadioMetadataSize, m_info.mac,
                    m_wii_ds_native_disconnect_enabled && trusted_peer, m_wii_ds_bridge_client))
            {
              if (m_wii_ds_bridge_notifications.size() < 256)
                m_wii_ds_bridge_notifications.emplace_back(*notification);
              const u16 reason =
                  static_cast<u16>((u16((*notification)[0x10]) << 8) | (*notification)[0x11]);
              WriteBridgeDiagnostic(fmt::format("event=DS_DISCONNECT subtype={:02x} reason={}",
                                                frame_control, reason));
              if (m_wii_ds_bridge_diagnostics)
                INFO_LOG_FMT(IOS_NET,
                             "Wii-DS bridge: received real DS disconnect subtype={:#04x} reason={} "
                             "and queued disconnected notification (type 1, AID 1)",
                             frame_control, reason);
            }
          }
          else if (frame_type == 0x08)
          {
            if (m_wii_ds_bridge_received_frames.size() < 128)
              m_wii_ds_bridge_received_frames.emplace_back(
                  reinterpret_cast<const u8*>(reply.data() + 23),
                  reinterpret_cast<const u8*>(reply.data() + 11 + length));
          }
        }
      }
    }
  }
}

// Clear only incoming radio state; pending guest IPC requests must retain their
// identity until completed or invalidated by WD close.
void NetWDCommandDevice::ClearBridgePeer()
{
  m_wii_ds_bridge_connected = false;
  m_wii_ds_bridge_peer_identity.clear();
  m_wii_ds_bridge_client = {};
  m_wii_ds_bridge_notifications.clear();
  m_wii_ds_bridge_received_frames.clear();
}

void NetWDCommandDevice::StopBridgeSession()
{
  WriteBridgeDiagnostic(fmt::format("event=SESSION_STOP announced={} connected={}",
                                    m_wii_ds_bridge_announced, m_wii_ds_bridge_connected));
  if (m_wii_ds_bridge_enabled && m_wii_ds_bridge_announced)
  {
    std::array<u8, 11> stop{};
    std::copy(WiiDSBridgeBinaryMagic.begin(), WiiDSBridgeBinaryMagic.end(), stop.begin());
    stop[8] = 8;  // WIIDSBRIDGE/1 STOP: no body.
    (void)m_wii_ds_bridge_socket.send(stop.data(), stop.size(), sf::IpAddress::LocalHost,
                                      WiiDSBridgePort);
  }
  ClearBridgePeer();
  m_wii_ds_bridge_announced = false;
  m_wii_ds_bridge_beacon.clear();
  if (!m_wii_ds_bridge_enabled)
    return;
  // Drop already queued replies from the preceding native session.
  std::array<char, 4096> packet{};
  std::optional<sf::IpAddress> sender;
  unsigned short port = 0;
  std::size_t size = 0;
  for (unsigned int count = 0; count < 64; ++count)
  {
    if (m_wii_ds_bridge_socket.receive(packet.data(), packet.size(), size, sender, port) !=
        sf::Socket::Status::Done)
      break;
  }
}

void NetWDCommandDevice::SendBridgeManagementResponse(const u8* request_frame,
                                                      std::size_t frame_size, u8 response_subtype,
                                                      u8 channel)
{
  if (frame_size < IEEE80211HeaderSize)
    return;

  const bool association = response_subtype == 0x10;
  // Bits 14 and 15 are required in the 802.11 association-ID field.  The
  // station ID is still 1 after the client masks those reserved bits.
  // Bodies taken byte-for-byte from the successful real-Wii exchange.  The
  // Nintendo vendor IE and full rate set are part of both driver responses.
  const std::array<u8, 33> association_body{0x21, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x01, 0x08, 0x82,
                                            0x84, 0x0b, 0x16, 0x24, 0x30, 0x48, 0x6c, 0x32, 0x04,
                                            0x0c, 0x12, 0x18, 0x60, 0xdd, 0x09, 0x00, 0x10, 0x18,
                                            0x02, 0x00, 0xf0, 0x00, 0x00, 0x00};
  const std::array<u8, 17> authentication_body{0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xdd, 0x09, 0x00,
                                               0x10, 0x18, 0x02, 0x00, 0xf0, 0x00, 0x00, 0x00};
  const std::size_t body_size = association ? association_body.size() : authentication_body.size();
  const std::size_t raw_size = IEEE80211HeaderSize + body_size + IEEE80211FCSSize;
  std::vector<u8> packet(11 + WiiDSRadioMetadataSize + raw_size, 0);
  std::copy(WiiDSBridgeBinaryMagic.begin(), WiiDSBridgeBinaryMagic.end(), packet.begin());
  packet[8] = WiiDSBridgeWiiRawFrameMessage;
  const std::size_t bridge_size = WiiDSRadioMetadataSize + raw_size;
  packet[9] = u8(bridge_size >> 8);
  packet[10] = u8(bridge_size);

  u8* metadata = packet.data() + 11;
  metadata[8] = 0x0a;  // real Wii management responses use 1 Mbit/s
  metadata[9] = channel;
  metadata[10] = u8(raw_size);
  metadata[11] = u8(raw_size >> 8);
  u8* response = metadata + WiiDSRadioMetadataSize;
  response[0] = response_subtype;
  response[2] = 0x3a;
  response[3] = 0x01;                                // captured response duration: 314 us
  std::copy_n(request_frame + 10, 6, response + 4);  // DS destination
  std::copy(m_info.mac.begin(), m_info.mac.end(), response + 10);  // Wii source
  std::copy(m_info.mac.begin(), m_info.mac.end(), response + 16);  // Wii BSSID
  if (association)
    std::copy(association_body.begin(), association_body.end(), response + IEEE80211HeaderSize);
  else
    std::copy(authentication_body.begin(), authentication_body.end(),
              response + IEEE80211HeaderSize);

  const auto status = m_wii_ds_bridge_socket.send(packet.data(), packet.size(),
                                                  sf::IpAddress::LocalHost, WiiDSBridgePort);
  WriteBridgeDiagnostic(fmt::format("event=MANAGEMENT_RESPONSE subtype={:02x} channel={} status={}",
                                    response_subtype, channel, static_cast<int>(status)));
  if (m_wii_ds_bridge_diagnostics)
    INFO_LOG_FMT(IOS_NET, "Wii-DS bridge: sent raw management response subtype {:#04x} (status {})",
                 response_subtype, static_cast<int>(status));
}

void NetWDCommandDevice::ProcessRecvRequests()
{
  auto& system = GetSystem();

  while (!m_recv_notification_requests.empty() && !m_wii_ds_bridge_notifications.empty())
  {
    const u32 address = m_recv_notification_requests.front();
    const IOCtlVRequest request{system, address};
    const auto* output = request.GetVector(0);
    const auto notification = m_wii_ds_bridge_notifications.front();
    const bool beacon_sent = notification[3] == 3;
    const bool disconnected = notification[3] == 1;
    if (!output || output->address == 0 || output->size < notification.size())
    {
      GetEmulationKernel().EnqueueIPCReply(Request{system, address},
                                           s32(ResultCode::IllegalParameter));
    }
    else
    {
      system.GetMemory().CopyToEmu(output->address, notification.data(), notification.size());
      ++m_wii_ds_diagnostic_notifications;
      if (!beacon_sent || DiagnosticSample(m_wii_ds_diagnostic_notifications))
        WriteBridgeDiagnostic(fmt::format("event=WD_NOTIFICATION n={} type={} aid={}",
                                          m_wii_ds_diagnostic_notifications, notification[3],
                                          (u16(notification[0x0e]) << 8) | notification[0x0f]));
      if (m_wii_ds_bridge_diagnostics)
        INFO_LOG_FMT(IOS_NET, "Wii-DS bridge: completed WD_ReceiveNotification ({})",
                     beacon_sent  ? "beacon-sent, type 3" :
                     disconnected ? "disconnected, type 1, AID 1" :
                                    "connected, AID 1");
      // WD_ReceiveNotification returns zero through IOS; the event is in the output buffer.
      GetEmulationKernel().EnqueueIPCReply(Request{system, address}, IPC_SUCCESS);
    }
    m_recv_notification_requests.pop_front();
    m_wii_ds_bridge_notifications.pop_front();
  }

  while (!m_recv_frame_requests.empty() && !m_wii_ds_bridge_received_frames.empty())
  {
    const u32 address = m_recv_frame_requests.front();
    const IOCtlVRequest request{system, address};
    const auto* output = request.GetVector(0);
    auto frame = std::move(m_wii_ds_bridge_received_frames.front());
    if (!output || output->address == 0)
    {
      GetEmulationKernel().EnqueueIPCReply(Request{system, address},
                                           s32(ResultCode::IllegalParameter));
    }
    else
    {
      // WD does not expose a raw 802.11 packet to Wii software.  It reports an
      // MP-end indication: a 10-byte header, an 8-byte per-client record, then
      // the Nintendo MP payload.  This layout is consumed by Revolution SDK's
      // ParseMPEndIndication and is also visible in wii-ds-rom-sender.
      const std::size_t payload_offset = IEEE80211HeaderSize;
      const std::size_t payload_length = frame.size() > IEEE80211HeaderSize + IEEE80211FCSSize ?
                                             frame.size() - IEEE80211HeaderSize - IEEE80211FCSSize :
                                             0;
      const u32 result_length = static_cast<u32>(WDReceiveHeaderSize + payload_length);
      if (output->size < result_length)
      {
        GetEmulationKernel().EnqueueIPCReply(Request{system, address},
                                             s32(ResultCode::IllegalParameter));
      }
      else
      {
        std::vector<u8> wd_frame(result_length, 0);
        // All multi-byte fields are big-endian in the PPC output buffer.
        wd_frame[0x04] = 0;
        wd_frame[0x05] = 1;  // one client record
        const u16 record_size = static_cast<u16>(8 + payload_length);
        wd_frame[0x06] = static_cast<u8>(record_size >> 8);
        wd_frame[0x07] = static_cast<u8>(record_size);
        wd_frame[0x0a] = static_cast<u8>(payload_length >> 8);
        wd_frame[0x0b] = static_cast<u8>(payload_length);
        wd_frame[0x0c] = 0;
        wd_frame[0x0d] = 0x40;  // stable synthetic RSSI
        wd_frame[0x0e] = 0;
        wd_frame[0x0f] = 1;  // association ID
        if (payload_length != 0)
          std::copy_n(frame.begin() + payload_offset, payload_length,
                      wd_frame.begin() + WDReceiveHeaderSize);
        system.GetMemory().CopyToEmu(output->address, wd_frame.data(), wd_frame.size());
        if (DiagnosticSample(++m_wii_ds_diagnostic_recv_frames))
          WriteBridgeDiagnostic(fmt::format("event=WD_RECEIVE n={} payloadBytes={} totalBytes={}",
                                            m_wii_ds_diagnostic_recv_frames, payload_length,
                                            result_length));
        if (m_wii_ds_bridge_diagnostics)
          INFO_LOG_FMT(
              IOS_NET,
              "Wii-DS bridge: completed WD_ReceiveFrame envelope with {} payload bytes ({} total)",
              payload_length, result_length);
        GetEmulationKernel().EnqueueIPCReply(Request{system, address}, result_length);
      }
    }
    m_recv_frame_requests.pop_front();
    m_wii_ds_bridge_received_frames.pop_front();
  }

  // All pending requests must still be processed when the handle to the resource manager is closed.
  const bool force_process = m_clear_all_requests.TestAndClear();

  const auto process_queue = [&](std::deque<u32>& queue) {
    if (!force_process)
      return;

    while (!queue.empty())
    {
      const auto request = queue.front();
      s32 result;

      // If the resource manager handle is closed while processing a request,
      // InvalidFd is returned.
      if (m_ipc_owner_fd < 0)
      {
        result = s32(ResultCode::InvalidFd);
      }
      else
      {
        // TODO: Frame/notification data would be copied here.
        // And result would be set to the data length or to an error code.
        result = 0;
      }

      INFO_LOG_FMT(IOS_NET, "Processed request {:08x} (result {:08x})", request, result);
      GetEmulationKernel().EnqueueIPCReply(Request{system, request}, result);
      queue.pop_front();
    }
  };

  process_queue(m_recv_notification_requests);
  process_queue(m_recv_frame_requests);
}

void NetWDCommandDevice::HandleStateChange()
{
  const auto status = m_status;
  const auto target_status = m_target_status;

  if (status == target_status)
    return;

  INFO_LOG_FMT(IOS_NET, "{}: Handling status change ({} -> {})", __func__, status, target_status);

  switch (status)
  {
  case Status::Idle:
    switch (target_status)
    {
    case Status::ScanningForAOSSAccessPoint:
      // This is supposed to reset the driver first by going into another state.
      // However, we can worry about that once we actually emulate WL.
      m_status = Status::ScanningForAOSSAccessPoint;
      break;
    case Status::ScanningForDS:
      // This is supposed to set a bunch of Wi-Fi driver parameters and initiate a scan.
      m_status = Status::ScanningForDS;
      break;
    case Status::Idle:
      break;
    }
    break;

  case Status::ScanningForDS:
    m_status = Status::Idle;
    break;

  case Status::ScanningForAOSSAccessPoint:
    // We are supposed to reset the driver by going into a reset state.
    // However, we can worry about that once we actually emulate WL.
    break;
  }

  INFO_LOG_FMT(IOS_NET, "{}: done (status: {} -> {}, target was {})", __func__, status, m_status,
               target_status);
}

void NetWDCommandDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);
  p.Do(m_ipc_owner_fd);
  p.Do(m_mode);
  p.Do(m_buffer_flags);
  p.Do(m_status);
  p.Do(m_target_status);
  p.Do(m_nitro_enabled_channels);
  p.Do(m_info);
  p.Do(m_recv_frame_requests);
  p.Do(m_recv_notification_requests);
  // A restored savestate cannot restore the live localhost radio peer.
  if (p.IsReadMode())
  {
    StopBridgeSession();
    // Offline states made with the bridge disabled contain the normal random
    // WD MAC. Preserve the enabled virtual radio's identity after restoring.
    if (m_wii_ds_bridge_enabled)
      m_info.mac = WiiDSBridgeMAC;
  }
}

std::optional<IPCReply> NetWDCommandDevice::Open(const OpenRequest& request)
{
  if (m_ipc_owner_fd < 0)
  {
    const auto flags = u32(request.flags);
    const auto mode = WD::Mode(flags & 0xFFFF);
    const auto buffer_flags = flags & 0x7FFF0000;
    INFO_LOG_FMT(IOS_NET, "Opening with mode={} buffer_flags={:08x}", mode, buffer_flags);
    WriteBridgeDiagnostic(fmt::format("event=WD_OPEN mode={} bufferFlags={:08x}",
                                      static_cast<u32>(mode), buffer_flags));

    // We don't support anything other than mode 1 and mode 3 at the moment.
    if (mode != WD::Mode::DSCommunications && mode != WD::Mode::AOSSAccessPointScan)
    {
      ERROR_LOG_FMT(IOS_NET, "Unsupported WD operating mode: {}", mode);
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesUncommonWDMode);
      return IPCReply(s32(ResultCode::UnavailableCommand));
    }

    if (m_target_status == Status::Idle && mode <= WD::Mode::Unknown6)
    {
      m_mode = mode;
      m_ipc_owner_fd = request.fd;
      m_buffer_flags = buffer_flags;
    }
  }

  INFO_LOG_FMT(IOS_NET, "Opened");
  return Device::Open(request);
}

std::optional<IPCReply> NetWDCommandDevice::Close(u32 fd)
{
  if (m_ipc_owner_fd < 0 || fd != u32(m_ipc_owner_fd))
  {
    ERROR_LOG_FMT(IOS_NET, "Invalid close attempt.");
    return IPCReply(u32(ResultCode::InvalidFd));
  }

  INFO_LOG_FMT(IOS_NET, "Closing and resetting status to Idle");
  WriteBridgeDiagnostic("event=WD_CLOSE");
  m_target_status = m_status = Status::Idle;
  StopBridgeSession();
  m_wii_ds_beacon_sent_events = 0;

  m_ipc_owner_fd = -1;
  m_clear_all_requests.Set();
  return Device::Close(fd);
}

IPCReply NetWDCommandDevice::SetLinkState(const IOCtlVRequest& request)
{
  const auto* vector = request.GetVector(0);
  if (!vector || vector->address == 0)
    return IPCReply(u32(ResultCode::IllegalParameter));

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const u32 state = memory.Read_U32(vector->address);
  INFO_LOG_FMT(IOS_NET, "WD_SetLinkState called (state={}, mode={})", state, m_mode);
  WriteBridgeDiagnostic(
      fmt::format("event=WD_LINK_STATE state={} mode={}", state, static_cast<u32>(m_mode)));

  if (state == 0)
  {
    if (!WD::IsValidMode(m_mode))
      return IPCReply(u32(ResultCode::UnavailableCommand));

    StopBridgeSession();
    m_target_status = Status::Idle;
    INFO_LOG_FMT(IOS_NET, "WD_SetLinkState: setting target status to {}", m_target_status);
  }
  else
  {
    if (state != 1)
      return IPCReply(u32(ResultCode::IllegalParameter));

    if (!WD::IsValidMode(m_mode))
      return IPCReply(u32(ResultCode::UnavailableCommand));

    const auto target_status = GetTargetStatusForMode(m_mode);
    if (m_status != target_status && m_info.enabled_channels == 0)
      return IPCReply(u32(ResultCode::UnavailableCommand));

    INFO_LOG_FMT(IOS_NET, "WD_SetLinkState: setting target status to {}", target_status);
    m_target_status = target_status;
  }

  return IPCReply(IPC_SUCCESS);
}

IPCReply NetWDCommandDevice::GetLinkState(const IOCtlVRequest& request) const
{
  INFO_LOG_FMT(IOS_NET, "WD_GetLinkState called (status={}, mode={})", m_status, m_mode);
  if (!WD::IsValidMode(m_mode))
    return IPCReply(u32(ResultCode::UnavailableCommand));

  // Contrary to what the name of the ioctl suggests, this returns a boolean, not the current state.
  return IPCReply(u32(m_status == GetTargetStatusForMode(m_mode)));
}

IPCReply NetWDCommandDevice::Disassociate(const IOCtlVRequest& request)
{
  const auto* vector = request.GetVector(0);
  if (!vector || vector->address == 0)
    return IPCReply(u32(ResultCode::IllegalParameter));

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  Common::MACAddress mac;
  memory.CopyFromEmu(mac.data(), vector->address, mac.size());

  INFO_LOG_FMT(IOS_NET, "WD_Disassociate: MAC {}", Common::MacAddressToString(mac));

  if (m_mode != WD::Mode::DSCommunications && m_mode != WD::Mode::Unknown5 &&
      m_mode != WD::Mode::Unknown6)
  {
    ERROR_LOG_FMT(IOS_NET, "WD_Disassociate: cannot disassociate in mode {}", m_mode);
    return IPCReply(u32(ResultCode::UnavailableCommand));
  }

  const auto target_status = GetTargetStatusForMode(m_mode);
  if (m_status != target_status)
  {
    ERROR_LOG_FMT(IOS_NET, "WD_Disassociate: cannot disassociate in status {} (target {})",
                  m_status, target_status);
    return IPCReply(u32(ResultCode::UnavailableCommand));
  }

  // TODO: Check the input MAC address and only return 0x80008001 if it is unknown.
  return IPCReply(u32(ResultCode::IllegalParameter));
}

IPCReply NetWDCommandDevice::GetInfo(const IOCtlVRequest& request) const
{
  const auto* vector = request.GetVector(0);
  if (!vector || vector->address == 0)
    return IPCReply(u32(ResultCode::IllegalParameter));

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  memory.CopyToEmu(vector->address, &m_info, sizeof(m_info));
  return IPCReply(IPC_SUCCESS);
}

void NetWDCommandDevice::SendBridgeFrame(const IOCtlVRequest& request)
{
  if (!m_wii_ds_bridge_enabled || !m_wii_ds_bridge_connected)
    return;
  // WD_MpSendFrame vector 0 is a Nintendo MP application payload, not a raw 802.11 frame.
  // The second vector is the 0x10-byte radio/client configuration.  Official packets can be
  // very small (a Ranch keep-alive is six bytes), so do not apply an 802.11-size check.
  const IOCtlVRequest::IOVector* selected = request.GetVector(0);
  if (!selected || selected->address == 0 || selected->size == 0 || selected->size > 2048)
  {
    WriteBridgeDiagnostic("event=WD_SEND_REJECT reason=payload_vector");
    return;
  }

  // WD_SendFrame (0x1009) is not the multiplayer-config ABI. Do not reinterpret
  // its second vector or change its existing transport merely because opted in.
  const bool configured = m_wii_ds_mp_config_enabled && request.request == IOCTLV_WD_MP_SEND_FRAME;
  std::array<u8, WD::WiiDSMPConfigSize> config{};
  auto& memory = GetSystem().GetMemory();
  if (configured)
  {
    const auto* config_vector = request.GetVector(1);
    if (!config_vector || config_vector->address == 0 || config_vector->size != config.size())
    {
      WARN_LOG_FMT(IOS_NET,
                   "Wii-DS bridge: configured WD MP send rejected: vector1 must contain 16 bytes");
      WriteBridgeDiagnostic("event=WD_SEND_REJECT reason=config_vector");
      return;  // Never silently substitute guessed radio timing for a malformed request.
    }
    memory.CopyFromEmu(config.data(), config_vector->address, config.size());
  }

  std::vector<u8> application(selected->size);
  memory.CopyFromEmu(application.data(), selected->address, selected->size);
  const auto packet = WD::BuildWiiDSMPBridgePacket(application, configured, config);
  if (!packet)
    return;
  const auto status = m_wii_ds_bridge_socket.send(packet->data(), packet->size(),
                                                  sf::IpAddress::LocalHost, WiiDSBridgePort);
  if (DiagnosticSample(++m_wii_ds_diagnostic_tx_frames))
    WriteBridgeDiagnostic(
        fmt::format("event=WD_SEND n={} command={:04x} payloadBytes={} type={} status={}",
                    m_wii_ds_diagnostic_tx_frames, request.request, selected->size, (*packet)[8],
                    static_cast<int>(status)));
  if (m_wii_ds_bridge_diagnostics)
    INFO_LOG_FMT(IOS_NET, "Wii-DS bridge: forwarded WD MP payload ({} bytes, status {})",
                 selected->size, static_cast<int>(status));
  if (configured)
  {
    ++m_wii_ds_mp_config_samples;
    // TMPTT can change on each SDK deadline calculation. Sample it rather than
    // turning that into an unbounded full-config log on every command.
    const bool changed =
        !std::equal(config.begin() + 6, config.begin() + 10, m_wii_ds_mp_config_last.begin() + 6);
    m_wii_ds_mp_config_last = config;
    if (changed || m_wii_ds_mp_config_samples <= 4 || m_wii_ds_mp_config_samples % 256 == 0)
    {
      WriteBridgeDiagnostic(fmt::format(
          "event=WD_MP_CONFIG n={} childField={:04x} aidMask={:04x} tmptt={:04x} raw={}",
          m_wii_ds_mp_config_samples, (u16(config[6]) << 8) | config[7],
          (u16(config[8]) << 8) | config[9], (u16(config[10]) << 8) | config[11],
          ArrayToString(config.data(), u32(config.size()), 16)));
      if (m_wii_ds_bridge_diagnostics)
        INFO_LOG_FMT(IOS_NET,
                     "Wii-DS bridge: type7 WD MP config n={} childField=0x{:04x} "
                     "aidMask=0x{:04x} tmptt=0x{:04x} config={} payloadBytes={}",
                     m_wii_ds_mp_config_samples, (u16(config[6]) << 8) | config[7],
                     (u16(config[8]) << 8) | config[9], (u16(config[10]) << 8) | config[11],
                     ArrayToString(config.data(), u32(config.size()), 16), selected->size);
    }
  }
}

void NetWDCommandDevice::SendBridgeBeacon(const IOCtlVRequest& request)
{
  if (!m_wii_ds_bridge_enabled)
    return;
  const auto* timestamp_vector = request.GetVector(0);
  const auto* data_vector = request.GetVector(1);
  if (!timestamp_vector || !data_vector || timestamp_vector->address == 0 ||
      data_vector->address == 0 || timestamp_vector->size < 2 || data_vector->size < 128)
  {
    WARN_LOG_FMT(IOS_NET, "Wii-DS bridge: malformed WD_SendBeacon vectors");
    WriteBridgeDiagnostic("event=BEACON_REJECT reason=vectors");
    return;
  }

  constexpr std::size_t payload_size = 2 + 128;
  std::array<u8, 11 + payload_size> packet{};
  std::copy(WiiDSBridgeBinaryMagic.begin(), WiiDSBridgeBinaryMagic.end(), packet.begin());
  packet[8] = WiiDSBridgeBeaconMessage;
  packet[9] = u8(payload_size >> 8);
  packet[10] = u8(payload_size);

  auto& memory = GetSystem().GetMemory();
  memory.CopyFromEmu(packet.data() + 11, timestamp_vector->address, 2);
  memory.CopyFromEmu(packet.data() + 13, data_vector->address, 128);

  m_wii_ds_bridge_beacon.assign(packet.begin(), packet.end());
  const auto status = m_wii_ds_bridge_socket.getLocalPort() != 0 ?
                          m_wii_ds_bridge_socket.send(packet.data(), packet.size(),
                                                      sf::IpAddress::LocalHost, WiiDSBridgePort) :
                          sf::Socket::Status::NotReady;
  if (DiagnosticSample(++m_wii_ds_diagnostic_beacons))
    WriteBridgeDiagnostic(
        fmt::format("event=BEACON_CONFIG n={} gameId={:02x}{:02x}{:02x}{:02x} status={}",
                    m_wii_ds_diagnostic_beacons, packet[17], packet[18], packet[19], packet[20],
                    static_cast<int>(status)));
  if (m_wii_ds_bridge_diagnostics)
    INFO_LOG_FMT(
        IOS_NET,
        "Wii-DS bridge: forwarded Nintendo beacon (game id {:02x}{:02x}{:02x}{:02x}, status {})",
        packet[13 + 4], packet[13 + 5], packet[13 + 6], packet[13 + 7], static_cast<int>(status));
}

std::optional<IPCReply> NetWDCommandDevice::IOCtlV(const IOCtlVRequest& request)
{
  switch (request.request)
  {
  case IOCTLV_WD_INVALID:
    return IPCReply(u32(ResultCode::UnavailableCommand));
  case IOCTLV_WD_GET_MODE:
    return IPCReply(s32(m_mode));
  case IOCTLV_WD_SET_LINKSTATE:
    return SetLinkState(request);
  case IOCTLV_WD_GET_LINKSTATE:
    return GetLinkState(request);
  case IOCTLV_WD_DISASSOC:
    return Disassociate(request);

  case IOCTLV_WD_SCAN:
  {
    // Gives parameters detailing type of scan and what to match
    // XXX - unused
    // ScanInfo *scan = (ScanInfo *)memory.GetPointer(request.in_vectors.at(0).m_Address);

    auto& system = GetSystem();
    auto& memory = system.GetMemory();
    u16* results = (u16*)memory.GetPointerForRange(request.io_vectors.at(0).address,
                                                   sizeof(u16) + sizeof(BSSInfo));
    // first u16 indicates number of BSSInfo following
    results[0] = Common::swap16(1);

    BSSInfo* bss = (BSSInfo*)&results[1];
    memset(bss, 0, sizeof(BSSInfo));

    bss->length = Common::swap16(sizeof(BSSInfo));
    bss->rssi = Common::swap16(0xffff);

    for (int i = 0; i < BSSID_SIZE; ++i)
      bss->bssid[i] = i;

    const char* ssid = "dolphin-emu";
    strcpy((char*)bss->ssid, ssid);
    bss->ssid_length = Common::swap16((u16)strlen(ssid));

    bss->channel = Common::swap16(2);
  }
  break;

  case IOCTLV_WD_GET_INFO:
    return GetInfo(request);

  case IOCTLV_WD_RECV_FRAME:
    m_recv_frame_requests.emplace_back(request.address);
    return std::nullopt;

  case IOCTLV_WD_RECV_NOTIFICATION:
    m_recv_notification_requests.emplace_back(request.address);
    return std::nullopt;

  case IOCTLV_WD_CHANGE_BEACON:
    if (m_wii_ds_bridge_enabled)
    {
      SendBridgeBeacon(request);
      return IPCReply(IPC_SUCCESS);
    }
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesWDUnimplementedIOCtl);
    request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_NET,
                 Common::Log::LogLevel::LWARNING);
    break;
  case IOCTLV_WD_MP_SEND_FRAME:
  case IOCTLV_WD_SEND_FRAME:
    if (m_wii_ds_bridge_enabled)
    {
      SendBridgeFrame(request);
      return IPCReply(IPC_SUCCESS);
    }
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesWDUnimplementedIOCtl);
    request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_NET,
                 Common::Log::LogLevel::LWARNING);
    break;
  case IOCTLV_WD_SET_CONFIG:
  case IOCTLV_WD_GET_CONFIG:
  case IOCTLV_WD_CALL_WL:
  case IOCTLV_WD_MEASURE_CHANNEL:
  case IOCTLV_WD_GET_LASTERROR:
  case IOCTLV_WD_CHANGE_GAMEINFO:
  case IOCTLV_WD_CHANGE_VTSF:
  default:
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesWDUnimplementedIOCtl);
    request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_NET,
                 Common::Log::LogLevel::LWARNING);
  }

  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
