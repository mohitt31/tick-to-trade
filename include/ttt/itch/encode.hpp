// Writes ITCH 5.0 messages.
//
// Project 1 only ever reads ITCH. This project also has to write it, in two
// places: the snapshot server describes a book as a series of Add Order
// messages, and the chaos harness generates order streams. Every field goes in
// at offsetof() on Project 1's layout structs, the mirror image of how its views
// read them, so the reader and the writer share a single source of truth for
// every offset.
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/messages.hpp"

namespace ttt::itch_out {

using itch::OrderRef;
using itch::Price;
using itch::Qty;
using itch::Timestamp;
using itch::u16;
using itch::u64;
using itch::u8;

// One encoded message, held by value. ITCH messages are at most 50 bytes, so
// this never allocates.
class Message {
public:
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>(buf_.data(), len_);
    }

    [[nodiscard]] char type() const noexcept { return static_cast<char>(buf_[0]); }

private:
    friend class Writer;

    std::array<std::byte, itch::wire::kMaxMessageLength> buf_{};
    u8                                                   len_ = 0;
};

// Fields shared by every message type.
struct Common {
    u16       locate = 0;
    u16       tracking = 0;
    Timestamp timestamp = 0;
};

class Writer {
public:
    Writer(Message& m, char type, const Common& c) noexcept : m_(m) {
        const std::size_t len = itch::wire::message_length(type);
        ITCH_ASSERT_MSG(len != 0, "unknown ITCH message type");
        m_.buf_.fill(std::byte{0});
        m_.len_ = static_cast<u8>(len);
        m_.buf_[0] = static_cast<std::byte>(type);
        // Every ITCH 5.0 message starts with the same four fields at the same
        // offsets, so AddOrder's offsets serve for all of them.
        be<u16>(offsetof(itch::wire::AddOrder, stock_locate), c.locate);
        be<u16>(offsetof(itch::wire::AddOrder, tracking_number), c.tracking);
        itch::store_be48(m_.buf_.data() + offsetof(itch::wire::AddOrder, timestamp),
                         c.timestamp & 0xFFFF'FFFF'FFFFULL);
    }

    template <class T>
    void be(std::size_t offset, T v) noexcept {
        ITCH_ASSERT(offset + sizeof(T) <= m_.len_);
        itch::store_be<T>(m_.buf_.data() + offset, v);
    }

    void ch(std::size_t offset, char c) noexcept {
        ITCH_ASSERT(offset < m_.len_);
        m_.buf_[offset] = static_cast<std::byte>(c);
    }

    void alpha(std::size_t offset, std::size_t width, std::string_view s) noexcept {
        ITCH_ASSERT(offset + width <= m_.len_);
        ITCH_ASSERT_MSG(s.size() <= width, "alpha field too long");
        for (std::size_t i = 0; i < width; ++i) {
            m_.buf_[offset + i] = static_cast<std::byte>(i < s.size() ? s[i] : ' ');
        }
    }

private:
    Message& m_;
};

[[nodiscard]] inline Message system_event(const Common& c, char event_code) noexcept {
    using L = itch::wire::SystemEvent;
    Message m;
    Writer  w(m, 'S', c);
    w.ch(offsetof(L, event_code), event_code);
    return m;
}

// A stock directory entry with the fields a book builder reads; the rest are
// left as plausible defaults.
[[nodiscard]] inline Message stock_directory(const Common& c, std::string_view stock) noexcept {
    using L = itch::wire::StockDirectory;
    Message m;
    Writer  w(m, 'R', c);
    w.alpha(offsetof(L, stock), sizeof(L::stock), stock);
    w.ch(offsetof(L, market_category), 'Q');
    w.ch(offsetof(L, financial_status_indicator), 'N');
    w.be<itch::u32>(offsetof(L, round_lot_size), 100);
    w.ch(offsetof(L, round_lots_only), 'N');
    return m;
}

[[nodiscard]] inline Message add_order(const Common& c, OrderRef ref, char side, Qty shares,
                                       std::string_view stock, Price price) noexcept {
    using L = itch::wire::AddOrder;
    ITCH_ASSERT_MSG(side == 'B' || side == 'S', "side must be B or S");
    Message m;
    Writer  w(m, 'A', c);
    w.be<u64>(offsetof(L, order_reference_number), ref);
    w.ch(offsetof(L, buy_sell_indicator), side);
    w.be<itch::u32>(offsetof(L, shares), shares);
    w.alpha(offsetof(L, stock), sizeof(L::stock), stock);
    w.be<itch::u32>(offsetof(L, price), price);
    return m;
}

[[nodiscard]] inline Message order_executed(const Common& c, OrderRef ref, Qty shares,
                                            u64 match) noexcept {
    using L = itch::wire::OrderExecuted;
    Message m;
    Writer  w(m, 'E', c);
    w.be<u64>(offsetof(L, order_reference_number), ref);
    w.be<itch::u32>(offsetof(L, executed_shares), shares);
    w.be<u64>(offsetof(L, match_number), match);
    return m;
}

[[nodiscard]] inline Message order_cancel(const Common& c, OrderRef ref, Qty shares) noexcept {
    using L = itch::wire::OrderCancel;
    Message m;
    Writer  w(m, 'X', c);
    w.be<u64>(offsetof(L, order_reference_number), ref);
    w.be<itch::u32>(offsetof(L, cancelled_shares), shares);
    return m;
}

[[nodiscard]] inline Message order_delete(const Common& c, OrderRef ref) noexcept {
    using L = itch::wire::OrderDelete;
    Message m;
    Writer  w(m, 'D', c);
    w.be<u64>(offsetof(L, order_reference_number), ref);
    return m;
}

[[nodiscard]] inline Message order_replace(const Common& c, OrderRef original, OrderRef replacement,
                                           Qty shares, Price price) noexcept {
    using L = itch::wire::OrderReplace;
    Message m;
    Writer  w(m, 'U', c);
    w.be<u64>(offsetof(L, original_order_reference_number), original);
    w.be<u64>(offsetof(L, new_order_reference_number), replacement);
    w.be<itch::u32>(offsetof(L, shares), shares);
    w.be<itch::u32>(offsetof(L, price), price);
    return m;
}

}  // namespace ttt::itch_out
