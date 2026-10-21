/*
 * stages.hpp: the pipeline's three stages, and running them on three threads or on one.
 *
 * What it does: run_pipeline() replays a feed through three threads (pipeline design §2, §5):
 *   - feed:      parses the input and pushes one FeedRecord per message the books use (ring 1);
 *   - engine:    pops each record, applies it to phase 4's book builder, and after each order
 *                message pushes that stock's TopOfBook (ring 2);
 *   - publisher: pops each update and hands it to a sink (step 3: the binary log).
 *   run_sequential() does exactly the same per-message work on the calling thread, with no rings:
 *   the answer the pipeline must match, byte for byte.
 *
 * Why it exists: Phase 5 splits the work so the stages overlap and the engine's core does only
 *   book work. The book itself stays single-threaded (README rule 5): only the engine thread
 *   ever touches it.
 *
 * How it works:
 *   - Start: the rings, the books and the threads are all created first; each thread pins itself
 *     to its core (if given one) and reports ready; only when all three are ready does the
 *     caller let them start, so nothing allocates once records flow (rule 1).
 *   - Stop: the feed pushes a FeedRecord of kind End after its last record (also when the input
 *     is broken); the engine passes on a TopOfBook with end = true and stops; the publisher
 *     stops when it sees that. No shared flags: the end travels through the rings like data.
 *   - The per-message work (Engine::apply) is one function used by both run_pipeline() and
 *     run_sequential(), so the only difference between them is the threads.
 *   - Waiting: on a full or empty ring a thread spins with `pause` for kSpinsBeforeYield tries,
 *     then yields its core before each further try, so the run still progresses when there are
 *     fewer free cores than threads (a CI runner); spin_only keeps it spinning, for timed runs on
 *     pinned, dedicated cores. Every full-ring wait is counted. A producer that finds its ring
 *     full waits kFullBackoff pauses before looking again (optimise design E1b), so it reads the
 *     consumer's counter less often.
 *
 * Data structures: Two SpscRings of `Capacity` records (65,536 by default, design §3.4), on the
 *   heap. Each thread writes only its own fields of PipelineResult; the caller reads them after
 *   joining the threads (a join is a synchronisation point).
 *
 * Trade-offs: Linux only (sched_setaffinity for pinning). Three spinning threads use three cores
 *   fully for the length of the run. The caller's ReadFeed decides how the input is read (one
 *   parse() of a buffer in the tests, pre-loaded windows of a file in the tools).
 */

#pragma once

#include <sched.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "nanobook/core/types.hpp"
#include "nanobook/itch/messages.hpp"
#include "nanobook/pipeline/records.hpp"
#include "nanobook/pipeline/spsc_ring.hpp"

