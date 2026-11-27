// SPDX-License-Identifier: MIT
// Shared experimental transport configuration; copied into each source tree.
#pragma once
#include <array>
#include <cstdlib>
#include <string_view>

namespace WiiDSLan
{
struct Config
{
    bool valid = true;
    std::array<unsigned char, 4> peer{127, 0, 0, 1};
    bool IsLocal() const { return peer == std::array<unsigned char, 4>{127, 0, 0, 1}; }
};

inline Config Parse(const char* value)
{
    Config result;
    if (!value || !*value) return result;
    const std::string_view text(value);
    size_t position = 0;
    for (auto& octet : result.peer)
    {
        unsigned number = 0;
        const size_t start = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9')
        {
            number = number * 10 + unsigned(text[position++] - '0');
            if (position - start > 3 || number > 255) { result.valid = false; return result; }
        }
        if (position == start || (position - start > 1 && text[start] == '0'))
        { result.valid = false; return result; }
        octet = static_cast<unsigned char>(number);
        if (&octet != &result.peer.back())
        {
            if (position == text.size() || text[position++] != '.')
            { result.valid = false; return result; }
        }
    }
    // Only unicast destinations. Keep the historical loopback address available.
    result.valid = position == text.size() && result.peer[0] != 0 &&
                   result.peer[0] < 224 && result.peer[3] != 255 &&
                   (result.peer[0] != 127 || result.IsLocal());
    return result;
}

inline Config FromEnvironment() { return Parse(std::getenv("WII_DS_PEER_IPV4")); }
}
