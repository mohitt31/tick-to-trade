#include "ttt/measure/histogram.hpp"

#include <hdr/hdr_histogram.h>

#include <cinttypes>
#include <new>
#include <stdexcept>
#include <utility>

namespace ttt::measure {

Histogram::Histogram(i64 highest_ns, int significant_figures) : highest_(highest_ns) {
    if (hdr_init(1, highest_ns, significant_figures, &h_) != 0) {
        throw std::bad_alloc();
    }
}

Histogram::~Histogram() {
    if (h_ != nullptr) {
        hdr_close(h_);
    }
}

Histogram::Histogram(Histogram&& o) noexcept
    : h_(std::exchange(o.h_, nullptr)),
      clamped_low_(o.clamped_low_),
      clamped_high_(o.clamped_high_),
      highest_(o.highest_) {}

Histogram& Histogram::operator=(Histogram&& o) noexcept {
    if (this != &o) {
        if (h_ != nullptr) {
            hdr_close(h_);
        }
        h_ = std::exchange(o.h_, nullptr);
        clamped_low_ = o.clamped_low_;
        clamped_high_ = o.clamped_high_;
        highest_ = o.highest_;
    }
    return *this;
}

void Histogram::record(i64 ns) noexcept {
    if (ns < 0) {
        ++clamped_low_;
        ns = 0;
    } else if (ns > highest_) {
        ++clamped_high_;
        ns = highest_;
    }
    hdr_record_value(h_, ns);
}

void Histogram::merge(const Histogram& other) {
    if (hdr_add(h_, other.h_) != 0) {
        throw std::runtime_error("histogram merge dropped values: ranges differ");
    }
    clamped_low_ += other.clamped_low_;
    clamped_high_ += other.clamped_high_;
}

void Histogram::reset() noexcept {
    hdr_reset(h_);
    clamped_low_ = 0;
    clamped_high_ = 0;
}

u64 Histogram::count() const noexcept { return static_cast<u64>(h_->total_count); }
i64 Histogram::min() const noexcept { return count() == 0 ? 0 : hdr_min(h_); }
i64 Histogram::max() const noexcept { return hdr_max(h_); }
i64 Histogram::at(double percentile) const noexcept {
    return hdr_value_at_percentile(h_, percentile);
}

std::string Histogram::summary() const {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "count %" PRIu64 "  min %" PRId64 "  p50 %" PRId64 "  p99 %" PRId64
                  "  p99.9 %" PRId64 "  p99.99 %" PRId64 "  max %" PRId64 " ns%s",
                  count(), min(), at(50.0), at(99.0), at(99.9), at(99.99), max(),
                  (clamped_low_ + clamped_high_) != 0 ? "  (some values clamped)" : "");
    return line;
}

void Histogram::write_hgrm(std::FILE* out) const {
    hdr_percentiles_print(h_, out, 5, 1000.0, CLASSIC);
}

}  // namespace ttt::measure
