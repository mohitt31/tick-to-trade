#include "ttt/measure/manifest.hpp"

#include <dirent.h>
#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "itch/core/assert.hpp"
#include "ttt/version.hpp"

namespace ttt::measure {

namespace {

const std::string kNa = "n/a";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

[[maybe_unused]] std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return kNa;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string v = trim(ss.str());
    return v.empty() ? std::string("(empty)") : v;
}

std::string run(const char* cmd) {
    std::FILE* p = ::popen(cmd, "r");
    if (p == nullptr) {
        return kNa;
    }
    std::string out;
    char        buf[512];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) {
        out += buf;
    }
    ::pclose(p);
    out = trim(out);
    return out.empty() ? kNa : out;
}

#ifdef __linux__
std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string   line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            const auto colon = line.find(':');
            return colon == std::string::npos ? kNa : trim(line.substr(colon + 1));
        }
    }
    return kNa;
}

// Hottest thermal zone, in millidegrees as the kernel reports it.
std::string max_temperature() {
    long best = LONG_MIN;
    for (int i = 0; i < 64; ++i) {
        const std::string v =
            read_file("/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp");
        if (v == kNa) {
            break;
        }
        best = std::max(best, std::strtol(v.c_str(), nullptr, 10));
    }
    return best == LONG_MIN ? kNa : std::to_string(best) + " mC";
}

std::string throttle_count() {
    long total = 0;
    bool any = false;
    for (int i = 0; i < 1024; ++i) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i);
        const std::string v = read_file(base + "/thermal_throttle/core_throttle_count");
        if (v == kNa) {
            if (read_file(base + "/online") == kNa && i > 0) {
                break;
            }
            continue;
        }
        any = true;
        total += std::strtol(v.c_str(), nullptr, 10);
    }
    return any ? std::to_string(total) : kNa;
}

std::string affinity() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) != 0) {
        return kNa;
    }
    std::string out;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (CPU_ISSET(c, &set)) {
            out += (out.empty() ? "" : ",") + std::to_string(c);
        }
    }
    return out;
}

std::string status_field(const char* key) {
    std::ifstream f("/proc/self/status");
    std::string   line;
    while (std::getline(f, line)) {
        if (line.rfind(key, 0) == 0) {
            return trim(line.substr(std::string(key).size()));
        }
    }
    return kNa;
}

// Interrupt lines naming the interface, with where each is allowed to run and
// where it actually runs.
std::string iface_irqs(const std::string& iface) {
    std::ifstream f("/proc/interrupts");
    std::string   line;
    std::string   out;
    while (std::getline(f, line)) {
        if (line.find(iface) == std::string::npos) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string irq = trim(line.substr(0, colon));
        out += (out.empty() ? "" : "; ") + irq + " allowed " +
               read_file("/proc/irq/" + irq + "/smp_affinity_list") + " effective " +
               read_file("/proc/irq/" + irq + "/effective_affinity_list");
    }
    return out.empty() ? kNa : out;
}
#endif

}  // namespace

Manifest collect_manifest(const ManifestOptions& opt) {
    Manifest m;
    auto add = [&](std::string k, std::string v) { m.emplace_back(std::move(k), std::move(v)); };

    char        when[64];
    std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    add("time_utc", when);
    add("tool", opt.tool.empty() ? kNa : opt.tool);
    add("git_commit", TTT_GIT_COMMIT);
    add("compiler", __VERSION__);
#ifdef NDEBUG
    add("assertions", "NDEBUG");
#else
    add("assertions", "debug");
#endif
    add("invariant_level", std::to_string(ITCH_INVARIANT_LEVEL));

    char host[256] = {};
    ::gethostname(host, sizeof(host) - 1);
    add("hostname", host);
    utsname u{};
    if (::uname(&u) == 0) {
        add("os", std::string(u.sysname) + " " + u.release + " " + u.machine);
    }
    add("cpus_online", std::to_string(::sysconf(_SC_NPROCESSORS_ONLN)));

#ifdef __linux__
    add("cpu_model", cpu_model());
    add("kernel_cmdline", read_file("/proc/cmdline"));
    add("isolated_cpus", read_file("/sys/devices/system/cpu/isolated"));
    add("nohz_full", read_file("/sys/devices/system/cpu/nohz_full"));
    add("governor_cpu0", read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"));
    add("epp_cpu0",
        read_file("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"));
    add("intel_pstate_no_turbo", read_file("/sys/devices/system/cpu/intel_pstate/no_turbo"));
    add("cpuidle_driver", read_file("/sys/devices/system/cpu/cpuidle/current_driver"));
    add("intel_idle_max_cstate", read_file("/sys/module/intel_idle/parameters/max_cstate"));
    add("thp_enabled", read_file("/sys/kernel/mm/transparent_hugepage/enabled"));
    add("thp_defrag", read_file("/sys/kernel/mm/transparent_hugepage/defrag"));
    add("sched_rt_runtime_us", read_file("/proc/sys/kernel/sched_rt_runtime_us"));
    add("busy_poll", read_file("/proc/sys/net/core/busy_poll"));
    add("busy_read", read_file("/proc/sys/net/core/busy_read"));
    add("max_temperature", max_temperature());
    add("core_throttle_count", throttle_count());
    add("process_affinity", affinity());
    add("process_vmlck", status_field("VmLck:"));
    if (!opt.iface.empty()) {
        const std::string net = "/sys/class/net/" + opt.iface;
        add("iface", opt.iface);
        add("iface_speed_mbps", read_file(net + "/speed"));
        add("iface_mtu", read_file(net + "/mtu"));
        add("iface_driver",
            run(("basename \"$(readlink " + net + "/device/driver)\" 2>/dev/null").c_str()));
        add("iface_napi_defer_hard_irqs", read_file(net + "/napi_defer_hard_irqs"));
        add("iface_gro_flush_timeout", read_file(net + "/gro_flush_timeout"));
        add("iface_xdp",
            run(("ip -d link show " + opt.iface + " 2>/dev/null | grep -o 'xdp[a-z]*' | head -1")
                    .c_str()));
        add("iface_irqs", iface_irqs(opt.iface));
        add("iface_coalesce",
            run(("ethtool -c " + opt.iface +
                 " 2>/dev/null | grep -E '^(adaptive|rx-usecs|rx-frames)' | tr '\\n' ' '")
                    .c_str()));
        add("iface_eee",
            run(("ethtool --show-eee " + opt.iface + " 2>/dev/null | grep -m1 'EEE status'")
                    .c_str()));
    }
#endif
#ifdef __APPLE__
    char   brand[256] = {};
    size_t len = sizeof(brand);
    if (::sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) == 0) {
        add("cpu_model", brand);
    }
    // Project 1 found Low Power Mode halves throughput; record it every time.
    add("low_power_mode", run("pmset -g 2>/dev/null | awk '/lowpowermode/ {print $2}'"));
    add("power_source", run("pmset -g ps 2>/dev/null | head -1"));
#endif
    return m;
}

void write_manifest(std::FILE* out, const Manifest& m) {
    for (const auto& [k, v] : m) {
        std::fprintf(out, "# %s: %s\n", k.c_str(), v.c_str());
    }
}

}  // namespace ttt::measure
