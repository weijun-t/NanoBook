/*
 * l1_table.hpp: the latest best bid and ask of every stock, for a viewer in another process.
 *
 * What it does: L1Table holds one Slot per stock locate (65,536, every uint16_t), each with the
 *   stock's last TopOfBook: best bid and ask with their sizes, the update's sequence number and
 *   ITCH time, and how many updates the stock has had. The publisher thread writes it (publish());
 *   any number of readers copy a slot out (try_read()) whenever they like. A header says whether
 *   the run is starting, running or done, and counts updates; a latency histogram counts the
 *   end-to-end latency of every paced update. The stocks' names sit beside the slots.
 *
 * Why it exists: Viewer design §3. The viewer wants the latest value per stock, not every update,
 *   and must never make the publisher wait (a waiting publisher fills ring 2 and stalls the
 *   engine). One slot per stock, overwritten in place, gives exactly that: nothing to fill up,
 *   and updates between two reads are simply skipped.
 *
 * How it works:
 *   - A seqlock per slot: `seq` is even while the slot is stable, odd while it is written.
 *     Write: seq = s + 1, then the fields, then seq = s + 2. Read: seq (even, else busy), the
 *     fields, seq again; if it changed, the writer was in the middle and the read is thrown away.
 *     O(1) both ways: 9 stores to write, 9 loads to read, no lock, no read-modify-write.
 *   - Memory orders: the writer stores the fields with release, so the odd seq is visible before
 *     any new field; the final even seq is a release too, so a reader who sees it sees the fields.
 *     The reader loads seq and the fields with acquire, so the second seq load can't move before
 *     the field loads. A reader that saw even one new field then must see seq changed (no
 *     standalone fences: ThreadSanitizer can't follow them, and GCC warns about them under TSan).
 *   - Every field is a std::atomic<std::uint64_t>: a seqlock over plain fields is a data race,
 *     undefined behaviour even if the torn copy is thrown away. On x86-64 these release stores
 *     and acquire loads are plain `mov`s; they only stop the compiler reordering.
 *   - Only the publisher writes, so a counter is bumped with a load and a store (relaxed), not a
 *     fetch_add (a locked instruction).
 *   - The symbols and ticks_per_second are written before state becomes Running (a release), and
 *     never again: a reader reads them only after seeing Running or Done (an acquire).
 *   - Latency: the bucket of `latency::bucket_of()` (latency_recorder.hpp: 1 tick wide below
 *     1,024, then 64 per doubling), in counter ticks, clamped to 32 bits like the recorder.
 *
 * Data structures: Slot is 64 bytes, one cache line (static_assert). L1Table is about 4.6 MiB:
 *   the header (two cache lines: set once, and bumped every update), 512 KiB of names, 4 MiB of
 *   slots, 19 KiB of latency buckets.
 *
 * Trade-offs: Exactly one writer: a second writer would break the seqlock. A reader can starve if
 *   one stock is rewritten faster than it can copy 64 bytes (try_read() then returns nothing and
 *   the viewer shows the slot's previous value). Holding all 65,536 slots costs 4 MiB but needs no
 *   lookup: a locate is the index.
 */

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "latency_recorder.hpp"
#include "nanobook/itch/messages.hpp"
#include "nanobook/pipeline/records.hpp"

namespace nanobook::view {

inline constexpr std::size_t kSlots = 65'536; // one per uint16_t locate
// Buckets for every 32-bit tick count: 1,024 exact ones, then 64 for each doubling 2^10..2^31.
inline constexpr std::size_t kLatencyBuckets =
    latency::kExactBelow + (32 - 10) * latency::kSubBuckets; // 2,432

inline constexpr std::uint64_t kMagic = 0x313057454956424E; // "NBVIEW01" read as little-endian
inline constexpr std::uint64_t kLayoutVersion = 1;

// Atomics in memory shared between processes must not hide a lock inside the process.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

enum class RunState : std::uint64_t { Starting = 0, Running = 1, Done = 2 };

// One stock's last update, as a reader gets it.
struct Quote {
    std::uint64_t sequence{};  // the update's number in the run (TopOfBook::sequence)
    std::uint64_t timestamp{}; // its ITCH time, ns since midnight
    std::int64_t bid_price{};  // 0: no bids
    std::int64_t bid_qty{};
    std::int64_t ask_price{}; // 0: no asks
    std::int64_t ask_qty{};
    std::uint64_t updates{}; // updates this stock has had in the run

