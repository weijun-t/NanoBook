/*
 * sizing.hpp (itch): turns per-stock counts into one BookConfig per stock, with safety margins.
 *
 * What it does: size_book() gives one stock's book its limits: room for its counted peak of
 *   resting orders times a margin, and a price band around its counted prices, widened by a
 *   margin. size_books() does that for every locate in today's directory, looking each symbol up
 *   in the stats; a symbol with no stats (a new listing, or a stock with no orders) gets a small
 *   default book that accepts any price. Locate 0 and gaps get a tiny book that is never used.
 *
 * Why it exists: Counting (day_stats.hpp) and sizing are separate on purpose. Today the stats come
 *   from the same day, with small margins. A real firm can't read tomorrow's file in advance: it
 *   would size today's books from yesterday's stats with bigger margins, and only these inputs
 *   change (ITCH design §7).
 *
 * How it works: Prices stay in ITCH's unit: 1 tick = $0.0001, so a Price(4) of 1,650,500 is
 *   Price{1650500}. max_qty is the largest share count an ITCH order can carry (a 4-byte field):
 *   a copy must accept whatever Nasdaq accepted, so it isn't a "fat finger" limit here. Every
 *   result passes BookConfig::is_valid().
 *
 * Trade-offs: If a stock outgrows its book anyway, the book answers BookFull (or InvalidPrice)
 *   and the book builder counts it: visible, never silent. Growing a book mid-day would allocate
 *   (rule 1), so that's left out.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include "nanobook/core/types.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/messages.hpp"

namespace nanobook::itch {

inline constexpr std::int64_t kMaxItchPrice = 2'000'000'000; // $200,000.0000 (the spec's maximum)
inline constexpr std::int64_t kMaxItchShares = std::numeric_limits<std::uint32_t>::max();

// Whole percentages, so the arithmetic is exact (1.1 has no exact double: 100100 * 1.1 rounds up
// to 110111, not 110110).
struct SizingRules {
    std::uint64_t order_percent{125};       // room for 125% of the counted peak...
    std::uint64_t extra_orders{16};         // ...plus this many
    std::uint64_t price_margin_percent{10}; // widen the counted price range by 10% each way
    std::uint32_t default_orders{64};       // a stock with no stats
};

// A book for a stock with no stats: a few orders, any price. Fine for FastBook (its memory
// follows the orders), but not for TickFastBook, whose memory follows the band: 2 billion
// prices x 32 bytes = 64 GB. Use the tick book only where every stock has stats.
inline BookConfig default_book(const SizingRules &rules) {
    return BookConfig{.min_price = Price{1},
                      .max_price = Price{kMaxItchPrice},
                      .max_orders = rules.default_orders,
                      .max_qty = Qty{kMaxItchShares}};
}

inline BookConfig size_book(const StockStats &stats, const SizingRules &rules) {
    if (stats.peak_orders == 0) {
        return default_book(rules);
    }
    // Rounded so the margin never shrinks: orders and the top price up, the bottom price down.
    const std::uint64_t orders =
        (std::uint64_t{stats.peak_orders} * rules.order_percent + 99) / 100 + rules.extra_orders;
    const std::uint64_t low =
        std::uint64_t{stats.min_price} * (100 - rules.price_margin_percent) / 100;
    const std::uint64_t high =
        (std::uint64_t{stats.max_price} * (100 + rules.price_margin_percent) + 99) / 100;
    constexpr std::uint64_t kMaxOrders = 1U << 30U; // far above any stock; keeps max_orders *
                                                    // max_qty inside BookConfig::is_valid()
    return BookConfig{.min_price = Price{std::max<std::int64_t>(1, static_cast<std::int64_t>(low))},
                      .max_price = Price{std::min(kMaxItchPrice, static_cast<std::int64_t>(high))},
                      .max_orders = static_cast<std::uint32_t>(std::min(orders, kMaxOrders)),
                      .max_qty = Qty{kMaxItchShares}};
}

// For locate 0 and gaps in the directory: never used, because the book builder treats a
// locate no R message named as unknown. As small as a valid book can be.
inline constexpr BookConfig kUnusedBook{
    .min_price = Price{1}, .max_price = Price{1}, .max_orders = 1, .max_qty = Qty{1}};

// One config per locate in `directory`: sized from the stock's stats, or the default book if the
// stock has none (a new listing), or kUnusedBook for locate 0 and gaps.
inline std::vector<BookConfig> size_books(const std::vector<Symbol> &directory,
                                          const std::map<Symbol, StockStats> &stats,
                                          const SizingRules &rules) {
    std::vector<BookConfig> configs(directory.size(), kUnusedBook);
    for (std::size_t locate = 1; locate < directory.size(); ++locate) {
        if (directory[locate] == Symbol{}) {
            continue;
        }
        const auto found = stats.find(directory[locate]);
        configs[locate] =
            found != stats.end() ? size_book(found->second, rules) : default_book(rules);
    }
    return configs;
}

} // namespace nanobook::itch
