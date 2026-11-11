#include "support/temp_file.hpp"

#include "itchbt/record/recorder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace itchbt::record {
namespace {

std::vector<char> slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

template <typename Record>
std::vector<Record> records_in(const std::vector<char>& bytes) {
    std::vector<Record> out;
    for (std::size_t pos = sizeof(FileHeader); pos + sizeof(Record) <= bytes.size();
         pos += sizeof(Record)) {
        Record r;
        std::memcpy(&r, bytes.data() + pos, sizeof(r));
        out.push_back(r);
    }
    return out;
}

/// Writes an interleaved sequence of all four record kinds.
void write_sample(Recorder& recorder, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        OrderRecord o;
        o.ts = i;
        o.order_id = i * 3;
        o.price = static_cast<std::uint32_t>(i);
        recorder.write(o);
        if (i % 2 == 0) {
            BboRecord b;
            b.ts = i;
            b.bid_qty = i + 7;
            recorder.write(b);
        }
        if (i % 5 == 0) {
            FillRecord f;
            f.ts = i;
            f.qty = static_cast<std::uint32_t>(i + 1);
            recorder.write(f);
        }
        if (i % 11 == 0) {
            RiskRecord r;
            r.ts = i;
            r.value = -static_cast<std::int64_t>(i);
            recorder.write(r);
        }
    }
}

class RecorderTest : public ::testing::Test {
protected:
    std::filesystem::path dir = test::TempFile::unique_path();
    void TearDown() override { std::filesystem::remove_all(dir); }
};

TEST_F(RecorderTest, DefaultRecorderDiscardsButStillCounts) {
    Recorder recorder;
    EXPECT_FALSE(recorder.enabled());
    write_sample(recorder, 10);
    EXPECT_EQ(recorder.counts().orders, 10U);
    EXPECT_EQ(recorder.counts().bbo, 5U);
    EXPECT_TRUE(recorder.close());
}

TEST_F(RecorderTest, FilesStartWithAVersionedHeader) {
    Recorder recorder;
    std::string error;
    ASSERT_TRUE(recorder.open(dir, error, RecorderMode::Sync)) << error;
    ASSERT_TRUE(recorder.close());
    struct Expected {
        const char* name;
        RecordType type;
        std::size_t size;
    };
    const std::array<Expected, 4> expected = {
        {{"orders.bin", RecordType::Order, sizeof(OrderRecord)},
         {"fills.bin", RecordType::Fill, sizeof(FillRecord)},
         {"bbo.bin", RecordType::Bbo, sizeof(BboRecord)},
         {"risk.bin", RecordType::Risk, sizeof(RiskRecord)}}};
    for (const auto& e : expected) {
        const std::vector<char> bytes = slurp(dir / e.name);
        ASSERT_EQ(bytes.size(), sizeof(FileHeader)) << e.name;
        FileHeader header;
        std::memcpy(&header, bytes.data(), sizeof(header));
        EXPECT_EQ(header.magic, kMagic);
        EXPECT_EQ(header.version, kFormatVersion);
        EXPECT_EQ(header.record_type, static_cast<std::uint32_t>(e.type));
        EXPECT_EQ(header.record_size, e.size);
    }
}

TEST_F(RecorderTest, RecordsRoundTripInOrder) {
    for (const RecorderMode mode : {RecorderMode::Sync, RecorderMode::Async}) {
        Recorder recorder;
        std::string error;
        ASSERT_TRUE(recorder.open(dir, error, mode)) << error;
        EXPECT_TRUE(recorder.enabled());
        write_sample(recorder, 1000);
        ASSERT_TRUE(recorder.close());

        const auto orders = records_in<OrderRecord>(slurp(dir / "orders.bin"));
        ASSERT_EQ(orders.size(), 1000U);
        for (std::size_t i = 0; i < orders.size(); ++i) {
            ASSERT_EQ(orders[i].ts, i);
            ASSERT_EQ(orders[i].order_id, i * 3);
        }
        const auto fills = records_in<FillRecord>(slurp(dir / "fills.bin"));
        ASSERT_EQ(fills.size(), 200U);
        EXPECT_EQ(fills[3].qty, 16U);
        const auto risk = records_in<RiskRecord>(slurp(dir / "risk.bin"));
        ASSERT_EQ(risk.size(), 91U);
        EXPECT_EQ(risk[1].value, -11);
        EXPECT_EQ(records_in<BboRecord>(slurp(dir / "bbo.bin")).size(), 500U);
        EXPECT_EQ(recorder.counts().orders, 1000U);
    }
}

