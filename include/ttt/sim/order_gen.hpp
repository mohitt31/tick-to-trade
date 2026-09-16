// Generates valid ITCH order flow for the chaos harness.
//
// Valid means a book builder can apply every message: executions, cancels,
// deletes and replaces only ever name orders that are resting, and never for
// more shares than remain. The generator keeps its own model of the live orders
// to guarantee that. Several symbols are interleaved, because the receiver
// tracks one and has to ignore the rest by locate code, and the snapshot has to
// carry the directory entry that makes that possible.
//
// Timestamps come in bursts and silences, so feed-time pacing produces packets
// of very different sizes and heartbeats get exercised.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "itch/core/types.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/itch/encode.hpp"

namespace ttt::sim {

using itch::u16;
using itch::u32;
using itch::u64;

struct GenConfig {
    u32 symbols = 3;          // SYM1..SYMn, locate codes 1..n
    u32 messages = 2000;      // order messages after the directory
    u32 max_live = 300;       // per symbol
    u64 max_gap_ns = 20'000;  // between consecutive messages inside a burst
    u32 silence_ppm = 2'000;  // chance a gap is a long silence instead
    u64 silence_ns = 30'000'000;
};

[[nodiscard]] inline std::string symbol_name(u32 i) { return "SYM" + std::to_string(i + 1); }

[[nodiscard]] inline std::vector<itch_out::Message> generate(Rng& rng, const GenConfig& cfg) {
    struct Live {
        itch::OrderRef ref;
        char           side;
        itch::Qty      qty;
    };
    std::vector<std::vector<Live>> live(cfg.symbols);
    std::vector<itch_out::Message> out;
    out.reserve(cfg.messages + cfg.symbols + 1);

    u64  ts = 4ULL * 3600 * 1'000'000'000ULL;
    u64  next_ref = 1;
    u64  match = 1;
    auto common = [&](u32 sym) {
        return itch_out::Common{static_cast<u16>(sym + 1), static_cast<u16>(rng.range(0, 7)), ts};
    };

    out.push_back(itch_out::system_event({0, 0, ts}, 'O'));
    for (u32 s = 0; s < cfg.symbols; ++s) {
        out.push_back(itch_out::stock_directory(common(s), symbol_name(s)));
    }

    // Bids sit below a fixed mid and asks above it, so the book never crosses.
    const itch::Price mid = 1'000'000;  // $100.00
    auto              price_for = [&](char side) {
        const itch::Price off = static_cast<itch::Price>(rng.range(1, 40) * 100);
        return side == 'B' ? mid - off : mid + off;
    };

    for (u32 i = 0; i < cfg.messages; ++i) {
        ts += rng.chance_ppm(cfg.silence_ppm) ? cfg.silence_ns : rng.range(0, cfg.max_gap_ns);
        const u32 sym = static_cast<u32>(rng.range(0, cfg.symbols - 1));
        auto&     book = live[sym];
        const u64 roll = rng.range(0, 99);

        if (book.empty() || (roll < 40 && book.size() < cfg.max_live)) {
            const char      side = rng.range(0, 1) == 0 ? 'B' : 'S';
            const itch::Qty qty = static_cast<itch::Qty>(rng.range(1, 50) * 100);
            const u64       ref = next_ref++;
            out.push_back(itch_out::add_order(common(sym), ref, side, qty, symbol_name(sym),
                                              price_for(side)));
            book.push_back({ref, side, qty});
            continue;
        }

        const std::size_t k = static_cast<std::size_t>(rng.range(0, book.size() - 1));
        Live&             o = book[k];
        auto              remove = [&] {
            book[k] = book.back();
            book.pop_back();
        };

        if (roll < 55) {
            const itch::Qty q = static_cast<itch::Qty>(rng.range(1, o.qty));
            out.push_back(itch_out::order_executed(common(sym), o.ref, q, match++));
            o.qty -= q;
            if (o.qty == 0) {
                remove();
            }
        } else if (roll < 70 && o.qty > 1) {
            const itch::Qty q = static_cast<itch::Qty>(rng.range(1, o.qty - 1));
            out.push_back(itch_out::order_cancel(common(sym), o.ref, q));
            o.qty -= q;
        } else if (roll < 90) {
            out.push_back(itch_out::order_delete(common(sym), o.ref));
            remove();
        } else {
            const u64       ref = next_ref++;
            const itch::Qty qty = static_cast<itch::Qty>(rng.range(1, 50) * 100);
            out.push_back(itch_out::order_replace(common(sym), o.ref, ref, qty, price_for(o.side)));
            o.ref = ref;
            o.qty = qty;
        }
    }
    return out;
}

}  // namespace ttt::sim
