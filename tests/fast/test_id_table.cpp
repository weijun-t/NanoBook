/*
 * test_id_table.cpp: unit tests for the fast book's flat hash table (order id -> slot).
 *
 * What it does: Checks the table's size rule, insert / find / erase, and above all removal, the
 *   tricky part of open addressing: removing from the start and the middle of a collision run, a
 *   run that wraps around the end of the array, and the one entry that must NOT slide back. Then a
 *   mini differential test: 200,000 random inserts and erases checked against
 *   std::unordered_map after every step. And the 8-byte entries' one new case (optimise design
 *   E9b): two ids that share a tag must still be told apart, by the id in their slots.
 *
 * Why it exists: A table that "loses" an id makes a cancel answer UnknownOrder for an order that is
 *   still resting, which silently breaks the book. Each tricky case is tested on its own here.
 *
 * How it works: ids_with_home() searches small ids for ones that land on a chosen home position,
 *   so collisions are built on purpose instead of hoped for. consistent() re-checks after every
 *   change that each entry is reachable from its home. Table wraps the IdTable with what the
 *   book's pool would know, the id in each slot, which find() and erase() use to confirm a match.
 *   Ids sharing a tag are built from the multiplier's inverse: adding it to an id adds 1 to the
 *   hash, which leaves the top 32 bits (the tag) unchanged.
 */

#include <gtest/gtest.h>
#include "nanobook/fast/id_table.hpp"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace nanobook;
using namespace nanobook::fast;

namespace {

// An IdTable plus what the book's pool would know: the id stored in each slot.
class Table {
public:
    explicit Table(std::uint32_t max_orders) : table_(max_orders) {
    }
    void insert(OrderId id, std::uint32_t slot) {
        if (slot >= ids_.size()) {
            ids_.resize(slot + 1);
        }
        ids_[slot] = id; // the pool holds the id before the table does
        table_.insert(id, slot);
    }
    // What the pool would answer: the id of the order in `slot`. Defined before find() and
    // erase() use it, because its return type is deduced (a lambda).
    [[nodiscard]] auto id_of() const {
        return [this](std::uint32_t slot) { return ids_.at(slot); };
    }
    [[nodiscard]] std::uint32_t find(OrderId id) const {
        return table_.find(id, id_of());
    }
    void erase(OrderId id) {
        table_.erase(id, id_of());
    }
    [[nodiscard]] std::size_t size() const {
        return table_.size();
    }
    [[nodiscard]] std::size_t capacity() const {
        return table_.capacity();
    }
    [[nodiscard]] std::size_t home(OrderId id) const {
        return table_.home(id);
    }
    [[nodiscard]] bool consistent() const {
        return table_.consistent();
    }

private:
    IdTable table_;
    std::vector<OrderId> ids_; // by slot
};

constexpr std::size_t kIdsPerHome = 3; // enough for the longest run any test builds

// The first kIdsPerHome ids (from 1 up) whose home position in `table` is `home`. One number in,
// not two, so a caller can't swap them by mistake (clang-tidy
// bugprone-easily-swappable-parameters).
std::vector<OrderId> ids_with_home(const Table &table, std::size_t home) {
    std::vector<OrderId> ids;
    for (std::uint64_t v = 1; ids.size() < kIdsPerHome; ++v) {
        if (table.home(OrderId{v}) == home) {
            ids.push_back(OrderId{v});
        }
    }
    return ids;
}

} // namespace

// The multiplicative inverse of the hash's multiplier, modulo 2^64 (Newton's iteration: each
// step doubles the number of correct low bits).
constexpr std::uint64_t kInverse = [] {
    constexpr std::uint64_t k = 11400714819323198485ULL;
    std::uint64_t x = k; // right in the lowest 3 bits, as for any odd number
    for (int i = 0; i < 6; ++i) {
        x *= 2 - (k * x);
    }
    return x;
}();
static_assert(11400714819323198485ULL * kInverse == 1);

