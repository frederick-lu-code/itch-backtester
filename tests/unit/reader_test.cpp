#include "support/collector.hpp"
#include "support/itch_writer.hpp"
#include "support/temp_file.hpp"

#include "itchbt/itch/reader.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace itchbt::itch {
namespace {

using test::Collector;
using test::ItchWriter;
using test::TempFile;

/// A stream long enough to span many buffer refills.
ItchWriter sample_stream(std::size_t orders) {
    ItchWriter w;
    w.system_event(1, 'O').stock_directory(2, 1, "AAPL");
    for (std::size_t i = 0; i < orders; ++i) {
        const auto ref = static_cast<OrderRef>(i + 1);
        const Timestamp ts = 100 + i;
        w.add(ts, 1, ref, i % 2 == 0 ? Side::Buy : Side::Sell, 100, "AAPL",
              static_cast<Price>(1'000'000 + i));
        if (i % 3 == 0) {
            w.execute(ts, 1, ref, 10, ref);
        }
        if (i % 5 == 0) {
            w.del(ts, 1, ref);
        }
    }
    w.system_event(1'000'000, 'C');
    return w;
}

TEST(MappedFile, ExposesTheWholeFile) {
    const ItchWriter w = sample_stream(100);
    const TempFile file(w);
    std::string error;
    const auto mapped = MappedFile::open(file.path(), error);
    ASSERT_TRUE(mapped) << error;
    ASSERT_EQ(mapped->bytes().size(), w.bytes().size());
    EXPECT_TRUE(std::equal(mapped->bytes().begin(), mapped->bytes().end(), w.bytes().begin()));

    Collector c;
    const ParseResult r = parse(mapped->bytes(), c);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.messages, w.message_count());
}

TEST(MappedFile, EmptyFileIsAnEmptySpan) {
    const TempFile file(ItchWriter{});
    std::string error;
    const auto mapped = MappedFile::open(file.path(), error);
    ASSERT_TRUE(mapped) << error;
    EXPECT_TRUE(mapped->bytes().empty());
}

TEST(MappedFile, MissingFileReportsAnError) {
    std::string error;
    EXPECT_FALSE(MappedFile::open("/nonexistent/itchbt/file.itch", error));
    EXPECT_NE(error.find("cannot open"), std::string::npos) << error;
}

TEST(MappedFile, DirectoryIsRejected) {
    std::string error;
    EXPECT_FALSE(MappedFile::open(std::filesystem::temp_directory_path(), error));
    EXPECT_FALSE(error.empty());
}

TEST(MappedFile, MoveTransfersOwnership) {
    const ItchWriter w = sample_stream(10);
    const TempFile file(w);
    std::string error;
    auto first = MappedFile::open(file.path(), error);
    ASSERT_TRUE(first);
    MappedFile second = std::move(*first);
    EXPECT_EQ(second.bytes().size(), w.bytes().size());
    MappedFile third;
    third = std::move(second);
    EXPECT_EQ(third.bytes().size(), w.bytes().size());
}

TEST(BufferedFile, DeliversTheSameMessagesAsTheMapping) {
    // 20,000 orders is about 1 MB: several refills of the minimum 128 KiB buffer, with records
    // straddling every refill boundary.
    const ItchWriter w = sample_stream(20'000);
    const TempFile file(w);
    std::string error;

    const auto mapped = MappedFile::open(file.path(), error);
    ASSERT_TRUE(mapped) << error;
    Collector from_map;
    ASSERT_TRUE(parse(mapped->bytes(), from_map).ok());

    auto buffered = BufferedFile::open(file.path(), error, BufferedFile::kMinBufferSize);
    ASSERT_TRUE(buffered) << error;
    Collector from_read;
    const ParseResult r = parse_stream(*buffered, from_read);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(buffered->failed());
    EXPECT_EQ(r.messages, w.message_count());
    EXPECT_EQ(r.offset, w.bytes().size());

    EXPECT_EQ(from_read.order, from_map.order);
    ASSERT_EQ(from_read.adds.size(), from_map.adds.size());
    for (std::size_t i = 0; i < from_map.adds.size(); ++i) {
        ASSERT_EQ(from_read.adds[i].ref, from_map.adds[i].ref);
        ASSERT_EQ(from_read.adds[i].price, from_map.adds[i].price);
        ASSERT_EQ(from_read.adds[i].h.ts, from_map.adds[i].h.ts);
    }
}

TEST(BufferedFile, TruncatedFileReportsAbsoluteOffset) {
    const ItchWriter w = sample_stream(20'000);
    const std::size_t cut = w.bytes().size() - 5; // inside the final system event
    const TempFile file(w.bytes().first(cut));
    std::string error;
    auto buffered = BufferedFile::open(file.path(), error, BufferedFile::kMinBufferSize);
    ASSERT_TRUE(buffered) << error;
    Collector c;
    const ParseResult r = parse_stream(*buffered, c);
    EXPECT_EQ(r.error, ParseError::Truncated);
    EXPECT_EQ(r.offset, w.bytes().size() - (2 + 12));
    EXPECT_EQ(r.messages, w.message_count() - 1);
}

TEST(BufferedFile, ErrorOffsetIsAbsoluteAcrossRefills) {
    ItchWriter w = sample_stream(20'000);
    const std::size_t bad_offset = w.bytes().size();
    const std::vector<std::byte> unknown(4, static_cast<std::byte>('z'));
    w.record(unknown).del(1, 1, 1);
    const TempFile file(w);
    std::string error;
    auto buffered = BufferedFile::open(file.path(), error, BufferedFile::kMinBufferSize);
    ASSERT_TRUE(buffered) << error;
    Collector c;
    const ParseResult r = parse_stream(*buffered, c);
    EXPECT_EQ(r.error, ParseError::UnknownType);
    EXPECT_EQ(r.offset, bad_offset);
    EXPECT_EQ(r.type, 'z');
}

TEST(BufferedFile, HonoursMaxMessages) {
    const ItchWriter w = sample_stream(20'000);
    const TempFile file(w);
    std::string error;
    auto buffered = BufferedFile::open(file.path(), error, BufferedFile::kMinBufferSize);
    ASSERT_TRUE(buffered) << error;
    Collector c;
    const ParseResult r = parse_stream(*buffered, c, 12'345);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.messages, 12'345U);
    EXPECT_EQ(c.order.size(), 12'345U);
}

TEST(BufferedFile, EmptyAndMissingFiles) {
    const TempFile file(ItchWriter{});
    std::string error;
    auto buffered = BufferedFile::open(file.path(), error);
    ASSERT_TRUE(buffered) << error;
    Collector c;
    const ParseResult r = parse_stream(*buffered, c);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.messages, 0U);

    EXPECT_FALSE(BufferedFile::open("/nonexistent/itchbt/file.itch", error));
}

} // namespace
} // namespace itchbt::itch
