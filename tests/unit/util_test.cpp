#include "itchbt/util/cli.hpp"
#include "itchbt/util/time.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace itchbt {
namespace {

TEST(Time, FormatsNanosecondsSinceMidnight) {
    EXPECT_EQ(format_time(0), "00:00:00.000000000");
    EXPECT_EQ(format_time(34'200'000'000'000), "09:30:00.000000000");
    EXPECT_EQ(format_time(57'600'123'456'789), "16:00:00.123456789");
}

TEST(Time, ParsesClockTimes) {
    EXPECT_EQ(parse_time("09:30"), 34'200'000'000'000ULL);
    EXPECT_EQ(parse_time("10:30:00"), 37'800'000'000'000ULL);
    EXPECT_EQ(parse_time("16:00:00.5"), 57'600'500'000'000ULL);
    EXPECT_EQ(parse_time("00:00:00.000000001"), 1ULL);
}

TEST(Time, RejectsMalformedTimes) {
    for (const char* bad : {"", "9", "24:00", "10:60", "10:30:60", "10:30:", "10::30", "ab:cd",
                            "10:30:00.", "10:30:00.1234567890", "100:00", "10.5"}) {
        EXPECT_FALSE(parse_time(bad)) << bad;
    }
}

TEST(Time, RoundTrips) {
    for (const Timestamp ts : {Timestamp{0}, Timestamp{1}, Timestamp{34'200'000'000'000},
                               Timestamp{86'399'999'999'999}}) {
        EXPECT_EQ(parse_time(format_time(ts)), ts);
    }
}

TEST(Args, ReadsValuesFlagsAndNumbers) {
    const std::vector<const char*> argv = {"tool",      "--input",        "a.itch",
                                           "--verbose", "--max-messages", "500"};
    Args args(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(args.value("--input"), "a.itch");
    EXPECT_TRUE(args.flag("--verbose"));
    EXPECT_FALSE(args.flag("--quiet"));
    bool bad = false;
    EXPECT_EQ(args.number("--max-messages", bad), 500U);
    EXPECT_FALSE(bad);
    EXPECT_FALSE(args.value("--missing"));
    EXPECT_TRUE(args.unused().empty());
}

TEST(Args, ReportsMalformedNumbersAndUnusedArguments) {
    const std::vector<const char*> argv = {"tool", "--max-messages", "12x", "--typo"};
    Args args(static_cast<int>(argv.size()), argv.data());
    bool bad = false;
    EXPECT_FALSE(args.number("--max-messages", bad));
    EXPECT_TRUE(bad);
    ASSERT_EQ(args.unused().size(), 1U);
    EXPECT_EQ(args.unused()[0], "--typo");
}

TEST(Args, SplitsCommaSeparatedLists) {
    EXPECT_EQ(split_commas("AAPL,MSFT"), (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_EQ(split_commas("AAPL"), (std::vector<std::string>{"AAPL"}));
    EXPECT_EQ(split_commas(",AAPL,,MSFT,"), (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_TRUE(split_commas("").empty());
}

} // namespace
} // namespace itchbt
