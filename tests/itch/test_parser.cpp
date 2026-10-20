/*
 * test_parser.cpp: tests for the ITCH 5.0 parser (include/nanobook/itch/parser.hpp).
 *
 * What it does: Checks the field readers (big-endian 2, 4, 6 and 8 bytes), then decodes bytes
 *   written out by hand from the spec's tables: the first message of the real 30 Jan 2019 file,
 *   an Add Order and an Order Replace. Then a round trip: the test encoder
 *   (support/itch_encoder.hpp) writes every decoded type plus skipped ones, and the parser must
 *   give back exactly what went in, in order. Last, broken input: an unknown type, a wrong
 *   length, a zero length, an incomplete message at the end, and that nothing after an error is
 *   delivered.
 *
 * Why it exists: A field read at the wrong offset gives plausible but wrong numbers, and the
 *   book would silently drift from Nasdaq's. The hand-written bytes check the offsets against
 *   the spec independently of the encoder, which shares the parser's offsets (an offset wrong in
 *   both would still pass a round trip).
 *
 * How it works: Recorder is a Handler that stores every message it receives, so a test can
 *   compare the whole sequence.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "itch_encoder.hpp"
#include "nanobook/itch/parser.hpp"

using namespace nanobook::itch;

namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

Symbol symbol(std::string_view text) { // exactly 8 characters, padded with spaces
    Symbol s{};
    EXPECT_EQ(text.size(), s.size());
    std::ranges::copy(text.substr(0, s.size()), s.begin());
    return s;
}

// Stores everything parse() hands it.
struct Recorder {
    std::vector<SystemEvent> system_events;
    std::vector<StockDirectory> directories;
    std::vector<StockTradingAction> trading_actions;
    std::vector<AddOrder> adds;
    std::vector<OrderExecuted> executions;
    std::vector<OrderExecutedWithPrice> executions_with_price;
    std::vector<OrderCancel> cancels;
    std::vector<OrderDelete> deletes;
    std::vector<OrderReplace> replaces;
    std::string skipped;
    std::string order; // every type letter received, in order (A for A and F)

    void on(const SystemEvent &m) {
        system_events.push_back(m);
        order += 'S';
    }
    void on(const StockDirectory &m) {
        directories.push_back(m);
        order += 'R';
    }
    void on(const StockTradingAction &m) {
        trading_actions.push_back(m);
        order += 'H';
    }
    void on(const AddOrder &m) {
        adds.push_back(m);
        order += 'A';
    }
    void on(const OrderExecuted &m) {
        executions.push_back(m);
        order += 'E';
    }
    void on(const OrderExecutedWithPrice &m) {
        executions_with_price.push_back(m);
        order += 'C';
    }
    void on(const OrderCancel &m) {
        cancels.push_back(m);
        order += 'X';
    }
    void on(const OrderDelete &m) {
        deletes.push_back(m);
        order += 'D';
    }
    void on(const OrderReplace &m) {
        replaces.push_back(m);
        order += 'U';
    }
    void on_skipped(char type) {
        skipped += type;
        order += type;
    }
};

static_assert(Handler<Recorder>);

constexpr std::uint64_t kNineThirty = 34'200'000'000'000; // 09:30:00 in ns since midnight

} // namespace

// ---------------------------------------------------------------------------
// Field readers
// ---------------------------------------------------------------------------

TEST(ItchFields, ReadsBigEndian) {
    const auto b = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    EXPECT_EQ(read_be<std::uint16_t>(b.data()), 0x0102U);
    EXPECT_EQ(read_be<std::uint32_t>(b.data()), 0x01020304U);
    EXPECT_EQ(read_be<std::uint64_t>(b.data()), 0x0102030405060708ULL);
    EXPECT_EQ(read_be48(b.data()), 0x010203040506ULL);        // the 6-byte timestamp
    EXPECT_EQ(read_be<std::uint16_t>(b.data() + 6), 0x0708U); // any alignment works
}

TEST(ItchFields, LengthTableMatchesTheSpec) {
    EXPECT_EQ(kMessageLength['A'], 36);
    EXPECT_EQ(kMessageLength['F'], 40);
    EXPECT_EQ(kMessageLength['U'], 35);
    EXPECT_EQ(kMessageLength['D'], 19);
    EXPECT_EQ(kMessageLength['Z'], 0); // not a type
    int types = 0;
    for (const std::uint8_t length : kMessageLength) {
        types += length != 0 ? 1 : 0;
    }
    EXPECT_EQ(types, 23); // every ITCH 5.0 message type
}

// ---------------------------------------------------------------------------
// Bytes written by hand from the spec's tables (no encoder)
// ---------------------------------------------------------------------------

TEST(ItchParse, FirstMessageOfTheRealFile) {
    // The first 14 bytes of 01302019.NASDAQ_ITCH50 (design §3.1).
    const auto b = bytes({
        0x00, 0x0C,                         // length 12
        0x53,                               // S: System Event
        0x00, 0x00,                         // stock locate 0
        0x00, 0x00,                         // tracking number
        0x0A, 0x0A, 0x60, 0xAA, 0xDB, 0x93, // timestamp
        0x4F,                               // event code O: Start of Messages
    });
    Recorder r;
    ASSERT_EQ(parse(b, r), b.size());
    ASSERT_EQ(r.system_events.size(), 1U);
    EXPECT_EQ(r.system_events[0],
              (SystemEvent{.locate = 0, .timestamp = 11'039'687'760'787, .event_code = 'O'}));
    // 11,039.687760787 s after midnight = 03:03:59.687760787, as the census found.
}

TEST(ItchParse, AddOrderAtTheSpecOffsets) {
    const auto b = bytes({
        0x00, 0x24,                                     // length 36
        0x41,                                           // A
        0x00, 0x0E,                                     // stock locate 14
        0x00, 0x00,                                     // tracking number
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x7B,             // timestamp 09:30:00.000000123
        0x00, 0x00, 0x01, 0x1F, 0x71, 0xFB, 0x04, 0xCB, // order ref 1,234,567,890,123
        0x42,                                           // side B
        0x00, 0x00, 0x00, 0x64,                         // shares 100
        0x41, 0x41, 0x50, 0x4C, 0x20, 0x20, 0x20, 0x20, // stock "AAPL    "
        0x00, 0x19, 0x2F, 0x44,                         // price 1,650,500 = $165.0500
    });
    Recorder r;
    ASSERT_EQ(parse(b, r), b.size());
    ASSERT_EQ(r.adds.size(), 1U);
    EXPECT_EQ(r.adds[0], (AddOrder{.locate = 14,
                                   .timestamp = kNineThirty + 123,
                                   .order_ref = 1'234'567'890'123,
                                   .side = 'B',
                                   .shares = 100,
                                   .stock = symbol("AAPL    "),
                                   .price = 1'650'500,
                                   .attribution = {}}));
}

TEST(ItchParse, OrderReplaceAtTheSpecOffsets) {
    const auto b = bytes({
        0x00, 0x23,                                     // length 35
        0x55,                                           // U
        0x00, 0x0E,                                     // stock locate 14
        0x00, 0x00,                                     // tracking number
        0x1F, 0x1A, 0xCE, 0xD9, 0xF1, 0xC8,             // timestamp 09:30:00.000000456
        0x00, 0x00, 0x01, 0x1F, 0x71, 0xFB, 0x04, 0xCB, // original order ref 1,234,567,890,123
        0x00, 0x00, 0x01, 0x1F, 0x71, 0xFB, 0x08, 0x37, // new order ref 1,234,567,890,999
        0x00, 0x00, 0x01, 0x2C,                         // shares 300
        0x00, 0x19, 0x2E, 0xE0,                         // price 1,650,400 = $165.0400
    });
    Recorder r;
    ASSERT_EQ(parse(b, r), b.size());
    ASSERT_EQ(r.replaces.size(), 1U);
    EXPECT_EQ(r.replaces[0], (OrderReplace{.locate = 14,
                                           .timestamp = kNineThirty + 456,
                                           .original_order_ref = 1'234'567'890'123,
                                           .new_order_ref = 1'234'567'890'999,
                                           .shares = 300,
                                           .price = 1'650'400}));
}

// ---------------------------------------------------------------------------
// Round trip through the test encoder
// ---------------------------------------------------------------------------

TEST(ItchParse, RoundTripOfEveryDecodedType) {
    const SystemEvent s{.locate = 0, .timestamp = 1, .event_code = 'Q'};
    const StockDirectory dir{.locate = 14, .timestamp = 2, .stock = symbol("AAPL    ")};
    const StockTradingAction act{
        .locate = 14, .timestamp = 3, .stock = symbol("AAPL    "), .trading_state = 'T'};
    const AddOrder a{.locate = 14,
                     .timestamp = 4,
                     .order_ref = 7,
                     .side = 'S',
                     .shares = 4'000'000'000, // near the top of uint32: no sign or width slip
                     .stock = symbol("AAPL    "),
                     .price = 2'000'000'000, // the spec's maximum price, $200,000.0000
                     .attribution = {}};
    const AddOrder f{.locate = 8713, // the highest locate in the real file
                     .timestamp = 5,
                     .order_ref = 0xFFFF'FFFF'FFFF'FFFF,
                     .side = 'B',
                     .shares = 1,
                     .stock = symbol("ZYXI    "),
                     .price = 1,
                     .attribution = {'N', 'S', 'D', 'Q'}};
    const OrderExecuted e{
        .locate = 14, .timestamp = 6, .order_ref = 7, .executed_shares = 30, .match_number = 99};
    const OrderExecutedWithPrice c{.locate = 14,
                                   .timestamp = 7,
                                   .order_ref = 7,
                                   .executed_shares = 10,
                                   .match_number = 100,
                                   .printable = 'Y',
                                   .execution_price = 1'650'000};
    const OrderCancel x{.locate = 14, .timestamp = 8, .order_ref = 7, .cancelled_shares = 5};
    const OrderReplace u{.locate = 14,
                         .timestamp = 9,
                         .original_order_ref = 7,
                         .new_order_ref = 8,
                         .shares = 50,
                         .price = 1'649'900};
    const OrderDelete d{.locate = 14, .timestamp = 0xFFFF'FFFF'FFFF, .order_ref = 8}; // 48 bits

    Writer w;
    w.system_event(s);
    w.stock_directory(dir);
    w.trading_action(act);
    w.add_order(a);
    w.skipped('P');
    w.add_order_mpid(f);
    w.executed(e);
    w.executed_with_price(c);
    w.skipped('I');
    w.cancel(x);
    w.replace(u);
    w.delete_order(d);

    Recorder r;
    ASSERT_EQ(parse(w.bytes(), r), w.bytes().size());
    EXPECT_EQ(r.order, "SRHAPAECIXUD");
    EXPECT_EQ(r.system_events, std::vector{s});
    EXPECT_EQ(r.directories, std::vector{dir});
    EXPECT_EQ(r.trading_actions, std::vector{act});
    EXPECT_EQ(r.adds, (std::vector{a, f}));
    EXPECT_EQ(r.executions, std::vector{e});
    EXPECT_EQ(r.executions_with_price, std::vector{c});
    EXPECT_EQ(r.cancels, std::vector{x});
    EXPECT_EQ(r.replaces, std::vector{u});
    EXPECT_EQ(r.deletes, std::vector{d});
    EXPECT_EQ(r.skipped, "PI");
}

TEST(ItchParse, SkipsEveryTypeItDoesNotDecode) {
    Writer w;
    const std::string others = "YLVWKJhPQBINO";
    for (const char type : others) {
        w.skipped(type);
    }
    Recorder r;
    ASSERT_EQ(parse(w.bytes(), r), w.bytes().size());
    EXPECT_EQ(r.skipped, others);
}

// ---------------------------------------------------------------------------
// Broken and incomplete input
// ---------------------------------------------------------------------------

TEST(ItchParse, EmptyAndTinyBuffersConsumeNothing) {
    Recorder r;
    EXPECT_EQ(parse(std::span<const std::byte>{}, r), 0U);
    const auto one = bytes({0x00});
    EXPECT_EQ(parse(one, r), 0U); // half a length prefix
    EXPECT_EQ(r.order, "");
}

TEST(ItchParse, StopsBeforeAnIncompleteMessageAtTheEnd) {
    Writer w;
    w.delete_order(OrderDelete{.locate = 1, .timestamp = 1, .order_ref = 1});
    const std::size_t whole = w.bytes().size(); // 2 + 19
    w.delete_order(OrderDelete{.locate = 1, .timestamp = 2, .order_ref = 2});

    // Every cut inside the second message: the first is delivered, the second isn't.
    for (std::size_t cut = whole; cut < w.bytes().size(); ++cut) {
        Recorder r;
        EXPECT_EQ(parse(w.bytes().first(cut), r), whole) << "cut at " << cut;
        EXPECT_EQ(r.deletes.size(), 1U) << "cut at " << cut;
    }
}

TEST(ItchParse, RejectsAnUnknownType) {
    Writer w;
    w.delete_order(OrderDelete{.locate = 1, .timestamp = 1, .order_ref = 1});
    w.malformed(19, 'Z');
    Recorder r;
    EXPECT_EQ(parse(w.bytes(), r),
              std::unexpected(ParseError{
                  .kind = ParseErrorKind::UnknownType, .offset = 21, .type = 'Z', .length = 19}));
    EXPECT_EQ(r.order, "D"); // the message before the error was delivered
}

TEST(ItchParse, RejectsAWrongLength) {
    Writer w;
    w.malformed(35, 'A'); // an Add Order is 36 bytes
    Recorder r;
    EXPECT_EQ(parse(w.bytes(), r),
              std::unexpected(ParseError{
                  .kind = ParseErrorKind::WrongLength, .offset = 0, .type = 'A', .length = 35}));
    EXPECT_EQ(r.order, "");
}

TEST(ItchParse, RejectsAZeroLength) {
    Writer w;
    w.malformed(0, '\0');
    Recorder r;
    EXPECT_EQ(parse(w.bytes(), r),
              std::unexpected(ParseError{
                  .kind = ParseErrorKind::WrongLength, .offset = 0, .type = '\0', .length = 0}));
}

TEST(ItchParse, NothingAfterAnErrorIsDelivered) {
    Writer w;
    w.malformed(20, 'D'); // a Delete is 19 bytes
    w.delete_order(OrderDelete{.locate = 1, .timestamp = 1, .order_ref = 1});
    Recorder r;
    EXPECT_FALSE(parse(w.bytes(), r).has_value());
    EXPECT_EQ(r.order, "");
}
