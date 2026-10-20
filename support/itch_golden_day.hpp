/*
 * itch_golden_day.hpp: a small made-up ITCH day whose every outcome is worked out by hand.
 *
 * What it does: golden_day() writes the day's bytes with the test encoder (itch_encoder.hpp):
 *   two stocks, every kind of message, deliberate errors, and two crossed moments. The comment
 *   above it says what the books must look like at the end. add() builds an AddOrder in one line.
 *
 * Why it exists: The golden replay test (tests/itch/test_book_builder.cpp, ITCH design §8.3)
 *   checks every number it produces, and the differential replay test
 *   (tests/itch/test_replay_comparison.cpp) runs it through two books at once. One copy of the
 *   day, so the two tests can't drift apart.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "itch_encoder.hpp"
#include "nanobook/itch/messages.hpp"

namespace nanobook::itch::golden {

inline constexpr Symbol kAaa{'A', 'A', 'A', ' ', ' ', ' ', ' ', ' '};
inline constexpr Symbol kBbb{'B', 'B', 'B', ' ', ' ', ' ', ' ', ' '};
inline constexpr Mpid kNsdq{'N', 'S', 'D', 'Q'};

// Small builders: one per message, in "ref, shares @ price" order. t is the timestamp.
inline AddOrder add(std::uint64_t t, std::uint16_t locate, std::uint64_t ref, char side,
                    std::uint32_t shares, std::uint32_t price) {
    return AddOrder{.locate = locate,
                    .timestamp = t,
                    .order_ref = ref,
                    .side = side,
                    .shares = shares,
                    .stock = {},
                    .price = price,
                    .attribution = {}};
}

// The golden day. Prices in ITCH units ($0.0001): 100'000 = $10.0000.
//   AAA (locate 1): ref 1 buys 100 @ $10.00, ref 2 buys 50 @ $10.00, ref 3 sells 30 @ $10.01 (F).
//     ref 3 executes 10; ref 1 executes 20 (C); ref 2 cancels 5; ref 2 is replaced by ref 4
//     (45 @ $10.005, still a buy); ref 3 is deleted.
//   BBB (locate 2): ref 5 sells 10 @ $20.00; ref 6 buys 10 @ $20.01 (crossed!); ref 6 deleted.
//     Then BBB is halted, ref 8 buys 5 @ $20.00 (crossed again: bid == ask, t = 25), deleted.
//   Errors: execute of unknown ref 99, cancel of 1000 from ref 1 (80 left), ref 1 added again,
//     an add for locate 9 (no such stock).
//   Final books: AAA buys [1: 80 @ 100'000], [4: 45 @ 100'050], no sells. BBB sells [5: 10].
inline std::vector<std::byte> golden_day() {
    Writer w;
    std::uint64_t t = 0;
    w.system_event({.locate = 0, .timestamp = ++t, .event_code = 'O'});
    w.stock_directory({.locate = 1, .timestamp = ++t, .stock = kAaa});
    w.stock_directory({.locate = 2, .timestamp = ++t, .stock = kBbb});
    w.system_event({.locate = 0, .timestamp = ++t, .event_code = 'S'});
    w.trading_action({.locate = 1, .timestamp = ++t, .stock = kAaa, .trading_state = 'T'});
    w.trading_action({.locate = 2, .timestamp = ++t, .stock = kBbb, .trading_state = 'T'});
    w.system_event({.locate = 0, .timestamp = ++t, .event_code = 'Q'}); // market open: t = 7

    w.add_order(add(++t, 1, 1, 'B', 100, 100'000));
    w.add_order(add(++t, 1, 2, 'B', 50, 100'000));
    AddOrder f = add(++t, 1, 3, 'S', 30, 100'100);
    f.attribution = kNsdq;
    w.add_order_mpid(f);
    w.executed(
        {.locate = 1, .timestamp = ++t, .order_ref = 3, .executed_shares = 10, .match_number = 1});
    w.executed_with_price({.locate = 1,
                           .timestamp = ++t,
                           .order_ref = 1,
                           .executed_shares = 20,
                           .match_number = 2,
                           .printable = 'Y',
                           .execution_price = 100'000});
    w.cancel({.locate = 1, .timestamp = ++t, .order_ref = 2, .cancelled_shares = 5});
    w.replace({.locate = 1,
               .timestamp = ++t,
               .original_order_ref = 2,
               .new_order_ref = 4,
               .shares = 45,
               .price = 100'050});
    w.delete_order({.locate = 1, .timestamp = ++t, .order_ref = 3});

    w.add_order(add(++t, 2, 5, 'S', 10, 200'000));
    w.add_order(add(++t, 2, 6, 'B', 10, 200'100)); // crosses: t = 17
    w.delete_order({.locate = 2, .timestamp = ++t, .order_ref = 6});

    w.executed({.locate = 1,
                .timestamp = ++t,
                .order_ref = 99,
                .executed_shares = 5,
                .match_number = 3});                                                     // unknown
    w.cancel({.locate = 1, .timestamp = ++t, .order_ref = 1, .cancelled_shares = 1000}); // too much
    w.add_order(add(++t, 1, 1, 'B', 1, 100'000));                      // duplicate
    w.add_order(add(++t, 9, 7, 'B', 1, 100'000));                      // no stock 9
    w.stock_directory({.locate = 1, .timestamp = ++t, .stock = kAaa}); // re-sent
    w.skipped('P');
    w.skipped('I');

    w.trading_action({.locate = 2, .timestamp = ++t, .stock = kBbb, .trading_state = 'H'});
    w.add_order(add(++t, 2, 8, 'B', 5, 200'000)); // locked (bid == ask) while halted: t = 25
    w.delete_order({.locate = 2, .timestamp = ++t, .order_ref = 8});
    w.system_event({.locate = 0, .timestamp = ++t, .event_code = 'M'});
    w.system_event({.locate = 0, .timestamp = ++t, .event_code = 'E'});
    w.system_event({.locate = 0, .timestamp = ++t, .event_code = 'C'});
    const std::span<const std::byte> bytes = w.bytes();
    return {bytes.begin(), bytes.end()};
}

} // namespace nanobook::itch::golden
