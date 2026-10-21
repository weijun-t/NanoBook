/*
 * itch_timing.hpp: times an ITCH replay without timing the disk (ITCH design §4, §9).
 *
 * What it does: replay_in_windows() feeds a mapped file to a parser Handler one window at a time
 *   (say 1 GiB): it loads the window's pages first (untimed), times only the parsing and the book
 *   work, then releases the pages. ParseOnly is a Handler that decodes every message and does
 *   almost nothing else, to time the parser alone. TimedOrders wraps a Handler and times every
 *   Nth order message with the phase 3 timer, tagged by kind, into a phase 3 Recorder.
 *   replay_in_windows() also counts the page faults taken while timed (phase 6 step 1).
 *
 * Why it exists: Step 6 measures messages per second and per-message latency on the real day.
 *   Reading 11 GB from disk takes longer than the replay itself, so the disk must stay out of the
 *   timed part. The file doesn't fit in memory next to the books on WSL2, hence windows.
 *
 * How it works:
 *   - A window ends at the last whole message: parse() stops before an incomplete one, and the
 *     next window starts there. Loading = madvise(MADV_WILLNEED), then reading one byte per 4 KiB
 *     page, so every page is mapped before the clock starts. Releasing = madvise(MADV_DONTNEED):
 *     the pages leave this process (the kernel's file cache may keep them), so memory stays at
 *     about one window. Cost: one pass, O(file).
 *   - Page faults: page_faults_now() just before the clock starts and just after it stops, in
 *     every window, summed. Loading the window's pages faults too, but before the first reading,
 *     so what's left is the handler's own faults (the books touching new memory: optimise design
 *     E2). Two system calls per window, outside the clock.
 *   - ParseOnly adds each message's timestamp (and order ref) into a sum, so the compiler can't
 *     skip the decoding as unused.
 *   - TimedOrders: on every Nth order message, start(), the inner handler's on(), stop(), record.
 *     Tags (latency-harness design §5.3): A, F -> add; E, C -> match (an execution is a match);
 *     D -> cancel (a full cancel); X -> modify (a size cut that keeps its place); U -> replace.
 *     The other messages and the other N-1 order messages run untimed, at full speed.
 *
 * Trade-offs: Tool code. A timed sample includes the builder's work (checks, conversion, the
 *   result count) and the book's, not the parsing. Timing 1 message in N disturbs the replay
 *   less: the timer's fences cost ~12 ns and stop the CPU overlapping that message with the next.
 */

#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

#include "latency_recorder.hpp"
#include "nanobook/itch/messages.hpp"
#include "nanobook/itch/parser.hpp"
#include "page_faults.hpp"
#include "tsc.hpp"

namespace nanobook::itch {

inline constexpr std::size_t kPage = 4096;

// Maps every page of `bytes` into this process, so touching them later costs no page fault.
inline void load_pages(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(bytes.data()) & ~(kPage - 1); // NOLINT
    madvise(reinterpret_cast<void *>(first), // NOLINT(performance-no-int-to-ptr): page address
            reinterpret_cast<std::uintptr_t>(bytes.data()) + bytes.size() - first, // NOLINT
            MADV_WILLNEED);
    unsigned sum = 0; // one byte from every page: reading it maps the page in
    for (std::size_t i = 0; i < bytes.size(); i += kPage) {
        sum += std::to_integer<unsigned>(bytes[i]);
    }
    sum += std::to_integer<unsigned>(bytes.back());
    asm volatile("" : : "r"(sum)); // "uses" sum, so the compiler must keep the reads
}

// Lets the kernel take back the whole pages inside `bytes` (MADV_DONTNEED). Only for a read-only
// FILE mapping, whose pages come back from the file when touched again. On ordinary memory (the
// heap, a std::vector) MADV_DONTNEED zeroes the pages, which would destroy whatever else shares
// them: never pass that here.
inline void release_pages(std::span<const std::byte> bytes) {
    const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data()); // NOLINT
    const std::uintptr_t first = (begin + kPage - 1) & ~(kPage - 1);   // round up
    const std::uintptr_t last = (begin + bytes.size()) & ~(kPage - 1); // round down
    if (last > first) {
        madvise(reinterpret_cast<void *>(first), last - first, // NOLINT(performance-no-int-to-ptr)
                MADV_DONTNEED);
    }
}

