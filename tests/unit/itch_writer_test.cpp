// Tests for the ITCH fixture writer.
//
// Expected lengths and offsets in this file were typed from the official specification tables,
// independently of the writer, so a mistake in the writer cannot hide behind itself.

#include "support/itch_writer.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace itchbt::test {
namespace {

using Bytes = std::span<const std::byte>;
using ::testing::ElementsAreArray;

constexpr Timestamp kOpen = 34'200'000'000'000; // 09:30:00, 0x1F1ACED9F000

template <typename... Ts>
std::array<std::byte, sizeof...(Ts)> bytes_of(Ts... values) {
    return {static_cast<std::byte>(values)...};
}

/// Big-endian unsigned integer of `length` bytes at `offset` in a message (no length prefix).
std::uint64_t be(Bytes message, std::size_t offset, std::size_t length) {
    std::uint64_t value = 0;
    for (const std::byte b : message.subspan(offset, length)) {
        value = (value << 8) | std::to_integer<std::uint64_t>(b);
    }
    return value;
}

std::string alpha(Bytes message, std::size_t offset, std::size_t length) {
    std::string text;
    for (const std::byte b : message.subspan(offset, length)) {
        text.push_back(std::to_integer<char>(b));
    }
    return text;
}

char character(Bytes message, std::size_t offset) {
    return std::to_integer<char>(message[offset]);
}

/// Splits a stream into messages, checking the framing as it goes.
std::vector<Bytes> split(Bytes stream) {
    std::vector<Bytes> messages;
    while (!stream.empty()) {
        if (stream.size() < 2) {
            throw std::runtime_error("truncated length prefix");
        }
        const auto length = static_cast<std::size_t>(be(stream, 0, 2));
        if (stream.size() < 2 + length) {
            throw std::runtime_error("truncated message");
        }
        messages.push_back(stream.subspan(2, length));
        stream = stream.subspan(2 + length);
    }
    return messages;
}

/// The only message in a writer that holds exactly one.
Bytes only(const ItchWriter& writer) {
    const auto messages = split(writer.bytes());
    if (messages.size() != 1) {
        throw std::runtime_error("expected exactly one message");
    }
    return messages.front();
}

/// One message of every type, in kMessageTypes order.
ItchWriter one_of_each() {
    ItchWriter w;
    w.system_event(1, 'O')
        .stock_directory(2, 1, "AAPL")
        .trading_action(3, 1, "AAPL", 'T')
        .reg_sho(4, 1, "AAPL", '0')
        .participant_position(5, 1, "GSCO", "AAPL")
        .mwcb_decline_level(6, 100, 200, 300)
        .mwcb_status(7, '1')
        .ipo_quoting_period(8, 1, "AAPL", 34'200, 'A', 150'000)
        .luld_auction_collar(9, 1, "AAPL", 100, 110, 90, 0)
        .operational_halt(10, 1, "AAPL", 'Q', 'H')
        .add(11, 1, 1, Side::Buy, 100, "AAPL", 1'000'000)
        .add_mpid(12, 1, 2, Side::Sell, 100, "AAPL", 1'000'100, "GSCO")
        .execute(13, 1, 1, 10, 1)
        .execute_with_price(14, 1, 1, 10, 2, true, 1'000'000)
        .cancel(15, 1, 1, 10)
        .del(16, 1, 1)
        .replace(17, 1, 2, 3, 100, 1'000'200)
        .trade(18, 1, Side::Buy, 50, "AAPL", 1'000'050, 3)
        .cross_trade(19, 1, 5'000, "AAPL", 1'000'000, 4, 'C')
        .broken_trade(20, 1, 3)
        .noii(21, 1, "AAPL")
        .rpii(22, 1, "AAPL", 'N')
        .direct_listing(23, 1, "AAPL");
    return w;
}

TEST(ItchWriter, AddOrderMatchesHandComputedBytes) {
    ItchWriter w;
    w.tracking(0x0102).add(kOpen, 7, 0x0102030405060708, Side::Buy, 300, "AAPL", 1'234'500);

    const auto expected =
        bytes_of(0x00, 0x24,                                     // record length: 36
                 'A',                                            // message type
                 0x00, 0x07,                                     // stock locate
                 0x01, 0x02,                                     // tracking number
                 0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,             // timestamp, 6 bytes
                 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // order reference
                 'B',                                            // buy/sell
                 0x00, 0x00, 0x01, 0x2C,                         // shares: 300
                 'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',         // stock
                 0x00, 0x12, 0xD6, 0x44);                        // price: 123.4500
    EXPECT_THAT(w.bytes(), ElementsAreArray(expected));
}

TEST(ItchWriter, SystemEventMatchesHandComputedBytes) {
    ItchWriter w;
    w.system_event(kOpen, 'Q');

    const auto expected =
        bytes_of(0x00, 0x0C, 'S', 0x00, 0x00, 0x00, 0x00, 0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00, 'Q');
    EXPECT_THAT(w.bytes(), ElementsAreArray(expected));
}

TEST(ItchWriter, EveryMessageTypeHasItsSpecifiedLength) {
    // Lengths from the official specification: last field offset plus last field length.
    const std::vector<std::pair<char, std::size_t>> official = {
        {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
        {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
        {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
    ASSERT_EQ(official.size(), kMessageTypes.size());

    const ItchWriter w = one_of_each();
    const auto messages = split(w.bytes());
    ASSERT_EQ(messages.size(), official.size());
    EXPECT_EQ(w.message_count(), official.size());

    for (std::size_t i = 0; i < official.size(); ++i) {
        const auto [type, length] = official[i];
        SCOPED_TRACE(std::string("message type ") + type);
        EXPECT_EQ(kMessageTypes[i], type);
        EXPECT_EQ(character(messages[i], 0), type);
        EXPECT_EQ(messages[i].size(), length);
        EXPECT_EQ(message_length(type), length);
        // Common header: timestamps were assigned 1..23 in order.
        EXPECT_EQ(be(messages[i], 5, 6), i + 1);
    }
}

TEST(ItchWriter, UnknownTypeHasNoLength) {
    EXPECT_EQ(message_length('Z'), 0U);
    EXPECT_EQ(message_length('\0'), 0U);
    EXPECT_EQ(message_length('s'), 0U); // types are case sensitive: 'h' is not 'H'
}

TEST(ItchWriter, HeaderFieldsSitAtSpecifiedOffsets) {
    ItchWriter w;
    w.tracking(0xBEEF).del(kMaxWireTimestamp, 0xABCD, 42);
    const Bytes m = only(w);
    EXPECT_EQ(character(m, 0), 'D');
    EXPECT_EQ(be(m, 1, 2), 0xABCDU);
    EXPECT_EQ(be(m, 3, 2), 0xBEEFU);
    EXPECT_EQ(be(m, 5, 6), kMaxWireTimestamp);
    EXPECT_EQ(be(m, 11, 8), 42U);
}

TEST(ItchWriter, SystemAndStockMessagesHaveLocateZeroWhereSpecified) {
    ItchWriter w;
    w.system_event(1, 'O').mwcb_decline_level(2, 1, 2, 3).mwcb_status(3, '2');
    for (const Bytes m : split(w.bytes())) {
        EXPECT_EQ(be(m, 1, 2), 0U);
    }
}

TEST(ItchWriter, TimestampMustFitSixBytes) {
    ItchWriter w;
    EXPECT_NO_THROW(w.system_event(0, 'O'));
    EXPECT_NO_THROW(w.system_event(kMaxWireTimestamp, 'C'));
    EXPECT_THROW(w.system_event(kMaxWireTimestamp + 1, 'C'), std::invalid_argument);
}

TEST(ItchWriter, AlphaFieldsAreLeftJustifiedAndSpacePadded) {
    ItchWriter w;
    w.add(1, 1, 1, Side::Sell, 1, "A", 1).add(2, 2, 2, Side::Sell, 1, "ABCDEFGH", 1);
    const auto messages = split(w.bytes());
    EXPECT_EQ(alpha(messages[0], 24, 8), "A       ");
    EXPECT_EQ(alpha(messages[1], 24, 8), "ABCDEFGH");
    EXPECT_EQ(character(messages[0], 19), 'S');
}

TEST(ItchWriter, OverlongAlphaFieldThrows) {
    ItchWriter w;
    EXPECT_THROW(w.add(1, 1, 1, Side::Buy, 1, "ABCDEFGHI", 1), std::invalid_argument);
    EXPECT_THROW(w.add_mpid(1, 1, 1, Side::Buy, 1, "AAPL", 1, "TOOLONG"), std::invalid_argument);
}

TEST(ItchWriter, StockDirectoryOffsets) {
    ItchWriter w;
    StockDirectoryFields f;
    f.round_lot_size = 100;
    f.issue_sub_type = "Z";
    f.etp_leverage_factor = 3;
    f.inverse_indicator = 'Y';
    w.stock_directory(1, 9, "QQQ", f);
    const Bytes m = only(w);
    EXPECT_EQ(alpha(m, 11, 8), "QQQ     ");
    EXPECT_EQ(character(m, 19), 'Q'); // market category
    EXPECT_EQ(character(m, 20), 'N'); // financial status
    EXPECT_EQ(be(m, 21, 4), 100U);    // round lot size
    EXPECT_EQ(character(m, 25), 'N'); // round lots only
    EXPECT_EQ(character(m, 26), 'C'); // issue classification
    EXPECT_EQ(alpha(m, 27, 2), "Z "); // issue sub-type
    EXPECT_EQ(character(m, 29), 'P'); // authenticity
    EXPECT_EQ(character(m, 30), 'N'); // short sale threshold
    EXPECT_EQ(character(m, 31), 'N'); // IPO flag
    EXPECT_EQ(character(m, 32), '1'); // LULD tier
    EXPECT_EQ(character(m, 33), 'N'); // ETP flag
    EXPECT_EQ(be(m, 34, 4), 3U);      // ETP leverage factor
    EXPECT_EQ(character(m, 38), 'Y'); // inverse indicator
}

TEST(ItchWriter, AdministrativeMessageOffsets) {
    {
        ItchWriter w;
        w.trading_action(1, 1, "AAPL", 'H', "LUDP");
        const Bytes m = only(w);
        EXPECT_EQ(alpha(m, 11, 8), "AAPL    ");
        EXPECT_EQ(character(m, 19), 'H');
        EXPECT_EQ(character(m, 20), ' '); // reserved
        EXPECT_EQ(alpha(m, 21, 4), "LUDP");
    }
    {
        ItchWriter w;
        w.reg_sho(1, 1, "AAPL", '1');
        EXPECT_EQ(character(only(w), 19), '1');
    }
    {
        ItchWriter w;
        w.participant_position(1, 1, "GSCO", "AAPL", 'Y', 'N', 'A');
        const Bytes m = only(w);
        EXPECT_EQ(alpha(m, 11, 4), "GSCO");
        EXPECT_EQ(alpha(m, 15, 8), "AAPL    ");
        EXPECT_EQ(character(m, 23), 'Y');
        EXPECT_EQ(character(m, 24), 'N');
        EXPECT_EQ(character(m, 25), 'A');
    }
    {
        ItchWriter w;
        w.mwcb_decline_level(1, 11, 22, 33);
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 11U);
        EXPECT_EQ(be(m, 19, 8), 22U);
        EXPECT_EQ(be(m, 27, 8), 33U);
    }
    {
        ItchWriter w;
        w.mwcb_status(1, '3');
        EXPECT_EQ(character(only(w), 11), '3');
    }
    {
        ItchWriter w;
        w.ipo_quoting_period(1, 1, "NEWCO", 36'000, 'A', 250'000);
        const Bytes m = only(w);
        EXPECT_EQ(alpha(m, 11, 8), "NEWCO   ");
        EXPECT_EQ(be(m, 19, 4), 36'000U);
        EXPECT_EQ(character(m, 23), 'A');
        EXPECT_EQ(be(m, 24, 4), 250'000U);
    }
    {
        ItchWriter w;
        w.luld_auction_collar(1, 1, "AAPL", 1'000, 1'100, 900, 2);
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 19, 4), 1'000U);
        EXPECT_EQ(be(m, 23, 4), 1'100U);
        EXPECT_EQ(be(m, 27, 4), 900U);
        EXPECT_EQ(be(m, 31, 4), 2U);
    }
    {
        ItchWriter w;
        w.operational_halt(1, 1, "AAPL", 'B', 'T');
        const Bytes m = only(w);
        EXPECT_EQ(character(m, 19), 'B');
        EXPECT_EQ(character(m, 20), 'T');
    }
}

TEST(ItchWriter, OrderMessageOffsets) {
    {
        ItchWriter w;
        w.add_mpid(1, 1, 77, Side::Sell, 500, "MSFT", 1'580'000, "MSCO");
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 77U);
        EXPECT_EQ(character(m, 19), 'S');
        EXPECT_EQ(be(m, 20, 4), 500U);
        EXPECT_EQ(alpha(m, 24, 8), "MSFT    ");
        EXPECT_EQ(be(m, 32, 4), 1'580'000U);
        EXPECT_EQ(alpha(m, 36, 4), "MSCO");
    }
    {
        ItchWriter w;
        w.execute(1, 1, 77, 200, 9'001);
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 77U);
        EXPECT_EQ(be(m, 19, 4), 200U);
        EXPECT_EQ(be(m, 23, 8), 9'001U);
    }
    {
        ItchWriter w;
        w.execute_with_price(1, 1, 77, 200, 9'002, true, 1'579'900)
            .execute_with_price(2, 1, 77, 100, 9'003, false, 1'579'900);
        const auto messages = split(w.bytes());
        EXPECT_EQ(be(messages[0], 11, 8), 77U);
        EXPECT_EQ(be(messages[0], 19, 4), 200U);
        EXPECT_EQ(be(messages[0], 23, 8), 9'002U);
        EXPECT_EQ(character(messages[0], 31), 'Y');
        EXPECT_EQ(be(messages[0], 32, 4), 1'579'900U);
        EXPECT_EQ(character(messages[1], 31), 'N');
    }
    {
        ItchWriter w;
        w.cancel(1, 1, 77, 50);
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 77U);
        EXPECT_EQ(be(m, 19, 4), 50U);
    }
    {
        ItchWriter w;
        w.replace(1, 1, 77, 78, 400, 1'580'100);
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 77U);
        EXPECT_EQ(be(m, 19, 8), 78U);
        EXPECT_EQ(be(m, 27, 4), 400U);
        EXPECT_EQ(be(m, 31, 4), 1'580'100U);
    }
}

TEST(ItchWriter, TradeMessageOffsets) {
    {
        ItchWriter w;
        w.trade(1, 1, Side::Buy, 25, "AAPL", 2'910'000, 5'000);
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 0U); // order reference is always zero
        EXPECT_EQ(character(m, 19), 'B');
        EXPECT_EQ(be(m, 20, 4), 25U);
        EXPECT_EQ(alpha(m, 24, 8), "AAPL    ");
        EXPECT_EQ(be(m, 32, 4), 2'910'000U);
        EXPECT_EQ(be(m, 36, 8), 5'000U);
    }
    {
        ItchWriter w;
        w.cross_trade(1, 1, 5'000'000'000, "AAPL", 2'915'200, 5'001, 'C');
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 5'000'000'000U); // shares are 8 bytes in a cross
        EXPECT_EQ(alpha(m, 19, 8), "AAPL    ");
        EXPECT_EQ(be(m, 27, 4), 2'915'200U);
        EXPECT_EQ(be(m, 31, 8), 5'001U);
        EXPECT_EQ(character(m, 39), 'C');
    }
    {
        ItchWriter w;
        w.broken_trade(1, 1, 5'000);
        EXPECT_EQ(be(only(w), 11, 8), 5'000U);
    }
}

TEST(ItchWriter, SkippedMessageOffsets) {
    {
        ItchWriter w;
        NoiiFields f;
        f.paired_shares = 1'000;
        f.imbalance_shares = 200;
        f.imbalance_direction = 'B';
        f.far_price = 10;
        f.near_price = 20;
        f.reference_price = 30;
        f.cross_type = 'O';
        f.price_variation = '1';
        w.noii(1, 1, "AAPL", f);
        const Bytes m = only(w);
        EXPECT_EQ(be(m, 11, 8), 1'000U);
        EXPECT_EQ(be(m, 19, 8), 200U);
        EXPECT_EQ(character(m, 27), 'B');
        EXPECT_EQ(alpha(m, 28, 8), "AAPL    ");
        EXPECT_EQ(be(m, 36, 4), 10U);
        EXPECT_EQ(be(m, 40, 4), 20U);
        EXPECT_EQ(be(m, 44, 4), 30U);
        EXPECT_EQ(character(m, 48), 'O');
        EXPECT_EQ(character(m, 49), '1');
    }
    {
        ItchWriter w;
        w.rpii(1, 1, "AAPL", 'A');
        const Bytes m = only(w);
        EXPECT_EQ(alpha(m, 11, 8), "AAPL    ");
        EXPECT_EQ(character(m, 19), 'A');
    }
    {
        ItchWriter w;
        DirectListingFields f;
        f.open_eligibility = 'Y';
        f.min_allowable_price = 1;
        f.max_allowable_price = 2;
        f.near_execution_price = 3;
        f.near_execution_time = 4;
        f.lower_collar = 5;
        f.upper_collar = 6;
        w.direct_listing(1, 1, "NEWCO", f);
        const Bytes m = only(w);
        EXPECT_EQ(alpha(m, 11, 8), "NEWCO   ");
        EXPECT_EQ(character(m, 19), 'Y');
        EXPECT_EQ(be(m, 20, 4), 1U);
        EXPECT_EQ(be(m, 24, 4), 2U);
        EXPECT_EQ(be(m, 28, 4), 3U);
        EXPECT_EQ(be(m, 32, 8), 4U);
        EXPECT_EQ(be(m, 40, 4), 5U);
        EXPECT_EQ(be(m, 44, 4), 6U);
    }
}

TEST(ItchWriter, RecordAndRawWriteExactlyWhatTheyAreGiven) {
    ItchWriter w;
    const auto unknown = bytes_of('Z', 0x01, 0x02);
    const auto junk = bytes_of(0xFF);
    w.record(unknown).raw(junk);

    EXPECT_THAT(w.bytes(), ElementsAreArray(bytes_of(0x00, 0x03, 'Z', 0x01, 0x02, 0xFF)));
    EXPECT_EQ(w.message_count(), 1U); // raw bytes are not a message
}

TEST(ItchWriter, RecordRejectsPayloadLongerThanTheLengthField) {
    ItchWriter w;
    const std::vector<std::byte> too_long(0x10000);
    EXPECT_THROW(w.record(too_long), std::invalid_argument);
    EXPECT_TRUE(w.bytes().empty());
}

TEST(ItchWriter, WriteFileRoundTrips) {
    const ItchWriter w = one_of_each();
    const auto path = std::filesystem::temp_directory_path() /
                      ("itchbt_writer_test_" +
                       std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".itch");
    w.write_file(path);

    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in);
    const std::vector<char> contents((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);

    ASSERT_EQ(contents.size(), w.bytes().size());
    for (std::size_t i = 0; i < contents.size(); ++i) {
        ASSERT_EQ(static_cast<std::byte>(contents[i]), w.bytes()[i]) << "at byte " << i;
    }
}

TEST(ItchWriter, WriteFileReportsUnwritablePath) {
    const ItchWriter w;
    EXPECT_THROW(w.write_file("/nonexistent-directory/itchbt/out.itch"), std::runtime_error);
}

} // namespace
} // namespace itchbt::test
