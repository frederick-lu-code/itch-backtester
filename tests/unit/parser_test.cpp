#include "support/collector.hpp"
#include "support/itch_writer.hpp"

#include "itchbt/itch/parser.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace itchbt::itch {
namespace {

using test::Collector;
using test::ItchWriter;

template <typename... Ts>
std::array<std::byte, sizeof...(Ts)> bytes_of(Ts... values) {
    return {static_cast<std::byte>(values)...};
}

constexpr Timestamp kOpen = 34'200'000'000'000;

static_assert(std::is_trivially_copyable_v<AddOrder>);
static_assert(std::is_trivially_copyable_v<StockDirectory>);
static_assert(std::is_trivially_copyable_v<CrossTrade>);

TEST(Parser, LengthTableMatchesTheSpecification) {
    // Independent copy of the official lengths; the writer has its own third copy.
    const std::vector<std::pair<char, std::size_t>> official = {
        {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
        {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
        {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
    std::size_t defined = 0;
    for (int c = 0; c < 256; ++c) {
        defined += message_length(static_cast<char>(c)) != 0 ? 1U : 0U;
    }
    EXPECT_EQ(defined, official.size());
    for (const auto& [type, length] : official) {
        EXPECT_EQ(message_length(type), length) << "type " << type;
        EXPECT_EQ(test::message_length(type), length) << "type " << type;
    }
}

TEST(Parser, DecodesAddOrderFromHandComputedBytes) {
    const auto stream = bytes_of(0x00, 0x24,                         // record length: 36
                                 'A',                                // message type
                                 0x00, 0x07,                         // stock locate
                                 0x01, 0x02,                         // tracking number
                                 0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00, // timestamp: 09:30:00
                                 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // order reference
                                 'B',                                            // buy/sell
                                 0x00, 0x00, 0x01, 0x2C,                         // shares: 300
                                 'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',         // stock
                                 0x00, 0x12, 0xD6, 0x44);                        // price: 123.4500
    Collector c;
    const ParseResult r = parse(stream, c);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.messages, 1U);
    EXPECT_EQ(r.offset, stream.size());
    ASSERT_EQ(c.adds.size(), 1U);
    const AddOrder& a = c.adds.front();
    EXPECT_EQ(a.h.type, 'A');
    EXPECT_EQ(a.h.locate, 7);
    EXPECT_EQ(a.h.tracking, 0x0102);
    EXPECT_EQ(a.h.ts, kOpen);
    EXPECT_EQ(a.ref, 0x0102030405060708ULL);
    EXPECT_EQ(a.side, Side::Buy);
    EXPECT_EQ(a.qty, 300U);
    EXPECT_EQ(a.symbol.view(), "AAPL");
    EXPECT_EQ(a.price, 1'234'500U);
}

TEST(Parser, DecodesOrderExecutedFromHandComputedBytes) {
    const auto stream =
        bytes_of(0x00, 0x1F,                                      // record length: 31
                 'E', 0x12, 0x34, 0x00, 0x00,                     // type, locate 0x1234, tracking
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,              // timestamp: 42 ns
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8,  // order reference: 1000
                 0x00, 0x00, 0x00, 0x64,                          // executed shares: 100
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00); // match number: 65536
    Collector c;
    ASSERT_TRUE(parse(stream, c).ok());
    ASSERT_EQ(c.executions.size(), 1U);
    EXPECT_EQ(c.executions[0].h.locate, 0x1234);
    EXPECT_EQ(c.executions[0].h.ts, 42U);
    EXPECT_EQ(c.executions[0].ref, 1000U);
    EXPECT_EQ(c.executions[0].qty, 100U);
    EXPECT_EQ(c.executions[0].match, 65'536U);
}

TEST(Parser, DecodesSystemEvent) {
    ItchWriter w;
    w.tracking(9).system_event(kOpen, 'Q');
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    ASSERT_EQ(c.system_events.size(), 1U);
    EXPECT_EQ(c.system_events[0].h.type, 'S');
    EXPECT_EQ(c.system_events[0].h.locate, 0);
    EXPECT_EQ(c.system_events[0].h.tracking, 9);
    EXPECT_EQ(c.system_events[0].h.ts, kOpen);
    EXPECT_EQ(c.system_events[0].code, 'Q');
}

TEST(Parser, DecodesStockDirectory) {
    test::StockDirectoryFields f;
    f.market_category = 'G';
    f.financial_status = 'D';
    f.round_lot_size = 10;
    f.round_lots_only = 'Y';
    f.issue_classification = 'A';
    f.issue_sub_type = "ET";
    f.authenticity = 'T';
    f.short_sale_threshold = 'Y';
    f.ipo_flag = 'Y';
    f.luld_tier = '2';
    f.etp_flag = 'Y';
    f.etp_leverage_factor = 3;
    f.inverse_indicator = 'Y';
    ItchWriter w;
    w.stock_directory(1, 4321, "SQQQ", f);
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    ASSERT_EQ(c.directories.size(), 1U);
    const StockDirectory& d = c.directories[0];
    EXPECT_EQ(d.h.locate, 4321);
    EXPECT_EQ(d.symbol.view(), "SQQQ");
    EXPECT_EQ(d.market_category, 'G');
    EXPECT_EQ(d.financial_status, 'D');
    EXPECT_EQ(d.round_lot_size, 10U);
    EXPECT_EQ(d.round_lots_only, 'Y');
    EXPECT_EQ(d.issue_classification, 'A');
    EXPECT_EQ(d.issue_sub_type[0], 'E');
    EXPECT_EQ(d.issue_sub_type[1], 'T');
    EXPECT_EQ(d.authenticity, 'T');
    EXPECT_EQ(d.short_sale_threshold, 'Y');
    EXPECT_EQ(d.ipo_flag, 'Y');
    EXPECT_EQ(d.luld_tier, '2');
    EXPECT_EQ(d.etp_flag, 'Y');
    EXPECT_EQ(d.etp_leverage_factor, 3U);
    EXPECT_EQ(d.inverse_indicator, 'Y');
}

TEST(Parser, DecodesTradingAction) {
    ItchWriter w;
    w.trading_action(5, 12, "TSLA", 'H', "LUDP");
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    ASSERT_EQ(c.trading_actions.size(), 1U);
    EXPECT_EQ(c.trading_actions[0].h.locate, 12);
    EXPECT_EQ(c.trading_actions[0].symbol.view(), "TSLA");
    EXPECT_EQ(c.trading_actions[0].state, 'H');
    EXPECT_EQ(std::string(c.trading_actions[0].reason.data(), 4), "LUDP");
}

TEST(Parser, DecodesAddOrderWithMpidAsAddOrder) {
    ItchWriter w;
    w.add_mpid(7, 3, 55, Side::Sell, 250, "MSFT", 1'580'000, "GSCO");
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    ASSERT_EQ(c.adds.size(), 1U);
    EXPECT_EQ(c.adds[0].h.type, 'F');
    EXPECT_EQ(c.adds[0].ref, 55U);
    EXPECT_EQ(c.adds[0].side, Side::Sell);
    EXPECT_EQ(c.adds[0].qty, 250U);
    EXPECT_EQ(c.adds[0].symbol.view(), "MSFT");
    EXPECT_EQ(c.adds[0].price, 1'580'000U);
}

TEST(Parser, DecodesOrderExecutedWithPrice) {
    ItchWriter w;
    w.execute_with_price(1, 3, 55, 40, 777, true, 1'579'900)
        .execute_with_price(2, 3, 55, 10, 778, false, 1'579'800);
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    ASSERT_EQ(c.priced_executions.size(), 2U);
    EXPECT_EQ(c.priced_executions[0].ref, 55U);
    EXPECT_EQ(c.priced_executions[0].qty, 40U);
    EXPECT_EQ(c.priced_executions[0].match, 777U);
    EXPECT_TRUE(c.priced_executions[0].printable);
    EXPECT_EQ(c.priced_executions[0].price, 1'579'900U);
    EXPECT_FALSE(c.priced_executions[1].printable);
    EXPECT_EQ(c.priced_executions[1].price, 1'579'800U);
}

TEST(Parser, DecodesCancelDeleteAndReplace) {
    ItchWriter w;
    w.cancel(1, 3, 55, 25).del(2, 3, 56).replace(3, 3, 57, 58, 900, 1'000'100);
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    ASSERT_EQ(c.cancels.size(), 1U);
    EXPECT_EQ(c.cancels[0].ref, 55U);
    EXPECT_EQ(c.cancels[0].qty, 25U);
    ASSERT_EQ(c.deletes.size(), 1U);
    EXPECT_EQ(c.deletes[0].ref, 56U);
    EXPECT_EQ(c.deletes[0].h.ts, 2U);
    ASSERT_EQ(c.replaces.size(), 1U);
    EXPECT_EQ(c.replaces[0].old_ref, 57U);
    EXPECT_EQ(c.replaces[0].new_ref, 58U);
    EXPECT_EQ(c.replaces[0].qty, 900U);
    EXPECT_EQ(c.replaces[0].price, 1'000'100U);
}

TEST(Parser, DecodesTradeCrossAndBrokenTrade) {
    ItchWriter w;
    w.trade(1, 3, Side::Buy, 75, "AMZN", 18'468'900, 9001)
        .cross_trade(2, 3, 5'000'000'000ULL, "AMZN", 18'470'000, 9002, 'C')
        .broken_trade(3, 3, 9001);
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    ASSERT_EQ(c.trades.size(), 1U);
    EXPECT_EQ(c.trades[0].side, Side::Buy);
    EXPECT_EQ(c.trades[0].qty, 75U);
    EXPECT_EQ(c.trades[0].symbol.view(), "AMZN");
    EXPECT_EQ(c.trades[0].price, 18'468'900U);
    EXPECT_EQ(c.trades[0].match, 9001U);
    ASSERT_EQ(c.crosses.size(), 1U);
    EXPECT_EQ(c.crosses[0].shares, 5'000'000'000ULL);
    EXPECT_EQ(c.crosses[0].symbol.view(), "AMZN");
    EXPECT_EQ(c.crosses[0].price, 18'470'000U);
    EXPECT_EQ(c.crosses[0].match, 9002U);
    EXPECT_EQ(c.crosses[0].cross_type, 'C');
    ASSERT_EQ(c.broken_trades.size(), 1U);
    EXPECT_EQ(c.broken_trades[0].match, 9001U);
}

TEST(Parser, RecognisesAndSkipsTheRemainingTypes) {
    ItchWriter w;
    w.reg_sho(1, 2, "AAPL", '0')
        .participant_position(2, 2, "GSCO", "AAPL")
        .mwcb_decline_level(3, 1, 2, 3)
        .mwcb_status(4, '1')
        .ipo_quoting_period(5, 2, "AAPL", 1, 'A', 1)
        .luld_auction_collar(6, 2, "AAPL", 1, 2, 3, 0)
        .operational_halt(7, 2, "AAPL", 'Q', 'H')
        .noii(8, 2, "AAPL")
        .rpii(9, 2, "AAPL", 'N')
        .direct_listing(10, 2, "AAPL");
    Collector c;
    const ParseResult r = parse(w.bytes(), c);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.messages, 10U);
    ASSERT_EQ(c.skipped.size(), 10U);
    const std::string expected = "YLVWKJhINO";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(c.skipped[i].type, expected[i]);
        EXPECT_EQ(c.skipped[i].ts, i + 1); // the header is still decoded
    }
}

TEST(Parser, TimestampUsesAllSixBytes) {
    ItchWriter w;
    w.del(kMaxWireTimestamp, 1, 1).del(0x0000'0100'0000'0000ULL, 1, 2);
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    EXPECT_EQ(c.deletes[0].h.ts, kMaxWireTimestamp);
    EXPECT_EQ(c.deletes[1].h.ts, 0x0000'0100'0000'0000ULL); // only the top byte set
}

TEST(Parser, SymbolViewStripsPaddingOnly) {
    ItchWriter w;
    w.add(1, 1, 1, Side::Buy, 1, "A", 1).add(2, 1, 2, Side::Buy, 1, "ABCDEFGH", 1);
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    EXPECT_EQ(c.adds[0].symbol.view(), "A");
    EXPECT_EQ(c.adds[1].symbol.view(), "ABCDEFGH");
    EXPECT_EQ(c.adds[0].symbol, Symbol("A"));
    EXPECT_NE(c.adds[0].symbol, Symbol("AB"));
}

TEST(Parser, DeliversMessagesInFileOrder) {
    ItchWriter w;
    w.system_event(1, 'O')
        .add(2, 1, 1, Side::Buy, 100, "AAPL", 10)
        .execute(3, 1, 1, 50, 1)
        .reg_sho(4, 1, "AAPL", '0')
        .del(5, 1, 1);
    Collector c;
    ASSERT_TRUE(parse(w.bytes(), c).ok());
    EXPECT_EQ(std::string(c.order.begin(), c.order.end()), "SAEYD");
}

TEST(Parser, EmptyInputIsValid) {
    Collector c;
    const ParseResult r = parse({}, c);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.messages, 0U);
    EXPECT_EQ(r.offset, 0U);
}

TEST(Parser, StopsAtMaxMessages) {
    ItchWriter w;
    w.del(1, 1, 1).del(2, 1, 2).del(3, 1, 3);
    Collector c;
    const ParseResult r = parse(w.bytes(), c, 2);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.messages, 2U);
    EXPECT_EQ(c.deletes.size(), 2U);
    EXPECT_EQ(r.offset, 2U * (2 + 19)); // resumable from here
}

TEST(Parser, TruncationAtEveryByteIsReportedNotCrashed) {
    ItchWriter w;
    w.del(1, 1, 1).add(2, 1, 2, Side::Buy, 100, "AAPL", 10);
    const std::span<const std::byte> all = w.bytes();
    const std::size_t first = 2 + 19;

    for (std::size_t cut = 0; cut < all.size(); ++cut) {
        Collector c;
        const ParseResult r = parse(all.first(cut), c);
        if (cut == 0 || cut == first) {
            EXPECT_TRUE(r.ok()) << "cut at " << cut;
        } else {
            EXPECT_EQ(r.error, ParseError::Truncated) << "cut at " << cut;
            // The offset points at the start of the incomplete record.
            EXPECT_EQ(r.offset, cut < first ? 0U : first) << "cut at " << cut;
        }
        EXPECT_EQ(r.messages, cut < first ? 0U : 1U) << "cut at " << cut;
    }
}

TEST(Parser, UnknownTypeIsAHardErrorWithOffset) {
    ItchWriter w;
    w.del(1, 1, 1);
    const auto unknown = bytes_of('Z', 0x00, 0x00);
    w.record(unknown).del(2, 1, 2);
    Collector c;
    const ParseResult r = parse(w.bytes(), c);
    EXPECT_EQ(r.error, ParseError::UnknownType);
    EXPECT_EQ(r.offset, 21U);
    EXPECT_EQ(r.type, 'Z');
    EXPECT_EQ(r.length, 3U);
    EXPECT_EQ(r.messages, 1U);
    EXPECT_EQ(c.deletes.size(), 1U); // nothing after the bad record is delivered
}

TEST(Parser, LengthMismatchIsAHardErrorWithOffset) {
    // A delete ('D', 19 bytes) framed with one byte too many and one too few.
    for (const std::size_t wrong : {std::size_t{18}, std::size_t{20}}) {
        std::vector<std::byte> payload(wrong, std::byte{0});
        payload[0] = static_cast<std::byte>('D');
        ItchWriter w;
        w.del(1, 1, 1).record(payload);
        Collector c;
        const ParseResult r = parse(w.bytes(), c);
        EXPECT_EQ(r.error, ParseError::LengthMismatch) << wrong;
        EXPECT_EQ(r.offset, 21U);
        EXPECT_EQ(r.type, 'D');
        EXPECT_EQ(r.length, wrong);
        EXPECT_EQ(r.messages, 1U);
    }
}

TEST(Parser, ZeroLengthRecordIsALengthMismatch) {
    const auto stream = bytes_of(0x00, 0x00, 0x00, 0x13);
    Collector c;
    const ParseResult r = parse(stream, c);
    EXPECT_EQ(r.error, ParseError::LengthMismatch);
    EXPECT_EQ(r.offset, 0U);
    EXPECT_EQ(r.messages, 0U);
}

TEST(Parser, TypeBytesAreCaseSensitive) {
    // 'h' (operational halt, 21 bytes) and 'H' (trading action, 25 bytes) are different.
    std::vector<std::byte> payload(25, std::byte{' '});
    payload[0] = static_cast<std::byte>('h');
    ItchWriter w;
    w.record(payload);
    Collector c;
    EXPECT_EQ(parse(w.bytes(), c).error, ParseError::LengthMismatch);
}

TEST(Parser, UnexpectedSideByteDoesNotCrash) {
    ItchWriter w;
    w.add(1, 1, 1, Side::Buy, 1, "AAPL", 1);
    std::vector<std::byte> bytes(w.bytes().begin(), w.bytes().end());
    bytes[2 + 19] = static_cast<std::byte>('?');
    Collector c;
    ASSERT_TRUE(parse(bytes, c).ok());
    EXPECT_EQ(c.adds[0].side, Side::Sell);
}

} // namespace
} // namespace itchbt::itch