TEST(IdTable, IdsThatShareATagAreToldApartByTheirSlots) {
    Table table{8};
    const OrderId a{1'000};
    const OrderId b{1'000 + kInverse}; // hash(b) = hash(a) + 1: the same tag, the same home
    ASSERT_EQ(IdTable::tag(a), IdTable::tag(b));
    ASSERT_EQ(table.home(a), table.home(b));
    table.insert(a, 4);
    table.insert(b, 5);
    EXPECT_EQ(table.find(a), 4U);
    EXPECT_EQ(table.find(b), 5U); // the first entry's tag matches too; its slot holds a, not b
    table.erase(a);
    EXPECT_EQ(table.find(a), kNone);
    EXPECT_EQ(table.find(b), 5U);
    EXPECT_TRUE(table.consistent());
}

TEST(IdTable, SizeIsAPowerOfTwoAtLeastTwiceMaxOrders) {
    EXPECT_EQ(IdTable{1}.capacity(), 2U);
    EXPECT_EQ(IdTable{8}.capacity(), 16U);
    EXPECT_EQ(IdTable{1000}.capacity(), 2048U); // never more than half full
}

TEST(IdTable, EmptyTableFindsNothing) {
    const Table table{8};
    EXPECT_EQ(table.size(), 0U);
    EXPECT_EQ(table.find(OrderId{5}), kNone);
    EXPECT_EQ(table.find(OrderId{0}), kNone);
    EXPECT_TRUE(table.consistent());
}

TEST(IdTable, InsertFindErase) {
    Table table{8};
    table.insert(OrderId{7}, 3);
    table.insert(OrderId{1'000'000'007}, 4);
    EXPECT_EQ(table.find(OrderId{7}), 3U);
    EXPECT_EQ(table.find(OrderId{1'000'000'007}), 4U);
    EXPECT_EQ(table.find(OrderId{8}), kNone);
    EXPECT_EQ(table.size(), 2U);
    table.erase(OrderId{7});
    EXPECT_EQ(table.find(OrderId{7}), kNone);
    EXPECT_EQ(table.find(OrderId{1'000'000'007}), 4U);
    EXPECT_TRUE(table.consistent());
}

TEST(IdTable, IdZeroIsNeverFoundEvenWhenTheTableHasEntries) {
    Table table{8};
    table.insert(OrderId{1}, 0);
    EXPECT_EQ(table.find(OrderId{0}), kNone); // 0 marks empty entries, never a real order
}

TEST(IdTable, CollisionRunIsFoundInFull) {
    Table table{8}; // 16 entries
    const std::vector<OrderId> ids = ids_with_home(table, 5);
    for (std::uint32_t i = 0; i < 3; ++i) {
        table.insert(ids[i], i);
    }
    for (std::uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(table.find(ids[i]), i); // stored at 5, 6 and 7
    }
    EXPECT_TRUE(table.consistent());
}

TEST(IdTable, EraseFromTheMiddleOfARun) {
    Table table{8};
    const std::vector<OrderId> ids = ids_with_home(table, 5);
    for (std::uint32_t i = 0; i < 3; ++i) {
        table.insert(ids[i], i);
    }
    table.erase(ids[1]); // the hole at 6 must be filled, or ids[2] at 7 becomes unreachable
    EXPECT_EQ(table.find(ids[1]), kNone);
    EXPECT_EQ(table.find(ids[0]), 0U);
    EXPECT_EQ(table.find(ids[2]), 2U);
    EXPECT_TRUE(table.consistent());
}

TEST(IdTable, EraseTheStartOfARun) {
    Table table{8};
    const std::vector<OrderId> ids = ids_with_home(table, 5);
    for (std::uint32_t i = 0; i < 3; ++i) {
        table.insert(ids[i], i);
    }
    table.erase(ids[0]);
    EXPECT_EQ(table.find(ids[1]), 1U);
    EXPECT_EQ(table.find(ids[2]), 2U);
    EXPECT_TRUE(table.consistent());
}

TEST(IdTable, RunThatWrapsAroundTheEnd) {
    Table table{8};
    const std::size_t last = table.capacity() - 1;
    const std::vector<OrderId> ids = ids_with_home(table, last); // stored at 15, 0 and 1
    for (std::uint32_t i = 0; i < 3; ++i) {
        table.insert(ids[i], i);
    }
    for (std::uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(table.find(ids[i]), i);
    }
    table.erase(ids[0]); // the entries at 0 and 1 must slide back across the end
    EXPECT_EQ(table.find(ids[1]), 1U);
    EXPECT_EQ(table.find(ids[2]), 2U);
    EXPECT_TRUE(table.consistent());
}

// The rule that makes backward shift correct: an entry whose home is AFTER the hole must stay.
TEST(IdTable, EntryWhoseHomeIsAfterTheHoleStaysPut) {
    Table table{8};
    const OrderId a = ids_with_home(table, 5)[0];
    const OrderId b = ids_with_home(table, 6)[0];
    const OrderId c = ids_with_home(table, 5)[1];
    table.insert(a, 0); // at 5 (its home)
    table.insert(b, 1); // at 6 (its home)
    table.insert(c, 2); // home 5 is taken, 6 is taken: at 7
    table.erase(a);     // hole at 5. b (home 6) must NOT move to 5; c (home 5) must.
    EXPECT_EQ(table.find(b), 1U);
    EXPECT_EQ(table.find(c), 2U);
    EXPECT_TRUE(table.consistent());
}

TEST(IdTable, IdsTheTableSizeApartAreFine) {
    Table table{8};
    const auto size = static_cast<std::uint64_t>(table.capacity());
    // These would all collide under a plain "id % size"; the scramble spreads them out.
    for (std::uint32_t i = 0; i < 8; ++i) {
        table.insert(OrderId{3 + i * size}, i);
    }
    for (std::uint32_t i = 0; i < 8; ++i) {
        EXPECT_EQ(table.find(OrderId{3 + i * size}), i);
    }
    EXPECT_TRUE(table.consistent());
}

// The same idea as the differential fuzzer, at small scale: random inserts and erases, with
// std::unordered_map as the trusted answer key.
TEST(IdTable, AgreesWithUnorderedMapOnRandomOperations) {
    constexpr std::uint32_t kMaxOrders = 512;
    Table table{kMaxOrders};
    std::unordered_map<std::uint64_t, std::uint32_t> model;
    std::vector<std::uint64_t> present;
    std::vector<std::uint32_t> free_slots; // like the pool: every resting order has its own slot
    free_slots.reserve(kMaxOrders);
    for (std::uint32_t s = 0; s < kMaxOrders; ++s) {
        free_slots.push_back(s);
    }
    std::mt19937_64 rng{12345};
    std::uint64_t next_id = 1;

    for (int step = 0; step < 200'000; ++step) {
        const bool can_insert = model.size() < kMaxOrders;
        if (can_insert && (present.empty() || rng() % 100 < 55)) {
            next_id += 1 + rng() % 7; // increasing with gaps, like exchange ids
            const std::uint32_t slot = free_slots.back();
            free_slots.pop_back();
            table.insert(OrderId{next_id}, slot);
            model.emplace(next_id, slot);
            present.push_back(next_id);
        } else {
            const std::size_t pick = rng() % present.size();
            table.erase(OrderId{present[pick]});
            free_slots.push_back(model.at(present[pick]));
            model.erase(present[pick]);
            present[pick] = present.back();
            present.pop_back();
        }

        // Look up one id that is present and one random id (usually absent).
        if (!present.empty()) {
            const std::uint64_t id = present[rng() % present.size()];
            ASSERT_EQ(table.find(OrderId{id}), model.at(id)) << "step " << step;
        }
        const std::uint64_t any = 1 + rng() % (next_id + 10);
        const auto it = model.find(any);
        ASSERT_EQ(table.find(OrderId{any}), it == model.end() ? kNone : it->second)
            << "step " << step;
        ASSERT_EQ(table.size(), model.size());
        if (step % 1000 == 0) {
            ASSERT_TRUE(table.consistent()) << "step " << step;
        }
    }
}