TEST_F(RecorderTest, AsyncAndSyncWriteIdenticalBytes) {
    const std::filesystem::path other = test::TempFile::unique_path();
    std::string error;
    Recorder sync_recorder;
    ASSERT_TRUE(sync_recorder.open(dir, error, RecorderMode::Sync)) << error;
    write_sample(sync_recorder, 20'000);
    ASSERT_TRUE(sync_recorder.close());
    Recorder async_recorder;
    ASSERT_TRUE(async_recorder.open(other, error, RecorderMode::Async, 64)) << error;
    write_sample(async_recorder, 20'000);
    ASSERT_TRUE(async_recorder.close());
    for (const char* name : {"orders.bin", "fills.bin", "bbo.bin", "risk.bin"}) {
        EXPECT_EQ(slurp(dir / name), slurp(other / name)) << name;
    }
    std::filesystem::remove_all(other);
}

TEST_F(RecorderTest, FullRingBlocksTheProducerAndLosesNothing) {
    Recorder recorder;
    std::string error;
    // The smallest ring: the producer outruns the writer almost at once.
    ASSERT_TRUE(recorder.open(dir, error, RecorderMode::Async, 2)) << error;
    write_sample(recorder, 50'000);
    const RecorderStats stats = recorder.stats();
    ASSERT_TRUE(recorder.close());
    EXPECT_GT(stats.blocked_records, 0U);
    EXPECT_GT(stats.blocked_ns, 0U);
    const auto orders = records_in<OrderRecord>(slurp(dir / "orders.bin"));
    ASSERT_EQ(orders.size(), 50'000U);
    for (std::size_t i = 0; i < orders.size(); ++i) {
        ASSERT_EQ(orders[i].ts, i) << "record lost or reordered";
    }
    EXPECT_EQ(records_in<BboRecord>(slurp(dir / "bbo.bin")).size(), 25'000U);
}

TEST_F(RecorderTest, CanBeReopenedAfterClose) {
    Recorder recorder;
    std::string error;
    ASSERT_TRUE(recorder.open(dir, error)) << error;
    write_sample(recorder, 10);
    ASSERT_TRUE(recorder.close());
    ASSERT_TRUE(recorder.open(dir, error)) << error;
    EXPECT_EQ(recorder.counts().orders, 0U);
    write_sample(recorder, 3);
    ASSERT_TRUE(recorder.close());
    EXPECT_EQ(records_in<OrderRecord>(slurp(dir / "orders.bin")).size(), 3U);
}

TEST_F(RecorderTest, UnwritableDirectoryIsReported) {
    Recorder recorder;
    std::string error;
    EXPECT_FALSE(recorder.open("/nonexistent-root-dir/itchbt/run", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(recorder.enabled());
}

TEST_F(RecorderTest, DestructorDrainsAnAsyncRecorderThatWasNotClosed) {
    {
        Recorder recorder;
        std::string error;
        ASSERT_TRUE(recorder.open(dir, error, RecorderMode::Async, 16)) << error;
        write_sample(recorder, 5'000);
    }
    EXPECT_EQ(records_in<OrderRecord>(slurp(dir / "orders.bin")).size(), 5'000U);
}

} // namespace
} // namespace itchbt::record