    bool operator==(const Quote &) const = default;
};

struct alignas(64) Slot {
    std::atomic<std::uint64_t> seq{0}; // even: stable; odd: being written
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> timestamp{0};
    std::atomic<std::uint64_t> bid_price{0}; // int64_t values, stored as their bits
    std::atomic<std::uint64_t> bid_qty{0};
    std::atomic<std::uint64_t> ask_price{0};
    std::atomic<std::uint64_t> ask_qty{0};
    std::atomic<std::uint64_t> updates{0};
};
static_assert(sizeof(Slot) == 64 && alignof(Slot) == 64);

struct Header {
    // Set when the table is made, and at the start and end of the run.
    alignas(64) std::atomic<std::uint64_t> magic{0}; // kMagic once the table is ready to read
    std::atomic<std::uint64_t> version{0};
    std::atomic<std::uint64_t> run_id{0};           // different for every run
    std::atomic<std::uint64_t> state{0};            // a RunState
    std::atomic<std::uint64_t> ticks_per_second{0}; // converts latency ticks to time
    // Bumped by the publisher on every update (its own cache line).
    alignas(64) std::atomic<std::uint64_t> updates{0}; // updates published in the run
    std::atomic<std::uint64_t> itch_time{0};           // the last update's ITCH time
};

struct L1Table {
    Header header;
    std::array<itch::Symbol, kSlots> symbols{}; // by locate; set before Running, then fixed
    std::array<Slot, kSlots> slots{};
    alignas(64) std::array<std::atomic<std::uint64_t>, kLatencyBuckets> latency{};
};

// ---------------------------------------------------------------------------
// Writer side (the publisher thread only)
// ---------------------------------------------------------------------------

inline void write(Slot &slot, const Quote &q) noexcept {
    constexpr auto rel = std::memory_order_release;
    const std::uint64_t s = slot.seq.load(std::memory_order_relaxed); // only we write it
    slot.seq.store(s + 1, std::memory_order_relaxed); // odd: readers will throw their copy away
    slot.sequence.store(q.sequence, rel);             // release: the odd seq is visible before this
    slot.timestamp.store(q.timestamp, rel);
    slot.bid_price.store(std::bit_cast<std::uint64_t>(q.bid_price), rel);
    slot.bid_qty.store(std::bit_cast<std::uint64_t>(q.bid_qty), rel);
    slot.ask_price.store(std::bit_cast<std::uint64_t>(q.ask_price), rel);
    slot.ask_qty.store(std::bit_cast<std::uint64_t>(q.ask_qty), rel);
    slot.updates.store(q.updates, rel);
    slot.seq.store(s + 2, rel); // even again: the fields above are visible before this
}

// A counter only we write: a load and a store, no locked instruction.
inline void bump(std::atomic<std::uint64_t> &counter) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// One update from ring 2 into its stock's slot.
inline void publish(L1Table &table, const pipeline::TopOfBook &t) noexcept {
    Slot &slot = table.slots[t.locate];
    write(slot, Quote{.sequence = t.sequence,
                      .timestamp = t.timestamp,
                      .bid_price = t.bid_price,
                      .bid_qty = t.bid_qty,
                      .ask_price = t.ask_price,
                      .ask_qty = t.ask_qty,
                      .updates = slot.updates.load(std::memory_order_relaxed) + 1});
    bump(table.header.updates);
    table.header.itch_time.store(t.timestamp, std::memory_order_relaxed);
}

// One paced update's end-to-end latency, in counter ticks.
inline void record_latency(L1Table &table, std::uint64_t ticks) noexcept {
    const std::uint64_t clamped = ticks < std::numeric_limits<std::uint32_t>::max()
                                      ? ticks
                                      : std::numeric_limits<std::uint32_t>::max();
    bump(table.latency[latency::bucket_of(clamped)]);
}

// Before the run: names and the clock rate, then Running. Ends with Done.
inline void start(L1Table &table, std::uint64_t ticks_per_second) noexcept {
    table.header.ticks_per_second.store(ticks_per_second, std::memory_order_relaxed);
    table.header.state.store(static_cast<std::uint64_t>(RunState::Running),
                             std::memory_order_release); // the names and rate are visible first
}
inline void finish(L1Table &table) noexcept {
    table.header.state.store(static_cast<std::uint64_t>(RunState::Done), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Reader side (any thread, any process)
// ---------------------------------------------------------------------------

// One attempt to copy a slot. Nothing if the writer was in the middle of it: try again later.
[[nodiscard]] inline std::optional<Quote> try_read(const Slot &slot) noexcept {
    constexpr auto acq = std::memory_order_acquire;
    const std::uint64_t before = slot.seq.load(acq); // pairs with the writer's last release
    if ((before & 1) != 0) {
        return std::nullopt;
    }
    const Quote q{.sequence = slot.sequence.load(acq),
                  .timestamp = slot.timestamp.load(acq),
                  .bid_price = std::bit_cast<std::int64_t>(slot.bid_price.load(acq)),
                  .bid_qty = std::bit_cast<std::int64_t>(slot.bid_qty.load(acq)),
                  .ask_price = std::bit_cast<std::int64_t>(slot.ask_price.load(acq)),
                  .ask_qty = std::bit_cast<std::int64_t>(slot.ask_qty.load(acq)),
                  .updates = slot.updates.load(acq)};
    // acquire loads above: this load can't be done before them.
    if (slot.seq.load(std::memory_order_relaxed) != before) {
        return std::nullopt;
    }
    return q;
}

[[nodiscard]] inline RunState state(const L1Table &table) noexcept {
    return static_cast<RunState>(table.header.state.load(std::memory_order_acquire));
}

} // namespace nanobook::view