namespace nanobook::pipeline {

inline constexpr std::size_t kRingCapacity = 65'536; // pipeline design §3.4, decision Q8

// Pins the calling thread to `core` (-1: don't pin). True if it now runs there.
inline bool pin_this_thread(int core) {
    if (core < 0) {
        return true;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core), &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0 && sched_getcpu() == core;
}

// The default for a record's `sent` stamp: none. Step 4 passes the paced clock instead.
// It sees the record, so a pacer can tell the warm-up (before a chosen time) from the timed part.
struct NoStamp {
    std::uint64_t operator()(const FeedRecord & /*unused*/) const {
        return 0;
    }
};

// ---------------------------------------------------------------------------
// Waiting on a full or empty ring: spin first, then give the core away
// ---------------------------------------------------------------------------

// A pure spin is fastest when each thread owns a core. But if the thread being waited for isn't
// running (fewer cores than threads, as on a 4-vCPU CI runner), a spinner burns its whole time
// slice for nothing, on every hand-over: a test that takes 0.04 s on 24 CPUs took over 100 s on
// 2. So after kSpinsBeforeYield failed tries (tens of microseconds of `pause`), each further try
// first yields the core to the operating system. spin_only: never yield (timed runs on pinned,
// dedicated cores, where yielding could only add latency).
inline constexpr std::uint64_t kSpinsBeforeYield = 1'024;

inline void wait_once(std::uint64_t failed_tries, bool spin_only) {
    if (spin_only || failed_tries < kSpinsBeforeYield) {
        cpu_relax();
    } else {
        std::this_thread::yield();
    }
}

// After a push finds the ring full, the producer waits kFullBackoff pauses before looking again
// (optimise design E1b): it then reads the consumer's tail_ far less often, so that cache line
// stays with the consumer. The budget before yielding stays kSpinsBeforeYield pauses.
inline constexpr std::uint64_t kFullBackoff = 64;

// Push or pop, waiting as above. Return how many times the ring was full or empty.
template <typename Ring, typename T>
std::uint64_t push_waiting(Ring &ring, const T &item, bool spin_only) {
    std::uint64_t failed = 0;
    while (!ring.try_push(item)) {
        if (spin_only || failed < kSpinsBeforeYield / kFullBackoff) {
            for (std::uint64_t i = 0; i < kFullBackoff; ++i) {
                cpu_relax();
            }
        } else {
            std::this_thread::yield();
        }
        ++failed;
    }
    return failed;
}
template <typename Ring, typename T> std::uint64_t pop_waiting(Ring &ring, T &out, bool spin_only) {
    std::uint64_t failed = 0;
    while (!ring.try_pop(out)) {
        wait_once(failed++, spin_only);
    }
    return failed;
}

// ---------------------------------------------------------------------------
// The engine's per-message work, shared by both ways of running
// ---------------------------------------------------------------------------

template <typename Builder> class Engine {
public:
    explicit Engine(Builder &builder) : builder_(builder) {
    }

    // Applies one record to the books; after an order message for a stock that has a book,
    // passes that stock's top of book to emit(). Returns false for End.
    template <typename Emit> bool apply(const FeedRecord &r, Emit &&emit) {
        if (r.kind == FeedKind::End) {
            return false;
        }
        decode(r, builder_);
        if (is_order(r.kind) && has_book(r.locate)) {
            emit(top_of_book(r));
        }
        return true;
    }

private:
    [[nodiscard]] bool has_book(std::uint16_t locate) const {
        return locate != 0 && locate < builder_.locates() &&
               builder_.name(locate) != itch::Symbol{};
    }

    TopOfBook top_of_book(const FeedRecord &r) {
        const auto &book = builder_.book(r.locate);
        TopOfBook t{
            .sequence = ++sequence_, .timestamp = r.timestamp, .sent = r.sent, .locate = r.locate};
        if (const auto bid = book.best_bid()) {
            t.bid_price = bid->ticks;
            t.bid_qty = book.level_qty(Side::Buy, *bid).value;
        }
        if (const auto ask = book.best_ask()) {
            t.ask_price = ask->ticks;
            t.ask_qty = book.level_qty(Side::Sell, *ask).value;
        }
        return t;
    }

    Builder &builder_;
    std::uint64_t sequence_{};
};

// ---------------------------------------------------------------------------
// The feed stage: a parser Handler that turns messages into records
// ---------------------------------------------------------------------------

// Out is anything with push(const FeedRecord &) returning the waits (a ring, or the sequential
// path below).
template <typename Out, typename Stamp> class FeedEncoder {
public:
    FeedEncoder(Out &out, Stamp stamp) : out_(out), stamp_(stamp) {
    }

    template <typename Message> void on(const Message &m) {
        FeedRecord r = encode(m);
        r.sent = stamp_(r);
        waits_ += out_.push(r);
        ++records_;
    }
    void on_skipped(char /*type*/) {
        ++skipped_; // P, Q, I, ...: the books don't use them
    }
    void finish() {
        waits_ += out_.push(FeedRecord{.kind = FeedKind::End});
    }

    [[nodiscard]] std::uint64_t records() const {
        return records_;
    }
    [[nodiscard]] std::uint64_t skipped() const {
        return skipped_;
    }
    [[nodiscard]] std::uint64_t waits() const {
        return waits_;
    }

private:
    Out &out_;
    Stamp stamp_;
    std::uint64_t records_{};
    std::uint64_t skipped_{};
    std::uint64_t waits_{};
};

// ---------------------------------------------------------------------------
// Running it
// ---------------------------------------------------------------------------

struct PipelineOptions {
    int feed_core{-1}; // -1: not pinned
    int engine_core{-1};
    int publish_core{-1};
    std::uint64_t (*allocations)(){nullptr}; // if set, counts allocations while records flow
    bool spin_only{false}; // never yield while waiting (pinned, dedicated cores only)
};

struct PipelineResult {
    bool parsed{false}; // the feed read the whole input
    std::uint64_t records{};
    std::uint64_t skipped{};
    std::uint64_t updates{};      // TopOfBooks published (End excluded)
    std::uint64_t feed_waits{};   // times ring 1 was full
    std::uint64_t engine_waits{}; // times ring 2 was full
    std::uint64_t allocations{};  // while records flowed (0 if not counted)
    double elapsed_ns{};          // from "go" to the last thread finishing (system clock)
    bool feed_pinned{true};
    bool engine_pinned{true};
    bool publish_pinned{true};
};

// The pipeline without threads: the same encode -> apply -> sink, on the calling thread. `stamp`
// stamps each record's `sent`, as in run_pipeline (a pacer, for one-thread latency runs).
template <typename Builder, typename ReadFeed, typename Sink, typename Stamp = NoStamp>
PipelineResult run_sequential(ReadFeed &&read_feed, Builder &builder, Sink &sink,
                              Stamp stamp = {}) {
    struct Direct { // stands in for ring 1: applies each record at once
        Engine<Builder> engine;
        Sink &sink;
        std::uint64_t updates{};
        std::uint64_t push(const FeedRecord &r) {
            engine.apply(r, [this](const TopOfBook &t) {
                sink(t);
                ++updates;
            });
            return 0;
        }
    };
    Direct direct{Engine<Builder>{builder}, sink};
    FeedEncoder<Direct, Stamp> feed{direct, stamp};
    PipelineResult result;
    result.parsed = read_feed(feed);
    feed.finish();
    result.records = feed.records();
    result.skipped = feed.skipped();
    result.updates = direct.updates;
    return result;
}

// The pipeline on three threads. read_feed(handler) parses the whole input into `handler` and
// returns true if it read all of it; sink(update) is called on the publisher thread.
template <std::size_t Capacity = kRingCapacity, typename Builder, typename ReadFeed, typename Sink,
          typename Stamp = NoStamp>
PipelineResult run_pipeline(ReadFeed &&read_feed, Builder &builder, Sink &sink,
                            const PipelineOptions &options, Stamp stamp = {}) {
    using RecordRing = SpscRing<FeedRecord, Capacity>;
    using UpdateRing = SpscRing<TopOfBook, Capacity>;
    const auto records = std::make_unique<RecordRing>(); // all allocation happens here, before
    const auto updates = std::make_unique<UpdateRing>(); // any record flows
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> finished{0};
    PipelineResult result;

    const bool spin_only = options.spin_only;
    const auto start_together = [&ready, &go, spin_only] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        for (std::uint64_t tries = 0; !go.load(std::memory_order_acquire); ++tries) {
            wait_once(tries, spin_only);
        }
    };
    struct RecordOut { // ring 1 as the feed's output, waiting as above
        RecordRing &ring;
        bool spin_only;
        std::uint64_t push(const FeedRecord &r) {
            return push_waiting(ring, r, spin_only);
        }
    };
    {
        std::jthread feed_thread{[&] {
            result.feed_pinned = pin_this_thread(options.feed_core);
            start_together();
            RecordOut out{*records, spin_only};
            FeedEncoder<RecordOut, Stamp> feed{out, stamp};
            result.parsed = read_feed(feed);
            feed.finish(); // End goes out even if the input was broken
            result.records = feed.records();
            result.skipped = feed.skipped();
            result.feed_waits = feed.waits();
            finished.fetch_add(1, std::memory_order_acq_rel);
        }};
        std::jthread engine_thread{[&] {
            result.engine_pinned = pin_this_thread(options.engine_core);
            start_together();
            Engine<Builder> engine{builder};
            std::uint64_t waits = 0;
            FeedRecord r;
            for (;;) {
                pop_waiting(*records, r, spin_only);
                if (!engine.apply(r, [&](const TopOfBook &t) {
                        waits += push_waiting(*updates, t, spin_only);
                    })) {
                    break;
                }
            }
            waits += push_waiting(*updates, TopOfBook{.end = true}, spin_only);
            result.engine_waits = waits;
            finished.fetch_add(1, std::memory_order_acq_rel);
        }};
        std::jthread publish_thread{[&] {
            result.publish_pinned = pin_this_thread(options.publish_core);
            start_together();
            std::uint64_t count = 0;
            TopOfBook t;
            for (;;) {
                pop_waiting(*updates, t, spin_only);
                if (t.end) {
                    break;
                }
                sink(t);
                ++count;
            }
            result.updates = count;
            finished.fetch_add(1, std::memory_order_acq_rel);
        }};

        while (ready.load(std::memory_order_acquire) < 3) { // every thread pinned and waiting
            std::this_thread::yield();
        }
        const std::uint64_t before = options.allocations ? options.allocations() : 0;
        const auto started = std::chrono::steady_clock::now(); // cold: twice per run
        go.store(true, std::memory_order_release);
        while (finished.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
        result.elapsed_ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started)
                .count();
        result.allocations = options.allocations ? options.allocations() - before : 0;
    } // the three threads are joined here: their writes to `result` are now visible
    return result;
}

} // namespace nanobook::pipeline
