/*
 * environment.hpp: collects the facts every published latency number must state (README rule 19).
 *
 * What it does: collect_environment() reads, once at start-up: the CPU model and how many logical
 *   cores there are, the kernel and OS, whether this is a virtual machine (WSL2 or any other),
 *   which cores the kernel isolated, the CPU frequency governor and turbo setting, the compiler,
 *   build type and flags, and the git commit. lines() turns it into "key: value" lines for the
 *   terminal and the CSV (latency-harness design §8).
 *
 * Why it exists: A latency number without its machine is meaningless, and numbers from a virtual
 *   machine must never be published (rule 20). Collecting it in the tool means it can't be
 *   forgotten or copied wrong by hand.
 *
 * How it works: Plain reads of /proc and /sys files, and uname(). A file that doesn't exist (WSL2
 *   has no cpufreq or intel_pstate) gives "unknown", never an error. Compiler facts come from the
 *   compiler's own macros; build type, flags and the commit are passed in by CMake (the commit is
 *   regenerated at every build, so it can't go stale). The small parsing functions take the file's
 *   text, so tests can feed them made-up contents.
 *
 * Data structures: Environment is a struct of strings, one per fact.
 *
 * Trade-offs: Linux only. Cold path: it allocates and reads files freely, before any timing.
 */

#pragma once

#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nanobook_git.hpp" // generated at build time: NANOBOOK_GIT_COMMIT
#include "tsc.hpp"

#ifndef NANOBOOK_BUILD_TYPE
#define NANOBOOK_BUILD_TYPE "unknown"
#endif
#ifndef NANOBOOK_CXX_FLAGS
#define NANOBOOK_CXX_FLAGS "unknown"
#endif

namespace nanobook::latency {

// ---------------------------------------------------------------------------
// Small parsers, testable on made-up text
// ---------------------------------------------------------------------------

// The whole file as text; empty if it can't be read.
inline std::string read_file(const std::string &path) {
    std::ifstream in{path};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Without leading and trailing whitespace (files in /sys end with a newline).
inline std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string{s.substr(first, last - first + 1)};
}

// The value of the first "key<spaces/tabs>: value" line, as in /proc/cpuinfo. Empty if absent.
inline std::string cpuinfo_value(std::string_view cpuinfo, std::string_view key) {
    std::istringstream lines{std::string{cpuinfo}};
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos && trim(std::string_view{line}.substr(0, colon)) == key) {
            return trim(std::string_view{line}.substr(colon + 1));
        }
    }
    return {};
}

// How many "processor : N" lines /proc/cpuinfo has: the logical cores.
inline int count_processors(std::string_view cpuinfo) {
    std::istringstream lines{std::string{cpuinfo}};
    std::string line;
    int n = 0;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos &&
            trim(std::string_view{line}.substr(0, colon)) == "processor") {
            ++n;
        }
    }
    return n;
}

// PRETTY_NAME="Ubuntu 24.04.5 LTS" from /etc/os-release, without the quotes.
inline std::string os_pretty_name(std::string_view os_release) {
    std::istringstream lines{std::string{os_release}};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.starts_with("PRETTY_NAME=")) {
            std::string value = trim(std::string_view{line}.substr(12));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
    }
    return {};
}

// WSL2's kernel names itself, e.g. "6.18.54.1-microsoft-standard-WSL2".
inline bool is_wsl(std::string_view kernel_release) {
    std::string lower{kernel_release};
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("microsoft") != std::string::npos || lower.find("wsl") != std::string::npos;
}

// intel_pstate's no_turbo file: "1" means turbo is off, "0" on.
inline std::string turbo_from_no_turbo(std::string_view no_turbo) {
    const std::string v = trim(no_turbo);
    if (v == "1") {
        return "off";
    }
    if (v == "0") {
        return "on";
    }
    return "unknown";
}

inline std::string or_unknown(std::string s) {
    return s.empty() ? "unknown" : s;
}

// ---------------------------------------------------------------------------
// The environment
// ---------------------------------------------------------------------------

struct Environment {
    std::string cpu;
    std::string logical_cores;
    std::string os;
    std::string kernel;
    std::string virtual_machine; // "no", or why it is one (WSL2, the hypervisor CPU flag)
    std::string isolated_cores;
    std::string nohz_full_cores;
    std::string governor;
    std::string turbo;
    std::string compiler;
    std::string build_type;
    std::string flags;
    std::string commit;

    // Published numbers must come from bare metal (rule 20).
    [[nodiscard]] bool publishable() const {
        return virtual_machine == "no";
    }

    // "key: value" lines, in a fixed order.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> lines() const {
        return {{"cpu", cpu},
                {"logical cores", logical_cores},
                {"os", os},
                {"kernel", kernel},
                {"virtual machine", virtual_machine},
                {"isolated cores", isolated_cores},
                {"nohz_full cores", nohz_full_cores},
                {"governor", governor},
                {"turbo", turbo},
                {"compiler", compiler},
                {"build type", build_type},
                {"flags", flags},
                {"commit", commit}};
    }
};

inline std::string compiler_name() {
#if defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#else
    return "unknown";
#endif
}

// `core` is the core the program is pinned to, or -1: its governor is the one reported.
inline Environment collect_environment(int core) {
    Environment env;
    const std::string cpuinfo = read_file("/proc/cpuinfo");
    env.cpu = or_unknown(cpuinfo_value(cpuinfo, "model name"));
    env.logical_cores = std::to_string(count_processors(cpuinfo));
    env.os = or_unknown(os_pretty_name(read_file("/etc/os-release")));

    utsname u{};
    std::string release;
    if (uname(&u) == 0) {
        release = u.release;
        env.kernel = std::string{u.sysname} + " " + u.release + " " + u.machine;
    } else {
        env.kernel = "unknown";
    }

    if (is_wsl(release)) {
        env.virtual_machine = "yes: WSL2";
    } else if (has_flag(read_cpu_flags(), "hypervisor")) {
        env.virtual_machine = "yes: the CPU reports a hypervisor";
    } else {
        env.virtual_machine = "no";
    }

    const std::string cpu_dir = "/sys/devices/system/cpu/";
    env.isolated_cores = trim(read_file(cpu_dir + "isolated"));
    env.isolated_cores = env.isolated_cores.empty() ? "none" : env.isolated_cores;
    env.nohz_full_cores = or_unknown(trim(read_file(cpu_dir + "nohz_full")));
    const std::string governor_core = std::to_string(core >= 0 ? core : 0);
    env.governor =
        or_unknown(trim(read_file(cpu_dir + "cpu" + governor_core + "/cpufreq/scaling_governor")));
    env.turbo = turbo_from_no_turbo(read_file(cpu_dir + "intel_pstate/no_turbo"));

    env.compiler = compiler_name();
    env.build_type = NANOBOOK_BUILD_TYPE;
    env.flags = NANOBOOK_CXX_FLAGS;
    env.commit = NANOBOOK_GIT_COMMIT;
    return env;
}

} // namespace nanobook::latency