struct WindowedRun {
    std::uint64_t timed_ticks{};  // parsing + handler work, all windows; loading excluded
    latency::PageFaults faults{}; // taken while timed, all windows; loading excluded
    std::size_t windows{};
    std::size_t bytes{}; // consumed: the whole file if it ends on a whole message
};

// Replays `file` (a read-only file mapping: see release_pages()) through `handler` in windows of
// about `window_bytes`, timing only parse(). A window must hold at least one whole message (52
// bytes, the longest ITCH message plus its length), or the replay stops early.
template <Handler H>
std::expected<WindowedRun, ParseError> replay_in_windows(std::span<const std::byte> file,
                                                         std::size_t window_bytes, H &handler) {
    WindowedRun run;
    while (run.bytes < file.size()) {
        const std::span<const std::byte> window =
            file.subspan(run.bytes, std::min(window_bytes, file.size() - run.bytes));
        load_pages(window);
        const latency::PageFaults f0 = latency::page_faults_now();
        const latency::Stamp t0 = latency::start();
        const auto parsed = parse(window, handler);
        const latency::Stamp t1 = latency::stop();
        run.faults += latency::page_faults_now() - f0;
        if (!parsed) {
            ParseError e = parsed.error();
            e.offset += run.bytes; // from the start of the file
            return std::unexpected(e);
        }
        run.timed_ticks += t1.ticks - t0.ticks;
        ++run.windows;
        if (*parsed == 0) {
            break; // an incomplete message that no larger window will complete: the file's end
        }
        release_pages(window.first(*parsed));
        run.bytes += *parsed;
    }
    return run;
}

// Decodes every message and keeps a sum of its fields, so the decoding can't be optimised away.
struct ParseOnly {
    std::uint64_t messages{};
    std::uint64_t sum{};

    template <typename Message> void on(const Message &m) {
        ++messages;
        sum += m.timestamp;
        if constexpr (requires { m.order_ref; }) {
            sum += m.order_ref;
        }
    }
    void on_skipped(char type) {
        ++messages;
        sum += static_cast<unsigned char>(type);
    }
};

// Times every `every`-th order message passed on to `inner`.
template <typename Inner> class TimedOrders {
public:
    TimedOrders(Inner &inner, latency::Recorder &recorder, std::uint64_t every)
        : inner_(inner), recorder_(recorder), every_(every) {
    }

    void on(const AddOrder &m) {
        timed(m, latency::Tag::Add);
    }
    void on(const OrderExecuted &m) {
        timed(m, latency::Tag::Match);
    }
    void on(const OrderExecutedWithPrice &m) {
        timed(m, latency::Tag::Match);
    }
    void on(const OrderCancel &m) {
        timed(m, latency::Tag::Modify);
    }
    void on(const OrderDelete &m) {
        timed(m, latency::Tag::Cancel);
    }
    void on(const OrderReplace &m) {
        timed(m, latency::Tag::Replace);
    }
    template <typename Message> void on(const Message &m) { // S, R, H: never timed
        inner_.on(m);
    }
    void on_skipped(char type) {
        inner_.on_skipped(type);
    }

private:
    template <typename Message> void timed(const Message &m, latency::Tag tag) {
        if (++order_messages_ % every_ != 0) {
            inner_.on(m);
            return;
        }
        const latency::Stamp t0 = latency::start();
        inner_.on(m);
        const latency::Stamp t1 = latency::stop();
        recorder_.record(t0, t1, tag);
    }

    Inner &inner_;
    latency::Recorder &recorder_;
    std::uint64_t every_;
    std::uint64_t order_messages_{};
};

} // namespace nanobook::itch
