/*
 * watchlist.hpp: what nanobook_view shows, worked out from the L1 table, with no terminal code.
 *
 * What it does: Watchlist::refresh() reads every named stock's slot from the table (l1_table.hpp)
 *   and, once a second, works out each stock's updates per second, the whole run's updates per
 *   second, the replay speed (ITCH seconds per real second) and the end-to-end latency of the
 *   last second's paced updates. rows() lists the stocks, filtered by a symbol prefix and sorted
 *   by activity, symbol or spread, pinned stocks first. Small helpers format prices and times.
 *
 * Why it exists: Viewer design §6. Keeping the numbers apart from the drawing means the tests can
 *   check them without a terminal, and the FTXUI part (view_render.hpp) only lays text out.
 *
 * How it works:
 *   - refresh(): O(stocks) per call, about 9,000 slots (only locates with a name), each one
 *     try_read(); a slot the writer is in the middle of keeps its previous value this time.
 *   - Rates over a 1 s window: at the start of each window it remembers each stock's update
 *     count, the run's count, the ITCH time and the latency buckets; a second later the
 *     differences divided by the real time between give the rates. At 10 redraws a second the
 *     rates change once a second, which is easier to read than ten noisy values.
 *   - Latency: the bucket counts' difference over the window; the p-th percentile is the bucket
 *     holding the sample at rank ceil(p x N), reported as that bucket's lower bound
 *     (latency::bucket_low(): at most 1/64, about 1.6%, below the true value), ticks to ns with
 *     the table's ticks_per_second. Integer ranks, as in latency_recorder.hpp.
 *   - A new run (run_id changed) starts everything over. Names are read only once the table says
 *     Running or Done (they are written before Running).
 *
 * Data structures: one Stock per locate (65,536, so a locate is the index), the list of named
 *   locates, their names (copied once per run), a pin flag per locate, and two copies of the
 * latency buckets (this window's start and a scratch copy).
 *
 * Trade-offs: Viewer-side code: it allocates and uses floating point freely (rates and the spread
 *   in basis points are for display; prices stay integers until they are formatted). Reading
 *   ~9,000 slots ten times a second pulls about 570 KiB of the publisher's cache lines to the
 *   viewer's core each time, the cost viewer design §4 measures.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "l1_table.hpp"
#include "latency_recorder.hpp"

namespace nanobook::view {

enum class SortBy { Activity, Symbol, Spread };

struct LatencyWindow {
    std::uint64_t count{}; // paced updates in the window
    double p50_ns{};
    double p99_ns{};
    double p999_ns{};

    bool operator==(const LatencyWindow &) const = default;
};

struct Health {
    RunState state{RunState::Starting};
    std::uint64_t updates{};   // in the whole run
    std::uint64_t itch_time{}; // the last update's ITCH time
    double updates_per_second{};
    double speed{};    // ITCH seconds per real second over the last window
    bool alive{false}; // updates moved in the last window
    bool paced{false}; // the table has a clock rate: a --speed run, so latency means something
    std::optional<LatencyWindow> latency; // the last window's, if it had paced updates
};

struct Row {
    std::uint16_t locate{};
    itch::Symbol symbol{};
    Quote quote{};
    double updates_per_second{};
    bool pinned{false};
};

// The symbol without ITCH's padding spaces.
inline std::string_view trimmed(const itch::Symbol &s) {
    const std::string_view text{s.data(), s.size()};
    const std::size_t end = text.find_last_not_of(" \0", std::string_view::npos, 2);
    return end == std::string_view::npos ? std::string_view{} : text.substr(0, end + 1);
}

// 1,650,500 (1/10,000 of a dollar) -> "165.0500"; 0 (an empty side) -> "-".
inline std::string format_price(std::int64_t price) {
    if (price <= 0) {
        return "-";
    }
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%lld.%04lld", static_cast<long long>(price / 10'000),
                  static_cast<long long>(price % 10'000));
    return text.data();
}

// Nanoseconds since midnight -> "09:31:04.123".
inline std::string format_time(std::uint64_t ns) {
    const std::uint64_t ms = ns / 1'000'000;
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%02llu:%02llu:%02llu.%03llu",
                  static_cast<unsigned long long>(ms / 3'600'000),
                  static_cast<unsigned long long>(ms / 60'000 % 60),
                  static_cast<unsigned long long>(ms / 1'000 % 60),
                  static_cast<unsigned long long>(ms % 1'000));
    return text.data();
}

[[nodiscard]] inline bool two_sided(const Quote &q) {
    return q.bid_price > 0 && q.ask_price > 0;
}
[[nodiscard]] inline bool crossed(const Quote &q) { // possible in auctions, or a broken book
    return two_sided(q) && q.bid_price >= q.ask_price;
}
// Ask minus bid over the mid price, in basis points (1/100 of a percent). Display only.
[[nodiscard]] inline std::optional<double> spread_bps(const Quote &q) {
    if (!two_sided(q)) {
        return std::nullopt;
    }
    return static_cast<double>(q.ask_price - q.bid_price) * 20'000.0 /
           static_cast<double>(q.ask_price + q.bid_price);
}

// The latency percentiles of `counts` (one count per bucket), or nothing if they are all 0.
[[nodiscard]] inline std::optional<LatencyWindow>
summarize_buckets(std::span<const std::uint64_t> counts, std::uint64_t ticks_per_second) {
    std::uint64_t total = 0;
    for (const std::uint64_t c : counts) {
        total += c;
    }
    if (total == 0 || ticks_per_second == 0) {
        return std::nullopt;
    }
    // The bucket holding the sample at rank ceil(p x total), p in millionths, as ns.
    const auto at = [&](std::uint64_t p_millionths) {
        const std::uint64_t rank = std::max<std::uint64_t>(1, (total * p_millionths + 999'999) /
                                                                  1'000'000); // ceil, in integers
        std::uint64_t seen = 0;
        std::size_t b = 0;
        while (b + 1 < counts.size() && seen + counts[b] < rank) {
            seen += counts[b];
            ++b;
        }
        return static_cast<double>(latency::bucket_low(b)) * 1e9 /
               static_cast<double>(ticks_per_second);
    };
    return LatencyWindow{
        .count = total, .p50_ns = at(500'000), .p99_ns = at(990'000), .p999_ns = at(999'000)};
}

class Watchlist {
public:
    Watchlist() : stocks_(kSlots), pinned_(kSlots, false) {
    }

    // Reads the table. `now_s`: seconds on any steady clock (the tests pass their own).
    void refresh(const L1Table &table, double now_s) {
        const std::uint64_t run = table.header.run_id.load(std::memory_order_relaxed);
        const RunState state = view::state(table);
        if (run != run_id_) {
            start_over(run);
        }
        health_.state = state;
        if (state == RunState::Starting) {
            return; // the names aren't written yet
        }
        if (!named_) {
            for (std::size_t locate = 1; locate < kSlots; ++locate) {
                if (!trimmed(table.symbols[locate]).empty()) {
                    listed_.push_back(static_cast<std::uint16_t>(locate));
                    names_[locate] = table.symbols[locate]; // fixed for the run: copied once
                }
            }
            named_ = true;
        }
        for (const std::uint16_t locate : listed_) {
            if (const auto q = try_read(table.slots[locate])) {
                stocks_[locate].quote = *q;
            }
        }
        health_.updates = table.header.updates.load(std::memory_order_relaxed);
        health_.itch_time = table.header.itch_time.load(std::memory_order_relaxed);
        const std::uint64_t ticks_per_second =
            table.header.ticks_per_second.load(std::memory_order_relaxed);
        health_.paced = ticks_per_second != 0;
        for (std::size_t b = 0; b < kLatencyBuckets; ++b) {
            scratch_[b] = table.latency[b].load(std::memory_order_relaxed);
        }

        if (!window_open_) {
            open_window(now_s);
            return;
        }
        const double seconds = now_s - window_start_;
        if (seconds < 1.0) {
            return;
        }
        for (const std::uint16_t locate : listed_) {
            Stock &s = stocks_[locate];
            s.updates_per_second =
                static_cast<double>(s.quote.updates - s.window_updates) / seconds;
        }
        health_.updates_per_second =
            static_cast<double>(health_.updates - window_.updates) / seconds;
        health_.speed =
            health_.itch_time > window_.itch_time
                ? static_cast<double>(health_.itch_time - window_.itch_time) / 1e9 / seconds
                : 0.0;
        health_.alive = health_.updates != window_.updates;
        for (std::size_t b = 0; b < kLatencyBuckets; ++b) {
            window_.latency[b] = scratch_[b] - window_.latency[b]; // this window's counts
        }
        health_.latency = summarize_buckets(window_.latency, ticks_per_second);
        open_window(now_s);
    }

    [[nodiscard]] const Health &health() const {
        return health_;
    }

    // The stocks whose symbol starts with `filter` (any case), pinned ones first, then by `sort`.
    [[nodiscard]] std::vector<Row> rows(std::string_view filter, SortBy sort) const {
        std::vector<Row> out;
        out.reserve(listed_.size());
        for (const std::uint16_t locate : listed_) {
            const Stock &s = stocks_[locate];
            if (!starts_with_any_case(trimmed(names_[locate]), filter)) {
                continue;
            }
            out.push_back(Row{.locate = locate,
                              .symbol = names_[locate],
                              .quote = s.quote,
                              .updates_per_second = s.updates_per_second,
                              .pinned = pinned_[locate]});
        }
        const auto by_sort = [sort](const Row &a, const Row &b) {
            if (a.pinned != b.pinned) {
                return a.pinned;
            }
            if (sort == SortBy::Activity) {
                if (a.updates_per_second != b.updates_per_second) {
                    return a.updates_per_second > b.updates_per_second;
                }
                if (a.quote.updates != b.quote.updates) {
                    return a.quote.updates > b.quote.updates;
                }
            } else if (sort == SortBy::Spread) {
                const auto sa = spread_bps(a.quote);
                const auto sb = spread_bps(b.quote);
                if (sa.has_value() != sb.has_value()) {
                    return sa.has_value(); // one-sided and empty books last
                }
                if (sa && *sa != *sb) {
                    return *sa < *sb;
                }
            }
            return trimmed(a.symbol) < trimmed(b.symbol);
        };
        std::ranges::sort(out, by_sort);
        return out;
    }

    void toggle_pin(std::uint16_t locate) {
        pinned_[locate] = !pinned_[locate];
    }

private:
    struct Stock {
        Quote quote{};
        std::uint64_t window_updates{}; // quote.updates when the window opened
        double updates_per_second{};
    };
    struct Window {
        std::uint64_t updates{};
        std::uint64_t itch_time{};
        std::vector<std::uint64_t> latency = std::vector<std::uint64_t>(kLatencyBuckets);
    };

    void start_over(std::uint64_t run) {
        run_id_ = run;
        named_ = false;
        window_open_ = false;
        listed_.clear();
        std::ranges::fill(stocks_, Stock{});
        std::ranges::fill(names_, itch::Symbol{});
        pinned_.assign(kSlots, false); // locates belong to one day's directory
        health_ = Health{};
    }

    void open_window(double now_s) {
        window_start_ = now_s;
        window_.updates = health_.updates;
        window_.itch_time = health_.itch_time;
        window_.latency = scratch_;
        for (const std::uint16_t locate : listed_) {
            stocks_[locate].window_updates = stocks_[locate].quote.updates;
        }
        window_open_ = true;
    }

    static bool starts_with_any_case(std::string_view text, std::string_view prefix) {
        if (prefix.size() > text.size()) {
            return false;
        }
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            const auto upper = [](char c) {
                return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            };
            if (upper(text[i]) != upper(prefix[i])) {
                return false;
            }
        }
        return true;
    }

    std::uint64_t run_id_{0};
    bool named_{false};
    bool window_open_{false};
    double window_start_{0};
    std::vector<Stock> stocks_;
    std::vector<std::uint16_t> listed_; // locates with a name, in locate order
    std::vector<bool> pinned_;
    std::vector<itch::Symbol> names_ = std::vector<itch::Symbol>(kSlots); // by locate
    std::vector<std::uint64_t> scratch_ = std::vector<std::uint64_t>(kLatencyBuckets);
    Window window_;
    Health health_;
};

} // namespace nanobook::view
