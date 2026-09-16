// The two things a sender offers besides the live feed: retransmission of recent
// messages, and a snapshot of the book.
//
// Both are fed every message the sender publishes, in sequence order, and both
// are pure: a request goes in, bytes come out. The chaos harness calls them in
// simulated time; on Linux they sit behind a UDP socket and a TCP listener.
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/views.hpp"
#include "ttt/core/mutant.hpp"
#include "ttt/feed/book_sink.hpp"
#include "ttt/feed/seq_slots.hpp"
#include "ttt/feed/snapshot_wire.hpp"
#include "ttt/itch/encode.hpp"
#include "ttt/mold/mold.hpp"
#include "ttt/mold/packer.hpp"

namespace ttt::replay {

using itch::u16;
using itch::u32;
using itch::u64;

// Answers MoldUDP64 retransmission requests from the last `capacity` messages.
//
// A request is answered only if every message it names is still held. A partial
// answer would look like progress to the receiver while leaving the hole where
// it was. Anything older than the table is simply not answered, which is also
// what a real rewind server does, and the receiver's retry and snapshot
// escalation is what copes with it.
class RewindServer {
public:
    RewindServer(const mold::Session& session, u32 capacity, std::size_t budget)
        : session_(session), slots_(capacity), budget_(budget) {}

    void publish(u64 seq, std::span<const std::byte> msg) {
        ITCH_ASSERT_MSG(seq == next_, "messages must be published in sequence order");
        slots_.put(seq, msg);
        ++next_;
    }

    // Calls emit(packet) for each response packet. Returns false, emitting
    // nothing, if the request cannot be answered in full.
    template <class Emit>
    bool serve(std::span<const std::byte> request, Emit&& emit) {
        mold::Header h;
        if (!mold::decode_request(request, h) || h.session != session_) {
            ++rejected_;
            return false;
        }
        const u64 end = h.sequence + h.count;
        if (end > next_ || !slots_.holds_all(h.sequence, end)) {
            ++unavailable_;
            return false;
        }
        mold::Packer p(session_, h.sequence, budget_);
        for (u64 q = h.sequence; q < end; ++q) {
            const auto m = slots_.find(q);
            if (!p.fits(m.size())) {
                emit(p.finish());
            }
            p.add(m);
        }
        emit(p.finish());
        ++served_;
        return true;
    }

    [[nodiscard]] u64 next_sequence() const noexcept { return next_; }
    [[nodiscard]] u64 served() const noexcept { return served_; }
    [[nodiscard]] u64 unavailable() const noexcept { return unavailable_; }
    [[nodiscard]] u64 rejected() const noexcept { return rejected_; }

private:
    mold::Session  session_;
    feed::SeqSlots slots_;
    std::size_t    budget_;
    u64            next_ = 1;
    u64            served_ = 0;
    u64            unavailable_ = 0;
    u64            rejected_ = 0;
};

// Keeps a book for one symbol and describes it as a snapshot on request.
class SnapshotServer {
public:
    SnapshotServer(const mold::Session& session, std::string symbol, feed::BookCapacity cap)
        : session_(session), book_(std::move(symbol), cap) {}

    void publish(u64 seq, std::span<const std::byte> msg) {
        ITCH_ASSERT_MSG(seq == last_ + 1, "messages must be published in sequence order");
        book_.apply(msg);
        if (!have_directory_ && !msg.empty() && static_cast<char>(msg[0]) == 'R') {
            const itch::wire::StockDirectoryView v(msg.data());
            if (itch::wire::trim_alpha(v.stock()) == book_.symbol()) {
                directory_ = std::vector<std::byte>(msg.begin(), msg.end());
                have_directory_ = true;
            }
        }
        last_ = seq;
    }

    // The book as of the last published message.
    void build(std::vector<std::byte>& out) const {
        u64 label = last_;
        if (TTT_MUTANT(SnapshotLabelOffByOne) && label > 0) {
            --label;
        }
        feed::SnapshotWriter w(out, session_, label);
        if (!have_directory_) {
            return;  // nothing about this symbol has been published yet
        }
        w.add(directory_);

        const auto&                 book = book_.book();
        const u16                   locate = book_.builder().locate();
        std::vector<itch::OrderRef> refs;
        for (itch::Side side : {itch::Side::Buy, itch::Side::Sell}) {
            book.for_each_level(side, [&](itch::Price price, u64, u32, const auto& queue) {
                refs.clear();
                for (itch::OrderRef r : queue) {
                    refs.push_back(r);
                }
                if (TTT_MUTANT(SnapshotReversedQueue)) {
                    std::reverse(refs.begin(), refs.end());
                }
                for (itch::OrderRef r : refs) {
                    const auto m = itch_out::add_order(itch_out::Common{locate, 0, 0}, r,
                                                       side == itch::Side::Buy ? 'B' : 'S',
                                                       book.qty_of(r), book_.symbol(), price);
                    w.add(m.bytes());
                }
            });
        }
    }

    [[nodiscard]] u64                   last_sequence() const noexcept { return last_; }
    [[nodiscard]] const feed::BookSink& book() const noexcept { return book_; }

private:
    mold::Session          session_;
    feed::BookSink         book_;
    std::vector<std::byte> directory_;
    bool                   have_directory_ = false;
    u64                    last_ = 0;
};

}  // namespace ttt::replay
