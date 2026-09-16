// Seeded generator for everything random in this project: fault injection,
// generated order streams, property tests.
//
// splitmix64, the same generator Project 1's tests use. It is small and well
// distributed, and it gives the same sequence on every platform and compiler,
// which is the property that matters: a chaos run that fails on the Linux box
// has to replay from its seed on the Mac.
#pragma once

#include <cstdint>

namespace ttt {

// GCC's -Wpedantic rejects __int128 unless it is marked as an extension.
__extension__ using u128 = unsigned __int128;

class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : s_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform in [lo, hi], by multiply-shift. The bias is far below anything a
    // test can observe, and unlike rejection sampling it consumes exactly one
    // draw, so how many draws a run makes does not depend on the values drawn.
    constexpr std::uint64_t range(std::uint64_t lo, std::uint64_t hi) noexcept {
        const std::uint64_t span = hi - lo + 1;
        if (span == 0) {  // the full 64-bit range
            return next();
        }
        return lo + static_cast<std::uint64_t>((static_cast<u128>(next()) * span) >> 64);
    }

    // True with probability ppm / 1,000,000. Parts per million rather than a
    // percentage because the interesting drop rates are well under one percent.
    constexpr bool chance_ppm(std::uint32_t ppm) noexcept { return range(0, 999'999) < ppm; }

    // An independent generator for a sub-component, so that adding draws to one
    // fault stream does not shift every other stream's sequence.
    constexpr Rng split() noexcept { return Rng(next()); }

private:
    std::uint64_t s_;
};

}  // namespace ttt
