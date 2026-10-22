/*
 * id_table.hpp: the fast book's order id -> slot lookup: a flat hash table, allocated once.
 *
 * What it does: Answers "which pool slot holds order #7?" for cancels and modifies. insert() when
 *   an order starts resting, erase() when it stops, find() to look one up (kNone if it isn't here).
 *
 * Why it exists: Order ids are big and spread out (exchange-wide, with gaps), slots are small and
 *   packed, so a translation table is needed. The reference book uses a tree: about 14 pointer
 *   jumps across memory for 10,000 orders. This table usually needs 1 or 2 reads, all in one
 *   array, and never allocates after construction (fast-book design §5.1).
 *
 * How it works: Open addressing with linear probing. An id's home position comes from a fixed
 *   scramble formula: multiply by 11400714819323198485 (2^64 divided by the golden ratio) and keep
 *   the top bits ("Fibonacci hashing"). Exchange ids arrive in near-sequential runs, and the
 *   multiply spreads neighbours apart. If the home is taken, try the next entry, wrapping at the
 *   end. The table has at least twice as many entries as max_orders, so it is never more than half
 *   full and runs stay short. Removal uses backward shift: after emptying an entry, later entries
 *   in the same run slide back into the hole when that brings them closer to home, so a search can
 *   never stop early at a gap, and no "deleted" markers pile up over a trading day.
 *
 * Data structures: std::pmr::vector<Entry>, sized once to a power of two. Entry = {id, slot}, 16
 *   bytes with padding. id 0 marks an empty entry: the exchange never issues id 0. pmr: the memory
 *   comes from the default memory resource when the table is built (plain new/delete, or the
 *   timed tools' huge-page arena: optimise design E6).
 *
 * Trade-offs: Memory is 2 x max_orders (rounded up to a power of two) x 16 bytes: 32 MB for 1M
 *   orders. A 12-byte packed entry, or storing only the slot and reading the id from the pool,
 * would be smaller but cost an extra read; design §5.1 measures those later.
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/order.hpp"

namespace nanobook::fast {

class IdTable {
public:
    explicit IdTable(std::uint32_t max_orders)
        : entries_(table_size(max_orders)), mask_(entries_.size() - 1),
          shift_(64 - std::countr_zero(entries_.size())) {
    }

    // The bytes a table for `max_orders` takes from its memory resource.
    [[nodiscard]] static std::size_t arena_bytes(std::uint32_t max_orders) {
        return table_size(max_orders) * sizeof(Entry);
    }

    // Adds `id` -> `slot`. `id` must not be 0 and must not already be in the table.
    void insert(OrderId id, std::uint32_t slot) {
        assert(id.value != 0 && "id 0 marks an empty entry");
        assert(2 * (size_ + 1) <= entries_.size() && "more entries than max_orders");
        std::size_t i = home(id);
        while (entries_[i].id.value != 0) {
            assert(entries_[i].id != id && "id is already in the table");
            i = (i + 1) & mask_;
        }
        entries_[i] = Entry{.id = id, .slot = slot};
        ++size_;
    }

    // The slot holding `id`, or kNone. Stops at the first empty entry: backward-shift removal
    // guarantees an id is never stored past an empty entry on its way from home.
    [[nodiscard]] std::uint32_t find(OrderId id) const {
        if (id.value == 0) {
            return kNone; // 0 is the empty marker, never a real order
        }
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            if (entries_[i].id == id) {
                return entries_[i].slot;
            }
            if (entries_[i].id.value == 0) {
                return kNone;
            }
        }
    }

    // Removes `id`, which must be in the table.
    void erase(OrderId id) {
        std::size_t hole = home(id);
        while (entries_[hole].id != id) {
            assert(entries_[hole].id.value != 0 && "erasing an id that isn't in the table");
            hole = (hole + 1) & mask_;
        }
        // Backward shift: walk the rest of the run. An entry can move into the hole unless its
        // home lies after the hole (cyclically, in (hole, j]): moving it there would put it before
        // its own home, where a search would never look.
        for (std::size_t j = (hole + 1) & mask_; entries_[j].id.value != 0; j = (j + 1) & mask_) {
            if (!in_cyclic_range(home(entries_[j].id), hole, j)) {
                entries_[hole] = entries_[j];
                hole = j;
            }
        }
        entries_[hole] = Entry{};
        --size_;
    }

    [[nodiscard]] std::size_t size() const {
        return size_;
    }
    [[nodiscard]] std::size_t capacity() const { // number of entries, a power of two
        return entries_.size();
    }

    // Where `id` would be stored if nothing collided (public for the unit tests).
    [[nodiscard]] std::size_t home(OrderId id) const {
        constexpr std::uint64_t kFibonacci = 11400714819323198485ULL; // 2^64 / golden ratio
        return static_cast<std::size_t>((id.value * kFibonacci) >> shift_);
    }

    // Checks that every entry can be found: no empty entry between its home and where it sits, and
    // the count is right (for invariant checks; slow, tests only).
    [[nodiscard]] bool consistent() const {
        std::size_t count = 0;
        for (std::size_t p = 0; p < entries_.size(); ++p) {
            if (entries_[p].id.value == 0) {
                continue;
            }
            ++count;
            for (std::size_t i = home(entries_[p].id); i != p; i = (i + 1) & mask_) {
                if (entries_[i].id.value == 0) {
                    return false;
                }
            }
        }
        return count == size_;
    }

private:
    struct Entry {
        OrderId id{}; // 0 = empty
        std::uint32_t slot{kNone};
    };

    // At least twice max_orders, rounded up to a power of two (so "& mask_" can wrap).
    [[nodiscard]] static std::size_t table_size(std::uint32_t max_orders) {
        return std::bit_ceil(std::max<std::uint64_t>(2 * std::uint64_t{max_orders}, 2));
    }

    // Is k in (from, to], going round the end of the array if needed?
    [[nodiscard]] static bool in_cyclic_range(std::size_t k, std::size_t from, std::size_t to) {
        return from <= to ? (from < k && k <= to) : (k > from || k <= to);
    }

    std::pmr::vector<Entry> entries_; // sized once; never resized
    std::size_t mask_;                // entries_.size() - 1, so "& mask_" wraps at the end
    int shift_; // 64 - log2(entries_.size()): keeps the top bits of the scramble
    std::size_t size_{0};
};

} // namespace nanobook::fast
