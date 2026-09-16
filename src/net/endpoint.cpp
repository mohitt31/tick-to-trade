#include "ttt/net/endpoint.hpp"

namespace ttt::net {

using itch::u16;
using itch::u8;

bool parse_endpoint(const std::string& text, Endpoint& out) {
    Endpoint    e;
    std::size_t pos = 0;
    for (int part = 0; part < 4; ++part) {
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
            return false;
        }
        unsigned v = 0;
        int      digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            v = v * 10 + static_cast<unsigned>(text[pos] - '0');
            ++pos;
            if (++digits > 3 || v > 255) {
                return false;
            }
        }
        e.ip[static_cast<std::size_t>(part)] = static_cast<u8>(v);
        const char want = part < 3 ? '.' : ':';
        if (pos >= text.size() || text[pos] != want) {
            return false;
        }
        ++pos;
    }
    unsigned port = 0;
    int      digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        port = port * 10 + static_cast<unsigned>(text[pos] - '0');
        ++pos;
        if (++digits > 5 || port > 65535) {
            return false;
        }
    }
    if (digits == 0 || pos != text.size()) {
        return false;
    }
    e.port = static_cast<u16>(port);
    out = e;
    return true;
}

std::string to_string(const Endpoint& e) {
    return std::to_string(e.ip[0]) + "." + std::to_string(e.ip[1]) + "." + std::to_string(e.ip[2]) +
           "." + std::to_string(e.ip[3]) + ":" + std::to_string(e.port);
}

}  // namespace ttt::net
