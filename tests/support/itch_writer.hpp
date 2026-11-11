#pragma once

// Builds valid Nasdaq TotalView-ITCH 5.0 byte streams for tests.
//
// Every layout here was checked field by field against the official specification
// (NQTVITCHspecification.pdf, revision of April 28, 2023). Test support only: it allocates and
// throws freely, which the engine itself may not do.

#include "itchbt/util/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace itchbt::test {

/// Length of each message type in bytes, excluding the 2-byte record length prefix.
/// Returns 0 for a type byte that ITCH 5.0 does not define.
[[nodiscard]] std::size_t message_length(char type) noexcept;

/// Every message type defined by ITCH 5.0, in specification order.
inline constexpr std::string_view kMessageTypes = "SRHYLVWKJhAFECXDUPQBINO";

/// Stock directory fields that tests rarely care about, with benign defaults.
struct StockDirectoryFields {
    char market_category = 'Q';
    char financial_status = 'N';
    std::uint32_t round_lot_size = 100;
    char round_lots_only = 'N';
    char issue_classification = 'C';
    std::string_view issue_sub_type = "Z";
    char authenticity = 'P';
    char short_sale_threshold = 'N';
    char ipo_flag = 'N';
    char luld_tier = '1';
    char etp_flag = 'N';
    std::uint32_t etp_leverage_factor = 0;
    char inverse_indicator = 'N';
};

/// Net order imbalance fields.
struct NoiiFields {
    std::uint64_t paired_shares = 0;
    std::uint64_t imbalance_shares = 0;
    char imbalance_direction = 'N';
    Price far_price = 0;
    Price near_price = 0;
    Price reference_price = 0;
    char cross_type = 'C';
    char price_variation = 'L';
};

/// Direct Listing with Capital Raise price discovery fields.
struct DirectListingFields {
    char open_eligibility = 'N';
    Price min_allowable_price = 0;
    Price max_allowable_price = 0;
    Price near_execution_price = 0;
    std::uint64_t near_execution_time = 0;
    Price lower_collar = 0;
    Price upper_collar = 0;
};

class ItchWriter {
public:
    /// Tracking number stamped on every later message. Defaults to 0.
    ItchWriter& tracking(std::uint16_t tracking_number);

    // Administrative messages.
    ItchWriter& system_event(Timestamp ts, char event_code);
    ItchWriter& stock_directory(Timestamp ts, Locate loc, std::string_view symbol,
                                const StockDirectoryFields& fields = {});
    ItchWriter& trading_action(Timestamp ts, Locate loc, std::string_view symbol, char state,
                               std::string_view reason = "");
    ItchWriter& reg_sho(Timestamp ts, Locate loc, std::string_view symbol, char action);
    ItchWriter& participant_position(Timestamp ts, Locate loc, std::string_view mpid,
                                     std::string_view symbol, char primary_market_maker = 'Y',
                                     char market_maker_mode = 'N', char participant_state = 'A');
    /// MWCB levels are Price (8): eight implied decimal places, eight bytes each.
    ItchWriter& mwcb_decline_level(Timestamp ts, std::uint64_t level1, std::uint64_t level2,
                                   std::uint64_t level3);
    ItchWriter& mwcb_status(Timestamp ts, char breached_level);
    ItchWriter& ipo_quoting_period(Timestamp ts, Locate loc, std::string_view symbol,
                                   std::uint32_t release_time_s, char qualifier, Price ipo_price);
    ItchWriter& luld_auction_collar(Timestamp ts, Locate loc, std::string_view symbol,
                                    Price reference, Price upper, Price lower,
                                    std::uint32_t extension);
    ItchWriter& operational_halt(Timestamp ts, Locate loc, std::string_view symbol,
                                 char market_code, char action);

    // Order book messages.
    ItchWriter& add(Timestamp ts, Locate loc, OrderRef ref, Side side, Qty qty,
                    std::string_view symbol, Price px);
    ItchWriter& add_mpid(Timestamp ts, Locate loc, OrderRef ref, Side side, Qty qty,
                         std::string_view symbol, Price px, std::string_view mpid);
    ItchWriter& execute(Timestamp ts, Locate loc, OrderRef ref, Qty qty, MatchNo match);
    ItchWriter& execute_with_price(Timestamp ts, Locate loc, OrderRef ref, Qty qty, MatchNo match,
                                   bool printable, Price px);
    ItchWriter& cancel(Timestamp ts, Locate loc, OrderRef ref, Qty qty);
    ItchWriter& del(Timestamp ts, Locate loc, OrderRef ref);
    ItchWriter& replace(Timestamp ts, Locate loc, OrderRef old_ref, OrderRef new_ref, Qty qty,
                        Price px);

    // Trade messages.
    /// Non-cross trade against a hidden order. The order reference is written as zero, as
    /// Nasdaq has done since 2010.
    ItchWriter& trade(Timestamp ts, Locate loc, Side side, Qty qty, std::string_view symbol,
                      Price px, MatchNo match);
    ItchWriter& cross_trade(Timestamp ts, Locate loc, std::uint64_t shares, std::string_view symbol,
                            Price px, MatchNo match, char cross_type);
    ItchWriter& broken_trade(Timestamp ts, Locate loc, MatchNo match);

    // Other messages the engine parses and skips.
    ItchWriter& noii(Timestamp ts, Locate loc, std::string_view symbol,
                     const NoiiFields& fields = {});
    ItchWriter& rpii(Timestamp ts, Locate loc, std::string_view symbol, char interest_flag);
    ItchWriter& direct_listing(Timestamp ts, Locate loc, std::string_view symbol,
                               const DirectListingFields& fields = {});

    // Escape hatches for malformed input.
    /// Appends a 2-byte big-endian length prefix followed by `payload`, with no validation.
    ItchWriter& record(std::span<const std::byte> payload);
    /// Appends bytes verbatim, with no length prefix.
    ItchWriter& raw(std::span<const std::byte> bytes);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t message_count() const noexcept { return message_count_; }
    void write_file(const std::filesystem::path& path) const;

private:
    void begin(char type, Locate loc, Timestamp ts);
    void end();
    void put_u8(std::uint8_t value);
    void put_u16(std::uint16_t value);
    void put_u32(std::uint32_t value);
    void put_u48(std::uint64_t value);
    void put_u64(std::uint64_t value);
    void put_char(char value);
    void put_alpha(std::string_view text, std::size_t width);
    void put_side(Side side);

    std::vector<std::byte> buffer_;
    std::size_t record_start_ = 0;
    std::size_t message_count_ = 0;
    std::uint16_t tracking_ = 0;
    char open_type_ = '\0';
};

} // namespace itchbt::test
