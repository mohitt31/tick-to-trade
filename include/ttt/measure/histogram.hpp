// A latency histogram: HdrHistogram_c underneath, nanoseconds in.
//
// Values are recorded exactly as given. There is no coordinated-omission
// correction here on purpose: this project avoids the problem at the source, by
// measuring against each packet's intended send time on an absolute schedule,
// and correcting after the fact would double count.
#pragma once

#include <cstdio>
#include <memory>
#include <string>

#include "itch/core/types.hpp"

struct hdr_histogram;

namespace ttt::measure {

using itch::i64;
using itch::u64;

class Histogram {
public:
    // Tracks 1 ns .. highest_ns at the given number of significant figures.
    explicit Histogram(i64 highest_ns = 10'000'000'000, int significant_figures = 3);
    ~Histogram();
    Histogram(Histogram&&) noexcept;
    Histogram& operator=(Histogram&&) noexcept;
    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;

    // Values below 1 are recorded as 1 and above the highest trackable value as
    // the highest; both are counted, because a clamped value is not a
    // measurement and a report has to say how many there were.
    void record(i64 ns) noexcept;
    void merge(const Histogram& other);
    void reset() noexcept;

    [[nodiscard]] u64 count() const noexcept;
    [[nodiscard]] i64 min() const noexcept;
    [[nodiscard]] i64 max() const noexcept;
    [[nodiscard]] i64 at(double percentile) const noexcept;
    [[nodiscard]] u64 clamped_low() const noexcept { return clamped_low_; }
    [[nodiscard]] u64 clamped_high() const noexcept { return clamped_high_; }

    // "count N  min .. p50 .. p99 .. p99.9 .. p99.99 .. max" in nanoseconds.
    [[nodiscard]] std::string summary() const;

    // The percentile distribution in HdrHistogram's .hgrm text format, values in
    // microseconds, readable by the standard plotting tools.
    void write_hgrm(std::FILE* out) const;

private:
    hdr_histogram* h_ = nullptr;
    u64            clamped_low_ = 0;
    u64            clamped_high_ = 0;
    i64            highest_ = 0;
};

}  // namespace ttt::measure
