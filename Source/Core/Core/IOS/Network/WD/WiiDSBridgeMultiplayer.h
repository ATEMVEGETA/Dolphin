// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/Network/WD/WiiDSBridgeDisconnect.h"

namespace IOS::HLE::WD
{
using WiiDSBridgeClock = std::chrono::steady_clock;
constexpr unsigned short WiiDSBridgeFirstPort = 7065;
constexpr unsigned short WiiDSBridgeLastPort = 7080;
constexpr std::size_t WiiDSBridgeMaximumPeers = 16;
constexpr u16 WiiDSBridgeMaximumAID = 15;
constexpr std::size_t WiiDSBridgeMaximumRounds = 128;
// This is a host-transport watchdog, not emulated radio timing. Guest reply
// slots still use the unmodified WD MP configuration in melonDS. A slow host
// must not turn another player's absent reply into a successful MP ACK.
constexpr auto WiiDSBridgeRoundTimeout = std::chrono::milliseconds(1000);

struct WiiDSBridgePeer
{
  unsigned short port = 0;
  std::string identity;
  bool multiplayer = false;
  u64 generation = 0;
  WiiDSBridgeClock::time_point last_ack{};
  u8 channel = 1;
  WiiDSBridgeClient client;
};

class WiiDSBridgePeerTable
{
public:
  enum class RegisterResult
  {
    Added,
    Unchanged,
    Replaced,
    LegacyConflict,
    Full,
  };

  struct Registration
  {
    RegisterResult result;
    WiiDSBridgePeer* peer;
    std::optional<WiiDSBridgeClient> previous_client;
  };

  WiiDSBridgePeer* Find(unsigned short port)
  {
    const auto it = std::find_if(m_peers.begin(), m_peers.end(),
                                 [port](const auto& peer) { return peer.port == port; });
    return it == m_peers.end() ? nullptr : &*it;
  }

  const WiiDSBridgePeer* Find(unsigned short port) const
  {
    return const_cast<WiiDSBridgePeerTable*>(this)->Find(port);
  }

  Registration Register(unsigned short port, std::string identity, bool multiplayer,
                        WiiDSBridgeClock::time_point now)
  {
    if (port < WiiDSBridgeFirstPort || port > WiiDSBridgeLastPort)
      return {RegisterResult::Full, nullptr, {}};
    auto* peer = Find(port);
    if (peer && peer->identity == identity && peer->multiplayer == multiplayer)
    {
      peer->last_ack = now;
      return {RegisterResult::Unchanged, peer, {}};
    }
    // Legacy radios have no round identity and only know AID 1. They may be
    // used alone; mixing one into an M2 session would misattribute replies.
    for (const auto& other : m_peers)
    {
      if (other.port != 0 && other.port != port && (!multiplayer || !other.multiplayer))
        return {RegisterResult::LegacyConflict, nullptr, {}};
    }
    const bool replaced = peer != nullptr;
    std::optional<WiiDSBridgeClient> old_client;
    if (peer && peer->client.associated)
      old_client = peer->client;
    if (!peer)
    {
      const auto it = std::find_if(m_peers.begin(), m_peers.end(),
                                   [](const auto& entry) { return entry.port == 0; });
      if (it == m_peers.end())
        return {RegisterResult::Full, nullptr, {}};
      peer = &*it;
    }
    *peer = {};
    peer->port = port;
    peer->identity = std::move(identity);
    peer->multiplayer = multiplayer;
    peer->generation = ++m_generation;
    peer->last_ack = now;
    return {replaced ? RegisterResult::Replaced : RegisterResult::Added, peer, old_client};
  }

  struct Association
  {
    u16 aid = 0;
    bool joined = false;
  };

  Association Associate(WiiDSBridgePeer& peer, const std::array<u8, 6>& mac)
  {
    if ((mac[0] & 1) != 0 || std::all_of(mac.begin(), mac.end(), [](u8 byte) { return byte == 0; }))
      return {};
    if (peer.client.associated)
      return peer.client.mac == mac ? Association{peer.client.aid, false} : Association{};
    for (const auto& other : m_peers)
    {
      if (other.port != 0 && &other != &peer && other.client.associated && other.client.mac == mac)
        return {};
    }
    for (u16 aid = 1; aid <= WiiDSBridgeMaximumAID; ++aid)
    {
      const bool used = std::any_of(m_peers.begin(), m_peers.end(), [aid](const auto& other) {
        return other.port != 0 && other.client.associated && other.client.aid == aid;
      });
      if (!used)
      {
        peer.client = {mac, true, aid};
        // A new association is a new radio session even on the same socket.
        // Late replies from a previous use of this AID must not satisfy it.
        peer.generation = ++m_generation;
        return {aid, true};
      }
    }
    return {};
  }

  void Remove(unsigned short port)
  {
    if (auto* peer = Find(port))
      *peer = {};
  }

  void Clear()
  {
    m_peers = {};
    // Keep the generation counter monotonic across session resets.
  }

  bool Empty() const
  {
    return std::none_of(m_peers.begin(), m_peers.end(),
                         [](const auto& peer) { return peer.port != 0; });
  }

