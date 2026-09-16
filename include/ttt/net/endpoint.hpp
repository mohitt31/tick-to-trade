// An IPv4 address and port.
#pragma once

#include <array>
#include <compare>
#include <string>

#include "itch/core/types.hpp"

namespace ttt::net {

struct Endpoint {
    std::array<itch::u8, 4> ip{};
    itch::u16               port = 0;

    [[nodiscard]] bool multicast() const noexcept { return ip[0] >= 224 && ip[0] <= 239; }

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
    friend auto operator<=>(const Endpoint&, const Endpoint&) = default;
};

// Parses "a.b.c.d:port". Returns false on anything else.
[[nodiscard]] bool        parse_endpoint(const std::string& text, Endpoint& out);
[[nodiscard]] std::string to_string(const Endpoint& e);

}  // namespace ttt::net
