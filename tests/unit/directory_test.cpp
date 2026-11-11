#include "support/itch_writer.hpp"

#include "itchbt/itch/directory.hpp"
#include "itchbt/itch/parser.hpp"

#include <gtest/gtest.h>

namespace itchbt::itch {
namespace {

using test::ItchWriter;

struct Feed : NullHandler {
    using NullHandler::on;
    SymbolDirectory directory;
    MarketState market;
    void on(const StockDirectory& m) { directory.on(m); }
    void on(const SystemEvent& m) { market.on(m); }
    void on(const TradingAction& m) { market.on(m); }
};

TEST(SymbolDirectory, MapsLocatesAndSymbolsBothWays) {
    ItchWriter w;
    w.stock_directory(1, 13, "AAPL").stock_directory(2, 4000, "MSFT");
    Feed feed;
    ASSERT_TRUE(parse(w.bytes(), feed).ok());
    EXPECT_EQ(feed.directory.size(), 2U);
    EXPECT_EQ(feed.directory.find("AAPL"), Locate{13});
    EXPECT_EQ(feed.directory.find("MSFT"), Locate{4000});
    EXPECT_FALSE(feed.directory.find("TSLA"));
    EXPECT_EQ(feed.directory.symbol(13), "AAPL");
    EXPECT_TRUE(feed.directory.known(4000));
    EXPECT_FALSE(feed.directory.known(14));
    EXPECT_TRUE(feed.directory.symbol(14).empty());
}

TEST(SymbolDirectory, RepeatedDirectoryMessageDoesNotDoubleCount) {
    ItchWriter w;
    w.stock_directory(1, 13, "AAPL").stock_directory(2, 13, "AAPL");
    Feed feed;
    ASSERT_TRUE(parse(w.bytes(), feed).ok());
    EXPECT_EQ(feed.directory.size(), 1U);
}

TEST(MarketState, RegularHoursRunFromQToM) {
    MarketState market;
    EXPECT_FALSE(market.regular_hours());
    for (const auto& [code, expected] : {std::pair{'O', false},
                                         {'S', false},
                                         {'Q', true},
                                         {'M', false},
                                         {'E', false},
                                         {'C', false}}) {
        SystemEvent e;
        e.code = code;
        market.on(e);
        EXPECT_EQ(market.regular_hours(), expected) << code;
        EXPECT_EQ(market.last_system_event(), code);
    }
}

TEST(MarketState, SymbolsAreHaltedUntilATradingActionSaysOtherwise) {
    ItchWriter w;
    w.trading_action(1, 5, "AAPL", 'T')
        .trading_action(2, 6, "MSFT", 'T')
        .trading_action(3, 6, "MSFT", 'H')
        .trading_action(4, 7, "TSLA", 'Q');
    Feed feed;
    ASSERT_TRUE(parse(w.bytes(), feed).ok());
    EXPECT_TRUE(feed.market.trading(5));
    EXPECT_FALSE(feed.market.trading(6)); // halted after trading
    EXPECT_FALSE(feed.market.trading(7)); // quotation only is not trading
    EXPECT_FALSE(feed.market.trading(8)); // never mentioned: assume halted
    EXPECT_EQ(feed.market.trading_state(6), 'H');
}

} // namespace
} // namespace itchbt::itch