  auto& Peers() { return m_peers; }
  const auto& Peers() const { return m_peers; }

private:
  std::array<WiiDSBridgePeer, WiiDSBridgeMaximumPeers> m_peers{};
  u64 m_generation = 0;
};

struct WiiDSBridgeReply
{
  u16 aid = 0;
  std::vector<u8> payload;
};

// Revolution SDK ParseMPEndIndication walks count fixed-stride records.
// Bytes not established by the original one-client implementation stay zero.
inline std::optional<std::vector<u8>>
BuildWiiDSMPReceiveEnvelope(const std::vector<WiiDSBridgeReply>& replies)
{
  if (replies.size() > WiiDSBridgeMaximumAID)
    return std::nullopt;
  std::size_t stride = 0;
  u16 seen = 0;
  for (const auto& reply : replies)
  {
    if (reply.aid == 0 || reply.aid > WiiDSBridgeMaximumAID || reply.payload.size() > 2048 ||
        (seen & (1u << reply.aid)) != 0)
      return std::nullopt;
    seen |= 1u << reply.aid;
    stride = std::max(stride, 8 + reply.payload.size());
  }
  std::vector<u8> result(10 + stride * replies.size(), 0);
  result[4] = u8(replies.size() >> 8);
  result[5] = u8(replies.size());
  result[6] = u8(stride >> 8);
  result[7] = u8(stride);
  std::size_t offset = 10;
  for (const auto& reply : replies)
  {
    result[offset] = u8(reply.payload.size() >> 8);
    result[offset + 1] = u8(reply.payload.size());
    result[offset + 3] = 0x40;  // stable virtual radio RSSI
    result[offset + 4] = u8(reply.aid >> 8);
    result[offset + 5] = u8(reply.aid);
    std::copy(reply.payload.begin(), reply.payload.end(), result.begin() + offset + 8);
    offset += stride;
  }
  return result;
}

struct WiiDSBridgeRoundRecipient
{
  unsigned short port = 0;
  u64 generation = 0;
  std::array<u8, 6> mac{};
};

struct WiiDSBridgeRound
{
  u32 id = 0;
  u16 target_mask = 0;
  u16 received_mask = 0;
  u16 failed_mask = 0;
  WiiDSBridgeClock::time_point started{};
  std::array<WiiDSBridgeRoundRecipient, 16> recipients{};
  std::array<std::optional<WiiDSBridgeReply>, 16> replies{};

  u16 FailureMask() const { return target_mask & ~received_mask; }

  std::vector<WiiDSBridgeReply> Replies() const
  {
    std::vector<WiiDSBridgeReply> result;
    for (u16 aid = 1; aid <= WiiDSBridgeMaximumAID; ++aid)
    {
      if (replies[aid])
        result.push_back(*replies[aid]);
    }
    return result;
  }
};

class WiiDSBridgeRounds
{
public:
  std::optional<u32> Begin(u16 target_mask, const WiiDSBridgePeerTable& peers,
                           WiiDSBridgeClock::time_point now)
  {
    if (m_rounds.size() >= WiiDSBridgeMaximumRounds)
      return std::nullopt;
    WiiDSBridgeRound round;
    round.id = ++m_last_id;
    if (round.id == 0)
      round.id = ++m_last_id;
    round.target_mask = target_mask & 0xfffe;
    round.started = now;
    for (const auto& peer : peers.Peers())
    {
      if (peer.port != 0 && peer.multiplayer && peer.client.associated)
        round.recipients[peer.client.aid] = {peer.port, peer.generation, peer.client.mac};
    }
    m_rounds.emplace_back(std::move(round));
    return m_rounds.back().id;
  }

  bool Receive(u32 id, const WiiDSBridgePeer& peer, std::vector<u8> payload)
  {
    if (!peer.client.associated || peer.client.aid == 0 || peer.client.aid > 15 ||
        payload.size() > 2048)
      return false;
    const auto it = std::find_if(m_rounds.begin(), m_rounds.end(),
                                 [id](const auto& round) { return round.id == id; });
    if (it == m_rounds.end())
      return false;
    const u16 aid = peer.client.aid;
    const u16 bit = 1u << aid;
    const auto& recipient = it->recipients[aid];
    if ((it->target_mask & bit) == 0 || (it->received_mask & bit) != 0 ||
        (it->failed_mask & bit) != 0 || recipient.port != peer.port ||
        recipient.generation != peer.generation || recipient.mac != peer.client.mac)
      return false;
    it->replies[aid] = WiiDSBridgeReply{aid, std::move(payload)};
    it->received_mask |= bit;
    return true;
  }

  void RemovePeer(unsigned short port, u64 generation)
  {
    for (auto& round : m_rounds)
    {
      for (u16 aid = 1; aid <= WiiDSBridgeMaximumAID; ++aid)
      {
        const auto& recipient = round.recipients[aid];
        if (recipient.port == port && recipient.generation == generation)
          round.failed_mask |= (1u << aid) & ~round.received_mask;
      }
    }
  }

  std::optional<WiiDSBridgeRound> TakeCompleted(WiiDSBridgeClock::time_point now)
  {
    if (m_rounds.empty())
      return std::nullopt;
    const auto& first = m_rounds.front();
    if (((first.received_mask | first.failed_mask) & first.target_mask) != first.target_mask &&
        now - first.started < WiiDSBridgeRoundTimeout)
      return std::nullopt;
    auto result = std::move(m_rounds.front());
    m_rounds.pop_front();
    return result;
  }

  void Clear() { m_rounds.clear(); }
  std::size_t Size() const { return m_rounds.size(); }

private:
  std::deque<WiiDSBridgeRound> m_rounds;
  u32 m_last_id = 0;
};
}  // namespace IOS::HLE::WD
