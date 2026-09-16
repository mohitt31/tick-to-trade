// Builds framed ITCH input for tests: the BinaryFILE format Project 1's
// FrameCursor reads, which is what the real day file looks like once inflated.
#pragma once

#include <cstddef>
#include <vector>

#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/itch/encode.hpp"

namespace ttt::test {

inline void append_framed(std::vector<std::byte>& out, const itch_out::Message& m) {
    const auto  b = m.bytes();
    std::size_t at = out.size();
    out.resize(at + 2 + b.size());
    itch::store_be<itch::u16>(out.data() + at, static_cast<itch::u16>(b.size()));
    for (std::size_t i = 0; i < b.size(); ++i) {
        out[at + 2 + i] = b[i];
    }
}

// Messages of mixed types and lengths with non-decreasing timestamps. Gaps
// between timestamps are drawn from max_gap_ns, and occasionally a long silence
// is inserted so heartbeats have something to fill.
inline std::vector<itch_out::Message> mixed_messages(Rng& rng, std::size_t n, itch::u64 max_gap_ns,
                                                     itch::u64 silence_ns) {
    std::vector<itch_out::Message> out;
    out.reserve(n);
    itch::u64 ts = 34'200'000'000'000ULL;  // 09:30
    for (std::size_t i = 0; i < n; ++i) {
        ts += rng.range(0, max_gap_ns);
        if (silence_ns != 0 && rng.chance_ppm(5'000)) {
            ts += silence_ns;
        }
        const itch_out::Common c{static_cast<itch::u16>(rng.range(1, 9)),
                                 static_cast<itch::u16>(rng.range(0, 3)), ts};
        const itch::u64        ref = rng.range(1, 1'000'000);
        switch (rng.range(0, 5)) {
            case 0: out.push_back(itch_out::add_order(c, ref, 'B', 100, "AAPL", 1'500'000)); break;
            case 1: out.push_back(itch_out::order_executed(c, ref, 10, i)); break;
            case 2: out.push_back(itch_out::order_cancel(c, ref, 5)); break;
            case 3: out.push_back(itch_out::order_delete(c, ref)); break;
            case 4: out.push_back(itch_out::order_replace(c, ref, ref + 1, 50, 1'500'100)); break;
            default: out.push_back(itch_out::system_event(c, 'Q')); break;
        }
    }
    return out;
}

inline std::vector<std::byte> framed(const std::vector<itch_out::Message>& msgs) {
    std::vector<std::byte> out;
    for (const auto& m : msgs) {
        append_framed(out, m);
    }
    return out;
}

}  // namespace ttt::test
