// The conditions a measurement was taken under, written at the top of every
// result file.
//
// A latency number depends on the machine state as much as on the code:
// isolated cores, nohz_full, the governor, C-state limits, transparent huge
// pages, where the NIC's interrupts land, whether the package was throttling.
// Project 1 learned that the hard way with Low Power Mode, which halved every
// figure silently. So every result carries what the kernel actually reported
// when it ran, never what the setup script meant to configure. A result
// without its manifest is not a measurement.
#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ttt::measure {

struct ManifestOptions {
    std::string iface;  // network interface to describe, if any
    std::string tool;   // which binary and mode produced the result
};

using Manifest = std::vector<std::pair<std::string, std::string>>;

// Reads the state. Anything unavailable on this platform is "n/a".
[[nodiscard]] Manifest collect_manifest(const ManifestOptions& opt);

// Writes "# key: value" lines, which .hgrm readers ignore as comments.
void write_manifest(std::FILE* out, const Manifest& m);

}  // namespace ttt::measure
