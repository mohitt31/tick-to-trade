// Stops a message source after n messages, reporting a clean end of input.
#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "itch/core/types.hpp"
#include "itch/wire/framing.hpp"
#include "ttt/replay/packet_stream.hpp"

namespace ttt::replay {

template <MessageSource Inner>
class Limited {
public:
    Limited(Inner inner, itch::u64 limit) : inner_(std::move(inner)), left_(limit) {}

    itch::wire::FrameStatus next(std::span<const std::byte>& out) {
        if (left_ == 0) {
            return itch::wire::FrameStatus::EndOfInput;
        }
        const itch::wire::FrameStatus st = inner_.next(out);
        if (st == itch::wire::FrameStatus::Ok) {
            --left_;
        }
        return st;
    }

private:
    Inner     inner_;
    itch::u64 left_;
};

}  // namespace ttt::replay
