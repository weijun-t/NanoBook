/*
 * id_table.hpp: the fast book's order id -> slot lookup: a flat hash table, allocated once.
 *
 * What it does: Answers "which pool slot holds order #7?" for cancels and modifies. insert() when
 *   an order starts resting, erase() when it stops, find() to look one up (kNone if it isn't here).
 *   find() and erase() take id_of: a function giving the id of the order in a slot (the book
 *   passes "read it from the pool"), used to confirm a match.
 *
 * Why it exists: Order ids are big and spread out (exchange-wide, with gaps), slots are small and
 *   packed, so a translation table is needed. The reference book uses a tree: about 14 pointer
 *   jumps across memory for 10,000 orders. This table usually needs 1 or 2 reads, all in one
 *   array, and never allocates after construction (fast-book design §5.1).
 *
 * How it works: Open addressing with linear probing. An id's 64-bit hash is a fixed scramble:
 *   multiply by 11400714819323198485 (2^64 divided by the golden ratio; odd, so different ids
 *   always give different hashes). Its top bits are the id's home entry ("Fibonacci hashing").
 *   Exchange ids arrive in near-sequential runs, and the multiply spreads neighbours apart. If the
 *   home is taken, try the next entry, wrapping at the end. The table has at least twice as many
 *   entries as max_orders, so it is never more than half full and runs stay short. Removal uses
 *   backward shift: after emptying an entry, later entries in the same run slide back into the
 *   hole when that brings them closer to home, so a search can never stop early at a gap, and no
 *   "deleted" markers pile up over a trading day.
 *   - 8-byte entries (optimise design E9b): an entry keeps the top 32 bits of the hash (its tag)
 *     and the slot, not the 8-byte id. Equal tags are confirmed with id_of(slot): the order's own
 *     id in the pool, which the book reads next anyway. Two different ids share a tag about once
 *     in 4 billion, and then the confirmation tells them apart. The home is the top bits of the
 *     hash, so it can be recomputed from the tag alone, which backward shift needs.
 *
 * Data structures: std::pmr::vector<Entry>, sized once to a power of two. Entry = {tag, slot}, 8
 *   bytes; slot kNone marks an empty entry. pmr: the memory comes from the default memory
 *   resource when the table is built (plain new/delete, or the timed tools' huge-page arena:
 *   optimise design E6).
 *
 * Trade-offs: Memory is 2 x max_orders (rounded up to a power of two) x 8 bytes: 16 MB for 1M
 *   orders, half the 16-byte entries' size, so twice as many entries fit in each cache level. The
 *   cost is the confirmation read of the slot, on a match only. At most 2^31 orders, so the home
 *   fits in the tag's 32 bits (asserted). id 0 is never stored: the exchange never issues it.
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
        assert(shift_ >= 32 && "more than 2^32 entries: the home no longer fits in the tag");
    }

    // The bytes a table for `max_orders` takes from its memory resource.
    [[nodiscard]] static std::size_t arena_bytes(std::uint32_t max_orders) {
        return table_size(max_orders) * sizeof(Entry);
    }

    // Adds `id` -> `slot`. `id` must not be 0 and must not already be in the table (the book
    // checks with find() first).
    void insert(OrderId id, std::uint32_t slot) {
        assert(id.value != 0 && "id 0 is never a real order");
        assert(slot != kNone && "kNone marks an empty entry");
        assert(2 * (size_ + 1) <= entries_.size() && "more entries than max_orders");
        std::size_t i = home(id);
        while (entries_[i].slot != kNone) {
            i = (i + 1) & mask_;
        }
        entries_[i] = Entry{.tag = tag(id), .slot = slot};
        ++size_;
    }

    // The slot holding `id`, or kNone. id_of(slot) gives the id stored in a slot. Stops at the
    // first empty entry: backward-shift removal guarantees an id is never stored past an empty
    // entry on its way from home.
    template <typename IdOf> [[nodiscard]] std::uint32_t find(OrderId id, IdOf &&id_of) const {
        if (id.value == 0) {
            return kNone; // never a real order
        }
        const std::uint32_t t = tag(id);
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Entry e = entries_[i];
            if (e.slot == kNone) {
                return kNone;
            }
            if (e.tag == t && id_of(e.slot) == id) {
                return e.slot;
            }
        }
    }

    // Removes `id`, which must be in the table. id_of as for find().
    template <typename IdOf> void erase(OrderId id, IdOf &&id_of) {
        const std::uint32_t t = tag(id);
        std::size_t hole = home(id);
        while (entries_[hole].tag != t || id_of(entries_[hole].slot) != id) {
            assert(entries_[hole].slot != kNone && "erasing an id that isn't in the table");
            hole = (hole + 1) & mask_;
        }
        // Backward shift: walk the rest of the run. An entry can move into the hole unless its
        // home lies after the hole (cyclically, in (hole, j]): moving it there would put it before
        // its own home, where a search would never look.
        for (std::size_t j = (hole + 1) & mask_; entries_[j].slot != kNone; j = (j + 1) & mask_) {
            if (!in_cyclic_range(home_of_tag(entries_[j].tag), hole, j)) {
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
        return static_cast<std::size_t>(hash(id) >> shift_);
    }
    // The top 32 bits of `id`'s hash: what an entry stores (public for the unit tests).
    [[nodiscard]] static std::uint32_t tag(OrderId id) {
        return static_cast<std::uint32_t>(hash(id) >> 32U);
    }

    // Checks that every entry can be found: no empty entry between its home and where it sits, and
    // the count is right (for invariant checks; slow, tests only).
    [[nodiscard]] bool consistent() const {
        std::size_t count = 0;
        for (std::size_t p = 0; p < entries_.size(); ++p) {
            if (entries_[p].slot == kNone) {
                continue;
            }
            ++count;
            for (std::size_t i = home_of_tag(entries_[p].tag); i != p; i = (i + 1) & mask_) {
                if (entries_[i].slot == kNone) {
                    return false;
                }
            }
        }
        return count == size_;
    }

private:
    struct Entry {
        std::uint32_t tag{};       // top 32 bits of the id's hash
        std::uint32_t slot{kNone}; // kNone = empty
    };
    static_assert(sizeof(Entry) == 8);

    static std::uint64_t hash(OrderId id) {
        constexpr std::uint64_t kFibonacci = 11400714819323198485ULL; // 2^64 / golden ratio
        return id.value * kFibonacci;
    }
    // The home of an id with this tag: the hash's top (64 - shift_) bits are the tag's top ones.
    [[nodiscard]] std::size_t home_of_tag(std::uint32_t t) const {
        return static_cast<std::size_t>(t >> (shift_ - 32));
    }

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
