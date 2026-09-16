#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "ttt/core/clock.hpp"
#include "ttt/measure/histogram.hpp"
#include "ttt/measure/manifest.hpp"
#include "ttt/measure/trailer.hpp"

namespace ttt::measure {
namespace {

using Bytes = std::vector<std::byte>;

TEST(Histogram, PercentilesOfAKnownDistribution) {
    Histogram h;
    for (i64 v = 1; v <= 100'000; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.count(), 100'000u);
    EXPECT_EQ(h.min(), 1);
    // Three significant figures: within 0.1% of the true value.
    EXPECT_NEAR(static_cast<double>(h.at(50.0)), 50'000.0, 50.0);
    EXPECT_NEAR(static_cast<double>(h.at(99.0)), 99'000.0, 99.0);
    EXPECT_NEAR(static_cast<double>(h.max()), 100'000.0, 100.0);
}

TEST(Histogram, ClampingIsCountedNotHidden) {
    Histogram h(1'000'000);
    h.record(0);
    h.record(-5);
    h.record(5'000'000);
    h.record(10);
    EXPECT_EQ(h.count(), 4u);
    EXPECT_EQ(h.clamped_low(), 2u);
    EXPECT_EQ(h.clamped_high(), 1u);
    EXPECT_NE(h.summary().find("clamped"), std::string::npos);
}

TEST(Histogram, MergeAndMove) {
    Histogram a;
    Histogram b;
    a.record(100);
    b.record(200);
    b.record(0);
    a.merge(b);
    EXPECT_EQ(a.count(), 3u);
    EXPECT_EQ(a.clamped_low(), 1u);
    Histogram c = std::move(a);
    EXPECT_EQ(c.count(), 3u);
    c.reset();
    EXPECT_EQ(c.count(), 0u);
    EXPECT_EQ(c.clamped_low(), 0u);
}

TEST(Trailer, RoundTripAndStrip) {
    Bytes packet(40, std::byte{0x42});
    Bytes datagram = packet;
    datagram.resize(packet.size() + kTrailerSize);
    const Trailer t{1, 12345, 1'000'000'000'000ULL, 1'000'000'000'777ULL};
    store_trailer(std::span<std::byte>(datagram).subspan(packet.size()), t);

    std::span<const std::byte> view(datagram);
    const auto                 got = split_trailer(view);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, t);
    EXPECT_EQ(view.size(), packet.size());

    std::span<const std::byte> plain(packet);
    EXPECT_FALSE(split_trailer(plain).has_value());
    EXPECT_EQ(plain.size(), packet.size());
}

TEST(Clock, MovesForwardInSubMicrosecondSteps) {
    // The reason macOS uses CLOCK_MONOTONIC_RAW: consecutive readings must be
    // able to differ by less than a microsecond.
    u64 smallest = ~u64{0};
    u64 prev = now_ns();
    for (int i = 0; i < 200'000; ++i) {
        const u64 v = now_ns();
        ASSERT_GE(v, prev);
        if (v != prev) {
            smallest = std::min(smallest, v - prev);
        }
        prev = v;
    }
    EXPECT_LT(smallest, 1000u);
}

TEST(Manifest, RecordsTheBasics) {
    const Manifest m = collect_manifest(ManifestOptions{"", "test"});
    auto           find = [&](const std::string& k) -> std::string {
        for (const auto& [key, v] : m) {
            if (key == k) return v;
        }
        return "";
    };
    EXPECT_EQ(find("tool"), "test");
    EXPECT_FALSE(find("git_commit").empty());
    EXPECT_FALSE(find("os").empty());
    EXPECT_FALSE(find("cpu_model").empty());
    EXPECT_NE(find("git_commit"), "unknown");
#ifdef __linux__
    EXPECT_FALSE(find("kernel_cmdline").empty());
    EXPECT_FALSE(find("isolated_cpus").empty());
#endif
#ifdef __APPLE__
    EXPECT_FALSE(find("low_power_mode").empty());
#endif
}

}  // namespace
}  // namespace ttt::measure
