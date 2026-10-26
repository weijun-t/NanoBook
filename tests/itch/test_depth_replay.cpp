/*
 * test_depth_replay.cpp: tests for one stock's replay behind nanobook_depth (depth design §5).
 *
 * What it does: Small ITCH days written with the test encoder, two stocks each, replayed for one
 *   of them. Checks that only the chosen stock's order messages reach its books; the L2 levels
 *   and the L3 queue (shares ahead, arrival time, attribution) after adds, partial executions,
 *   cancels, deletes and a replace (which loses its place); the tape and the last trade; step()
 *   and advance_to(); following an order to its fill or cancel; a broken message; and a planted
 *   difference between the two books stopping the replay at the exact message. The golden day
 *   ends with the books worked out by hand, and a busy day ends with the same book as a full
 *   replay of every stock. Last, ReplayClock with made-up real times.
 *
 * Why it exists: The screen only lays out what DepthReplay says; if DepthReplay is right, so is
 *   the picture of the book.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "depth_replay.hpp"
#include "itch_encoder.hpp"
#include "itch_golden_day.hpp"
#include "itch_replay_comparison.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"

using namespace nanobook;
using namespace nanobook::depth;
using namespace nanobook::itch;
using golden::add;
using golden::kAaa;
using golden::kBbb;
using golden::kNsdq;

namespace {

constexpr std::uint16_t kA = 1;         // AAA, the stock replayed
constexpr std::uint16_t kB = 2;         // BBB, the other one
constexpr std::uint32_t kTen = 100'000; // $10.0000

std::vector<BookConfig> configs_for(std::span<const std::byte> day, std::uint16_t locate) {
    DayCounter counter;
    EXPECT_TRUE(parse(day, counter).has_value());
    return one_stock_configs(counter.directory(), counter.stats(), locate);
}

std::vector<std::byte> copy(const Writer &w) {
    const std::span<const std::byte> bytes = w.bytes();
    return {bytes.begin(), bytes.end()};
}

// The start of every day here: both stocks named and trading.
Writer two_stocks() {
    Writer w;
    w.stock_directory({.locate = kA, .timestamp = 1, .stock = kAaa});
    w.stock_directory({.locate = kB, .timestamp = 1, .stock = kBbb});
    w.trading_action({.locate = kA, .timestamp = 2, .stock = kAaa, .trading_state = 'T'});
    return w;
}

// AAA: ref 1 buys 100 @ $10.00 (t 10), ref 2 buys 50 @ $10.00 attributed to NSDQ (t 11), ref 3
// buys 30 @ $9.99 (t 12), ref 4 sells 40 @ $10.01 (t 13). BBB: ref 9 buys 70 @ $10.00 (t 11).
Writer four_orders() {
    Writer w = two_stocks();
    w.add_order(add(10, kA, 1, 'B', 100, kTen));
    AddOrder f = add(11, kA, 2, 'B', 50, kTen);
    f.attribution = kNsdq;
    w.add_order_mpid(f);
    w.add_order(add(11, kB, 9, 'B', 70, kTen));
    w.add_order(add(12, kA, 3, 'B', 30, kTen - 100));
    w.add_order(add(13, kA, 4, 'S', 40, kTen + 100));
    return w;
}

struct Replay {
    std::vector<std::byte> day;
    DepthReplay replay;

    explicit Replay(const Writer &w, std::uint16_t locate = kA, ReplayOptions options = {})
        : day(copy(w)), replay(day, configs_for(day, locate), locate, options) {
    }
};

// The value, failing the test if there is none (and a default, so the test can go on).
template <typename T> T got(const std::optional<T> &value) {
    EXPECT_TRUE(value.has_value());
    return value.value_or(T{});
}

std::vector<std::uint64_t> ids(const std::vector<QueueRow> &rows) {
    std::vector<std::uint64_t> out;
    out.reserve(rows.size());
    for (const QueueRow &row : rows) {
        out.push_back(row.id.value);
    }
    return out;
}

} // namespace

TEST(DepthReplay, NamesAndSizing) {
    EXPECT_EQ(to_symbol("aapl"), (Symbol{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '}));
    EXPECT_EQ(to_symbol(""), std::nullopt);
    EXPECT_EQ(to_symbol("TOOLONGXX"), std::nullopt);
    const std::vector<Symbol> directory{Symbol{}, kAaa, kBbb};
    EXPECT_EQ(find_locate(directory, kBbb), kB);
    EXPECT_EQ(find_locate(directory, got(to_symbol("ZZZ"))), std::nullopt);

    Replay r{four_orders()};
    const std::vector<BookConfig> configs = configs_for(r.day, kA);
    ASSERT_EQ(configs.size(), 3U);
    EXPECT_GE(configs[kA].max_orders, 4U); // sized from AAA's counted peak
    EXPECT_EQ(configs[kB].max_orders, kUnusedBook.max_orders);
}

TEST(DepthReplay, OnlyTheChosenStockReachesItsBook) {
    Replay r{four_orders()};
    r.replay.advance_to(100);
    EXPECT_TRUE(r.replay.at_end());
    EXPECT_EQ(r.replay.messages(), 8U);       // every message of both stocks was parsed
    EXPECT_EQ(r.replay.stock_messages(), 4U); // BBB's add was not applied
    EXPECT_EQ(r.replay.book().order_count(), 4U);
    EXPECT_EQ(r.replay.trading_state(), 'T');
    EXPECT_FALSE(r.replay.mismatch().has_value());
    EXPECT_GT(r.replay.quick_checks(), 0U);
}

TEST(DepthReplay, LevelsAndQueue) {
    Replay r{four_orders()};
    r.replay.advance_to(100);

    const std::vector<LevelRow> bids = r.replay.levels(Side::Buy, 10);
    ASSERT_EQ(bids.size(), 2U);
    EXPECT_EQ(bids[0].price, Price{kTen});
    EXPECT_EQ(bids[0].shares, Qty{150});
    EXPECT_EQ(bids[0].orders, 2U);
    EXPECT_EQ(bids[1].price, Price{kTen - 100});
    EXPECT_EQ(bids[1].orders, 1U);
    EXPECT_EQ(r.replay.levels(Side::Buy, 1).size(), 1U);
    ASSERT_EQ(r.replay.levels(Side::Sell, 10).size(), 1U);

    const std::vector<QueueRow> queue = r.replay.queue(Side::Buy, Price{kTen});
    ASSERT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue[0].id, OrderId{1});
    EXPECT_EQ(queue[0].shares, Qty{100});
    EXPECT_EQ(queue[0].ahead, Qty{0});
    EXPECT_EQ(queue[0].since, 10U);
    EXPECT_EQ(queue[0].attribution, Mpid{});
    EXPECT_EQ(queue[1].id, OrderId{2});
    EXPECT_EQ(queue[1].ahead, Qty{100}); // ref 1's 100 shares go first
    EXPECT_EQ(queue[1].since, 11U);
    EXPECT_EQ(queue[1].attribution, kNsdq);
    EXPECT_TRUE(r.replay.queue(Side::Sell, Price{kTen}).empty());
}

TEST(DepthReplay, ExecutionsCancelsAndTheTape) {
    Writer w = four_orders();
    w.executed({.locate = kA, .timestamp = 20, .order_ref = 1, .executed_shares = 60});
    w.cancel({.locate = kB, .timestamp = 21, .order_ref = 9, .cancelled_shares = 70}); // BBB
    w.cancel({.locate = kA, .timestamp = 22, .order_ref = 1, .cancelled_shares = 40});
    w.executed_with_price({.locate = kA,
                           .timestamp = 23,
                           .order_ref = 4,
                           .executed_shares = 10,
                           .printable = 'Y',
                           .execution_price = kTen + 50});
    w.delete_order({.locate = kA, .timestamp = 24, .order_ref = 3});
    Replay r{w};

    r.replay.advance_to(20); // the partial execution
    std::vector<QueueRow> queue = r.replay.queue(Side::Buy, Price{kTen});
    ASSERT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue[0].shares, Qty{40});
    EXPECT_EQ(queue[1].ahead, Qty{40});
    ASSERT_TRUE(r.replay.last_trade().has_value());
    EXPECT_EQ(got(r.replay.last_trade()).price, Price{kTen}); // E has no price: the order's
    EXPECT_EQ(got(r.replay.last_trade()).shares, Qty{60});
    const TapeEntry &exec = r.replay.tape().front();
    EXPECT_EQ(exec.kind, TapeKind::Execute);
    EXPECT_EQ(exec.side, Side::Buy);
    EXPECT_EQ(exec.result, FeedResult::Ok);

    r.replay.advance_to(22); // ref 1's last 40 cancelled: ref 2 is at the front
    queue = r.replay.queue(Side::Buy, Price{kTen});
    ASSERT_EQ(ids(queue), (std::vector<std::uint64_t>{2}));
    EXPECT_EQ(queue[0].ahead, Qty{0});
    EXPECT_EQ(r.replay.find_note(OrderId{1}), nullptr); // gone from the book: note dropped
    EXPECT_EQ(r.replay.tape().front().kind, TapeKind::Cancel);
    EXPECT_EQ(r.replay.tape().front().price, Price{kTen});
    EXPECT_EQ(r.replay.tape().front().shares, Qty{40});

    r.replay.advance_to(24);
    EXPECT_EQ(got(r.replay.last_trade()).price, Price{kTen + 50}); // C carries its own price
    const TapeEntry &del = r.replay.tape().front();
    EXPECT_EQ(del.kind, TapeKind::Delete);
    EXPECT_EQ(del.shares, Qty{30}); // what was left
    EXPECT_EQ(del.price, Price{kTen - 100});

    // Newest first; BBB's cancel isn't on AAA's tape.
    std::vector<TapeKind> kinds;
    for (const TapeEntry &e : r.replay.tape()) {
        kinds.push_back(e.kind);
    }
    EXPECT_EQ(kinds, (std::vector<TapeKind>{TapeKind::Delete, TapeKind::Execute, TapeKind::Cancel,
                                            TapeKind::Execute, TapeKind::Add, TapeKind::Add,
                                            TapeKind::Add, TapeKind::Add}));
}

TEST(DepthReplay, AReplaceLosesItsPlace) {
    Writer w = four_orders();
    w.replace({.locate = kA,
               .timestamp = 30,
               .original_order_ref = 1,
               .new_order_ref = 5,
               .shares = 100,
               .price = kTen});
    Replay r{w};
    r.replay.advance_to(100);
    const std::vector<QueueRow> queue = r.replay.queue(Side::Buy, Price{kTen});
    EXPECT_EQ(ids(queue), (std::vector<std::uint64_t>{2, 5})); // same price, now at the back
    EXPECT_EQ(queue[1].since, 30U);
    EXPECT_EQ(queue[1].ahead, Qty{50});
    const TapeEntry &u = r.replay.tape().front();
    EXPECT_EQ(u.kind, TapeKind::Replace);
    EXPECT_EQ(u.id, OrderId{1});
    EXPECT_EQ(u.new_id, OrderId{5});
    EXPECT_EQ(u.side, Side::Buy);
}

TEST(DepthReplay, ResultsOfBadMessages) {
    Writer w = four_orders();
    w.executed({.locate = kA, .timestamp = 20, .order_ref = 99, .executed_shares = 5});
    w.add_order(add(21, kA, 7, 'Z', 5, kTen)); // a side that isn't B or S
    Replay r{w};
    r.replay.advance_to(20);
    EXPECT_EQ(r.replay.tape().front().result, FeedResult::UnknownOrder);
    EXPECT_FALSE(r.replay.last_trade().has_value());
    r.replay.advance_to(21);
    EXPECT_EQ(r.replay.tape().front().result, std::nullopt); // never reached a book
    EXPECT_FALSE(r.replay.mismatch().has_value());
}

TEST(DepthReplay, StepAppliesOneMessageOfTheStock) {
    Replay r{four_orders()};
    ASSERT_TRUE(r.replay.step());
    EXPECT_EQ(r.replay.stock_messages(), 1U);
    EXPECT_EQ(r.replay.itch_time(), 10U);
    ASSERT_TRUE(r.replay.step());
    EXPECT_EQ(r.replay.book().order_count(), 2U);
    ASSERT_TRUE(r.replay.step()); // passes BBB's add on the way
    EXPECT_EQ(r.replay.stock_messages(), 3U);
    EXPECT_EQ(r.replay.itch_time(), 12U);
    EXPECT_EQ(r.replay.next_time(), 13U);
    ASSERT_TRUE(r.replay.step());
    EXPECT_FALSE(r.replay.step()); // the end
    EXPECT_TRUE(r.replay.at_end());
}

TEST(DepthReplay, AdvanceStopsAtTheTime) {
    Replay r{four_orders()};
    EXPECT_EQ(r.replay.advance_to(9), 3U); // the two names and AAA's trading state
    EXPECT_EQ(r.replay.book().order_count(), 0U);
    EXPECT_EQ(r.replay.advance_to(11), 3U); // ref 1, ref 2, BBB's ref 9
    EXPECT_EQ(r.replay.book().order_count(), 2U);
    EXPECT_EQ(r.replay.advance_to(11), 0U);
}

TEST(DepthReplay, TheTapeIsCapped) {
    Replay r{four_orders(), kA, ReplayOptions{.tape = 3}};
    r.replay.advance_to(100);
    ASSERT_EQ(r.replay.tape().size(), 3U);
    EXPECT_EQ(r.replay.tape().front().id, OrderId{4});
    EXPECT_EQ(r.replay.tape().back().id, OrderId{2});
}

TEST(DepthReplay, FollowAnOrderToItsFill) {
    Writer w = four_orders();
    w.executed({.locate = kA, .timestamp = 20, .order_ref = 1, .executed_shares = 100});
    w.replace({.locate = kA,
               .timestamp = 21,
               .original_order_ref = 2,
               .new_order_ref = 5,
               .shares = 50,
               .price = kTen - 100});
    w.executed({.locate = kA, .timestamp = 22, .order_ref = 3, .executed_shares = 30});
    w.executed({.locate = kA, .timestamp = 23, .order_ref = 5, .executed_shares = 50});
    Replay r{w};
    r.replay.advance_to(13);
    EXPECT_FALSE(r.replay.follow_status().has_value());
    r.replay.follow(OrderId{2});

    std::optional<FollowStatus> s = r.replay.follow_status();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(got(s).state, FollowState::Resting);
    EXPECT_EQ(got(s).place, 2U);
    EXPECT_EQ(got(s).ahead, Qty{100});
    EXPECT_EQ(got(s).price, Price{kTen});

    r.replay.advance_to(20); // ref 1 filled: ref 2 moves to the front
    s = r.replay.follow_status();
    EXPECT_EQ(got(s).place, 1U);
    EXPECT_EQ(got(s).ahead, Qty{0});

    r.replay.advance_to(21); // replaced: follow the new order, at the back of $9.99
    s = r.replay.follow_status();
    EXPECT_EQ(got(s).id, OrderId{5});
    EXPECT_EQ(got(s).price, Price{kTen - 100});
    EXPECT_EQ(got(s).place, 2U);
    EXPECT_EQ(got(s).ahead, Qty{30});

    r.replay.advance_to(23);
    s = r.replay.follow_status();
    EXPECT_EQ(got(s).state, FollowState::Filled);
    EXPECT_EQ(got(s).id, OrderId{5});

    r.replay.unfollow();
    EXPECT_FALSE(r.replay.follow_status().has_value());
}

TEST(DepthReplay, FollowAnOrderToItsCancel) {
    Writer w = four_orders();
    w.cancel({.locate = kA, .timestamp = 20, .order_ref = 4, .cancelled_shares = 10}); // part
    w.delete_order({.locate = kA, .timestamp = 21, .order_ref = 4});
    Replay r{w};
    r.replay.advance_to(13);
    r.replay.follow(OrderId{4});
    r.replay.advance_to(20);
    EXPECT_EQ(got(r.replay.follow_status()).state, FollowState::Resting);
    EXPECT_EQ(got(r.replay.follow_status()).shares, Qty{30});
    r.replay.advance_to(21);
    EXPECT_EQ(got(r.replay.follow_status()).state, FollowState::Cancelled);

    r.replay.follow(OrderId{77}); // never existed
    EXPECT_EQ(got(r.replay.follow_status()).state, FollowState::NotResting);
}

TEST(DepthReplay, ABrokenMessageStopsTheReplay) {
    Writer w = four_orders();
    w.malformed(3, 'A');
    w.add_order(add(30, kA, 8, 'B', 1, kTen));
    const std::vector<std::byte> day = copy(w);
    // Sized from the good part: the counting pass stops at the broken message too.
    DepthReplay replay{day, configs_for(copy(four_orders()), kA), kA};
    replay.advance_to(100);
    EXPECT_FALSE(replay.running());
    ASSERT_TRUE(replay.error().has_value());
    EXPECT_EQ(got(replay.error()).kind, ParseErrorKind::WrongLength);
    EXPECT_EQ(replay.book().order_count(), 4U); // ref 8, after it, never applied
    EXPECT_FALSE(replay.step());
}

TEST(DepthReplay, ADifferenceBetweenTheBooksStopsAtItsMessage) {
    const std::vector<std::byte> day = copy(four_orders());
    const std::vector<BookConfig> fast = configs_for(day, kA);
    std::vector<BookConfig> reference = fast;
    reference[kA].max_orders = 2; // the reference's book is full after two orders
    DepthReplay replay{day, fast, reference, kA, ReplayOptions{}};
    replay.advance_to(100);
    EXPECT_FALSE(replay.running());
    ASSERT_TRUE(replay.mismatch().has_value());
    EXPECT_EQ(got(replay.mismatch()).timestamp, 12U); // ref 3, the third order
    EXPECT_EQ(replay.stock_messages(), 3U);           // stopped right after it
    EXPECT_FALSE(replay.at_end());
}

TEST(DepthReplay, GoldenDayEndsWithTheHandCheckedBook) {
    const std::vector<std::byte> day = golden::golden_day();
    DepthReplay replay{day, configs_for(day, kA), kA};
    replay.advance_to(1'000);
    EXPECT_TRUE(replay.at_end());
    EXPECT_FALSE(replay.mismatch().has_value());
    EXPECT_FALSE(replay.error().has_value());
    // golden_day.hpp: AAA buys [1: 80 @ 100'000], [4: 45 @ 100'050], no sells.
    const std::vector<LevelRow> bids = replay.levels(Side::Buy, 10);
    ASSERT_EQ(bids.size(), 2U);
    EXPECT_EQ(bids[0].price, Price{100'050});
    EXPECT_EQ(bids[0].shares, Qty{45});
    EXPECT_EQ(bids[1].price, Price{100'000});
    EXPECT_EQ(bids[1].shares, Qty{80});
    EXPECT_TRUE(replay.levels(Side::Sell, 10).empty());
    EXPECT_EQ(ids(replay.queue(Side::Buy, Price{100'000})), (std::vector<std::uint64_t>{1}));
}

TEST(DepthReplay, BusyDayEndsWithTheFullReplaysBook) {
    const std::vector<std::byte> day = golden::busy_day(4'000);
    DayCounter counter;
    ASSERT_TRUE(parse(day, counter).has_value());
    const std::vector<BookConfig> all =
        size_books(counter.directory(), counter.stats(), SizingRules{});
    BookBuilder<FastBook> full{all, BuilderOptions{}};
    ASSERT_TRUE(parse(day, full).has_value());

    for (std::uint16_t locate = 1; locate <= 3; ++locate) {
        DepthReplay replay{day, configs_for(day, locate), locate, ReplayOptions{.full_every = 1}};
        replay.advance_to(std::numeric_limits<std::uint64_t>::max());
        EXPECT_TRUE(replay.at_end());
        EXPECT_FALSE(replay.mismatch().has_value());
        EXPECT_GT(replay.book().order_count(), 0U);
        EXPECT_EQ(compare_books(replay.book(), full.book(locate)), std::nullopt) << locate;
    }
}

TEST(ReplayClock, RunsAtItsSpeed) {
    ReplayClock clock{1'000, 60, 5'000};
    EXPECT_EQ(clock.itch_now(5'000), 1'000U);
    EXPECT_EQ(clock.itch_now(4'000), 1'000U); // never before the start
    EXPECT_EQ(clock.itch_now(5'010), 1'600U);
}

TEST(ReplayClock, PauseFreezesAndResumeStartsFromTheReplay) {
    ReplayClock clock{0, 10, 0};
    clock.pause(100); // ITCH 1,000
    EXPECT_TRUE(clock.paused());
    EXPECT_EQ(clock.itch_now(500), 1'000U);
    clock.resume(600, 400); // the replay is behind the clock: keep the clock
    EXPECT_EQ(clock.itch_now(600), 1'000U);
    EXPECT_EQ(clock.itch_now(610), 1'100U);
    clock.pause(610);
    clock.resume(700, 5'000); // steps moved the replay ahead: start from there
    EXPECT_EQ(clock.itch_now(700), 5'000U);
    EXPECT_EQ(clock.itch_now(701), 5'010U);
}

TEST(ReplayClock, ANewSpeedDoesNotJump) {
    ReplayClock clock{0, 10, 0};
    clock.set_speed(100, 2); // at ITCH 1,000
    EXPECT_EQ(clock.itch_now(100), 1'000U);
    EXPECT_EQ(clock.itch_now(150), 1'100U);
    clock.set_speed(150, 1'000'000);
    EXPECT_EQ(clock.speed(), ReplayClock::kMaxSpeed);
    clock.set_speed(150, 0);
    EXPECT_EQ(clock.speed(), ReplayClock::kMinSpeed);
    clock.pause(150);
    clock.set_speed(200, 4); // while paused: still frozen
    EXPECT_EQ(clock.itch_now(300), 1'100U);
}
