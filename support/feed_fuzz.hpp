/*
 * feed_fuzz.hpp: random feed inputs (add, reduce, remove, replace) for testing the books' apply().
 *
 * What it does: random_feed() makes a seeded list of feed inputs; apply() runs one on a book.
 *   Ids are drawn from a small range, so many inputs hit a resting order and many don't: every
 *   FeedResult happens, from Ok to DuplicateOrder, ReduceTooLarge and BookFull. Buy and sell
 *   prices overlap, so the book often crosses, which a feed book must accept.
 *
 * Why it exists: The typed tests check each rule on its own. This checks them all mixed
 *   together, the way phase 2's fuzzer did for matching: the reference book and the fast books
 *   must give the same result and the same book after every input (tests/test_feed.cpp). The
 *   same list drives the zero-allocation test (tests/test_alloc.cpp).
 *
 * How it works: The inputs don't depend on the book's answers, so the whole list can be made
 *   before anything is timed or counted. Mix: 40% add, 25% reduce, 15% remove, 20% replace;
 *   about 1 in 100 has a quantity of 0 and 1 in 100 a price outside the band.
 *
 * Trade-offs: Test-only (it allocates the list). Ids aren't increasing, unlike a real feed's,
 *   which only makes it harder for the books.
 */

#pragma once

#include <cstdint>
#include <random>
#include <variant>
#include <vector>

#include "nanobook/core/messages.hpp"
#include "nanobook/core/types.hpp"

namespace nanobook::support {

using FeedInput = std::variant<FeedAdd, FeedReduce, FeedRemove, FeedReplace>;

struct FeedFuzzOptions {
    std::uint64_t seed{1};
    std::size_t count{100'000};
    std::uint64_t max_id{128}; // ids are 1 ... max_id
    std::int64_t low_price{95};
    std::int64_t high_price{105};
    std::int64_t band_max{1000}; // the book's max_price, so 1 in 100 can be just outside it
};

inline std::vector<FeedInput> random_feed(const FeedFuzzOptions &o) {
    std::mt19937_64 rng{o.seed};
    const auto pick = [&rng](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>{lo, hi}(rng);
    };
    const auto id = [&] {
        return OrderId{static_cast<std::uint64_t>(pick(1, static_cast<std::int64_t>(o.max_id)))};
    };
    const auto qty = [&] { return Qty{pick(1, 100) == 1 ? 0 : pick(1, 100)}; };
    const auto price = [&] {
        return Price{pick(1, 100) == 1 ? o.band_max + 1 : pick(o.low_price, o.high_price)};
    };

    std::vector<FeedInput> inputs;
    inputs.reserve(o.count);
    for (std::size_t i = 0; i < o.count; ++i) {
        const std::int64_t roll = pick(1, 100);
        if (roll <= 40) {
            inputs.emplace_back(FeedAdd{.id = id(),
                                        .side = pick(0, 1) == 0 ? Side::Buy : Side::Sell,
                                        .price = price(),
                                        .qty = qty()});
        } else if (roll <= 65) {
            inputs.emplace_back(FeedReduce{.id = id(), .qty = Qty{pick(0, 60)}});
        } else if (roll <= 80) {
            inputs.emplace_back(FeedRemove{.id = id()});
        } else {
            inputs.emplace_back(
                FeedReplace{.old_id = id(), .new_id = id(), .price = price(), .qty = qty()});
        }
    }
    return inputs;
}

template <typename Book> FeedResult apply(Book &book, const FeedInput &input) {
    return std::visit([&book](const auto &in) { return book.apply(in); }, input);
}

} // namespace nanobook::support
