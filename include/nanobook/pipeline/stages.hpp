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
 *   - Waiting: a full or empty ring spins (SpscRing::push/pop); every full-ring wait is counted.
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
struct NoStamp {
    std::uint64_t operator()() const {
        return 0;
    }
};

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
        r.sent = stamp_();
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
};

struct PipelineResult {
    bool parsed{false}; // the feed read the whole input
    std::uint64_t records{};
    std::uint64_t skipped{};
    std::uint64_t updates{};      // TopOfBooks published (End excluded)
    std::uint64_t feed_waits{};   // times ring 1 was full
    std::uint64_t engine_waits{}; // times ring 2 was full
    std::uint64_t allocations{};  // while records flowed (0 if not counted)
    bool feed_pinned{true};
    bool engine_pinned{true};
    bool publish_pinned{true};
};

// The pipeline without threads: the same encode -> apply -> sink, on the calling thread.
template <typename Builder, typename ReadFeed, typename Sink>
PipelineResult run_sequential(ReadFeed &&read_feed, Builder &builder, Sink &sink) {
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
    FeedEncoder<Direct, NoStamp> feed{direct, NoStamp{}};
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

    const auto start_together = [&ready, &go] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) {
            cpu_relax();
        }
    };
    {
        std::jthread feed_thread{[&] {
            result.feed_pinned = pin_this_thread(options.feed_core);
            start_together();
            FeedEncoder<RecordRing, Stamp> feed{*records, stamp};
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
                records->pop(r);
                if (!engine.apply(r, [&](const TopOfBook &t) { waits += updates->push(t); })) {
                    break;
                }
            }
            waits += updates->push(TopOfBook{.end = true});
            result.engine_waits = waits;
            finished.fetch_add(1, std::memory_order_acq_rel);
        }};
        std::jthread publish_thread{[&] {
            result.publish_pinned = pin_this_thread(options.publish_core);
            start_together();
            std::uint64_t count = 0;
            TopOfBook t;
            for (;;) {
                updates->pop(t);
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
        go.store(true, std::memory_order_release);
        while (finished.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
        result.allocations = options.allocations ? options.allocations() - before : 0;
    } // the three threads are joined here: their writes to `result` are now visible
    return result;
}

} // namespace nanobook::pipeline
