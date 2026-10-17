/*
 * temporary_id_index.hpp: the fast book's order id -> slot lookup, TEMPORARY version.
 *
 * What it does: Answers "which pool slot holds order #7?" for cancels and modifies, and is
 *   updated whenever an order starts or stops resting.
 *
 * Why it exists: Step 4 of the phase 2 plan builds the fast book correct first, with a simple,
 *   trusted lookup. Step 5 replaces it with the flat hash table from fast-book design §5.1 (same
 *   insert / find / erase interface) and records the before/after cancel numbers.
 *
 * How it works: A std::unordered_map from the id's number to the slot. find() returns kNone when
 *   the id isn't resting here.
 *
 * Data structures: std::unordered_map<uint64_t, uint32_t>: a hash table with a separate heap node
 *   per entry ("chaining"). Buckets are reserved for max_orders entries up front.
 *
 * Trade-offs: Allocates a node on every insert and scatters entries in memory, which the flat
 *   table fixes. That's why it is temporary.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/order.hpp"

namespace nanobook::fast {

class TemporaryIdIndex {
public:
    explicit TemporaryIdIndex(std::uint32_t capacity) {
        slots_.reserve(capacity);
    }

    void insert(OrderId id, std::uint32_t slot) {
        [[maybe_unused]] const bool inserted = slots_.emplace(id.value, slot).second;
        assert(inserted && "id is already resting");
    }

    // The slot holding `id`, or kNone if it isn't resting here.
    [[nodiscard]] std::uint32_t find(OrderId id) const {
        const auto it = slots_.find(id.value);
        return it == slots_.end() ? kNone : it->second;
    }

    void erase(OrderId id) {
        [[maybe_unused]] const std::size_t erased = slots_.erase(id.value);
        assert(erased == 1 && "erasing an id that isn't resting");
    }

    [[nodiscard]] std::size_t size() const {
        return slots_.size();
    }

private:
    std::unordered_map<std::uint64_t, std::uint32_t> slots_;
};

} // namespace nanobook::fast
