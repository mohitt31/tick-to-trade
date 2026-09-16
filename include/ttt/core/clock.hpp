// Monotonic time, and waiting for a deadline without sleeping through it.
//
// Everything here is a monotonic clock in nanoseconds: CLOCK_MONOTONIC on Linux,
// CLOCK_MONOTONIC_RAW on macOS. macOS's CLOCK_MONOTONIC only moves in whole
// microseconds (measured: every step was 1000 ns), which would turn every
// sub-microsecond latency into rounding; its _RAW clock steps at 41 ns. It is the clock both the
// sender and a receiver on the same host can read, which is what lets a
// same-host latency be computed at all. The TSC-based clock for the Linux
// measurements is a separate piece and replaces this only where it has been
// shown to agree with it.
#pragma once

#include <ctime>

#include "itch/core/types.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace ttt {

#ifdef __APPLE__
inline constexpr clockid_t kClock = CLOCK_MONOTONIC_RAW;
#else
inline constexpr clockid_t kClock = CLOCK_MONOTONIC;
#endif

[[nodiscard]] inline itch::u64 now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(kClock, &ts);
    return static_cast<itch::u64>(ts.tv_sec) * 1'000'000'000u + static_cast<itch::u64>(ts.tv_nsec);
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

// Sleeps until spin_ns before the deadline, then spins. A sleep alone wakes up
// whenever the scheduler gets round to it, which on a loaded machine is
// milliseconds late; a spin alone burns a core for the whole wait.
inline void wait_until(itch::u64 deadline_ns, itch::u64 spin_ns = 200'000) noexcept {
    itch::u64 now = now_ns();
    if (deadline_ns > now + spin_ns) {
        const itch::u64 sleep = deadline_ns - now - spin_ns;
        timespec        ts{static_cast<time_t>(sleep / 1'000'000'000u),
                           static_cast<long>(sleep % 1'000'000'000u)};
        ::nanosleep(&ts, nullptr);
    }
    while (now_ns() < deadline_ns) {
        cpu_relax();
    }
}

}  // namespace ttt
