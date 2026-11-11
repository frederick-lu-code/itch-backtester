#include "support/itch_writer.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace itchbt::test {

namespace {

constexpr std::size_t kLengthPrefixSize = 2;
constexpr std::size_t kSymbolWidth = 8;
constexpr std::size_t kMpidWidth = 4;

} // namespace

std::size_t message_length(char type) noexcept {
    switch (type) {
    case 'S': return 12; // System event
    case 'R': return 39; // Stock directory
    case 'H': return 25; // Stock trading action
    case 'Y': return 20; // Reg SHO restriction
    case 'L': return 26; // Market participant position
    case 'V': return 35; // MWCB decline level
    case 'W': return 12; // MWCB status
    case 'K': return 28; // IPO quoting period update
    case 'J': return 35; // LULD auction collar
    case 'h': return 21; // Operational halt
    case 'A': return 36; // Add order
    case 'F': return 40; // Add order with MPID
    case 'E': return 31; // Order executed
    case 'C': return 36; // Order executed with price
    case 'X': return 23; // Order cancel
    case 'D': return 19; // Order delete
    case 'U': return 35; // Order replace
    case 'P': return 44; // Trade (non-cross)
    case 'Q': return 40; // Cross trade
    case 'B': return 19; // Broken trade
    case 'I': return 50; // NOII
    case 'N': return 20; // RPII
    case 'O': return 48; // Direct listing with capital raise
    default: return 0;
    }
}

ItchWriter& ItchWriter::tracking(std::uint16_t tracking_number) {
    tracking_ = tracking_number;
    return *this;
}

ItchWriter& ItchWriter::system_event(Timestamp ts, char event_code) {
    begin('S', 0, ts);
    put_char(event_code);
    end();
    return *this;
}

ItchWriter& ItchWriter::stock_directory(Timestamp ts, Locate loc, std::string_view symbol,
                                        const StockDirectoryFields& fields) {
    begin('R', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_char(fields.market_category);
    put_char(fields.financial_status);
    put_u32(fields.round_lot_size);
    put_char(fields.round_lots_only);
    put_char(fields.issue_classification);
    put_alpha(fields.issue_sub_type, 2);
    put_char(fields.authenticity);
    put_char(fields.short_sale_threshold);
    put_char(fields.ipo_flag);
    put_char(fields.luld_tier);
    put_char(fields.etp_flag);
    put_u32(fields.etp_leverage_factor);
    put_char(fields.inverse_indicator);
    end();
    return *this;
}

ItchWriter& ItchWriter::trading_action(Timestamp ts, Locate loc, std::string_view symbol,
                                       char state, std::string_view reason) {
    begin('H', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_char(state);
    put_char(' '); // Reserved
    put_alpha(reason, 4);
    end();
    return *this;
}

ItchWriter& ItchWriter::reg_sho(Timestamp ts, Locate loc, std::string_view symbol, char action) {
    begin('Y', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_char(action);
    end();
    return *this;
}

ItchWriter& ItchWriter::participant_position(Timestamp ts, Locate loc, std::string_view mpid,
                                             std::string_view symbol, char primary_market_maker,
                                             char market_maker_mode, char participant_state) {
    begin('L', loc, ts);
    put_alpha(mpid, kMpidWidth);
    put_alpha(symbol, kSymbolWidth);
    put_char(primary_market_maker);
    put_char(market_maker_mode);
    put_char(participant_state);
    end();
    return *this;
}

ItchWriter& ItchWriter::mwcb_decline_level(Timestamp ts, std::uint64_t level1, std::uint64_t level2,
                                           std::uint64_t level3) {
    begin('V', 0, ts);
    put_u64(level1);
    put_u64(level2);
    put_u64(level3);
    end();
    return *this;
}

ItchWriter& ItchWriter::mwcb_status(Timestamp ts, char breached_level) {
    begin('W', 0, ts);
    put_char(breached_level);
    end();
    return *this;
}

ItchWriter& ItchWriter::ipo_quoting_period(Timestamp ts, Locate loc, std::string_view symbol,
                                           std::uint32_t release_time_s, char qualifier,
                                           Price ipo_price) {
    begin('K', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_u32(release_time_s);
    put_char(qualifier);
    put_u32(ipo_price);
    end();
    return *this;
}

ItchWriter& ItchWriter::luld_auction_collar(Timestamp ts, Locate loc, std::string_view symbol,
                                            Price reference, Price upper, Price lower,
                                            std::uint32_t extension) {
    begin('J', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_u32(reference);
    put_u32(upper);
    put_u32(lower);
    put_u32(extension);
    end();
    return *this;
}

ItchWriter& ItchWriter::operational_halt(Timestamp ts, Locate loc, std::string_view symbol,
                                         char market_code, char action) {
    begin('h', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_char(market_code);
    put_char(action);
    end();
    return *this;
}

ItchWriter& ItchWriter::add(Timestamp ts, Locate loc, OrderRef ref, Side side, Qty qty,
                            std::string_view symbol, Price px) {
    begin('A', loc, ts);
    put_u64(ref);
    put_side(side);
    put_u32(qty);
    put_alpha(symbol, kSymbolWidth);
    put_u32(px);
    end();
    return *this;
}

ItchWriter& ItchWriter::add_mpid(Timestamp ts, Locate loc, OrderRef ref, Side side, Qty qty,
                                 std::string_view symbol, Price px, std::string_view mpid) {
    begin('F', loc, ts);
    put_u64(ref);
    put_side(side);
    put_u32(qty);
    put_alpha(symbol, kSymbolWidth);
    put_u32(px);
    put_alpha(mpid, kMpidWidth);
    end();
    return *this;
}

ItchWriter& ItchWriter::execute(Timestamp ts, Locate loc, OrderRef ref, Qty qty, MatchNo match) {
    begin('E', loc, ts);
    put_u64(ref);
    put_u32(qty);
    put_u64(match);
    end();
    return *this;
}

ItchWriter& ItchWriter::execute_with_price(Timestamp ts, Locate loc, OrderRef ref, Qty qty,
                                           MatchNo match, bool printable, Price px) {
    begin('C', loc, ts);
    put_u64(ref);
    put_u32(qty);
    put_u64(match);
    put_char(printable ? 'Y' : 'N');
    put_u32(px);
    end();
    return *this;
}

ItchWriter& ItchWriter::cancel(Timestamp ts, Locate loc, OrderRef ref, Qty qty) {
    begin('X', loc, ts);
    put_u64(ref);
    put_u32(qty);
    end();
    return *this;
}

ItchWriter& ItchWriter::del(Timestamp ts, Locate loc, OrderRef ref) {
    begin('D', loc, ts);
    put_u64(ref);
    end();
    return *this;
}

ItchWriter& ItchWriter::replace(Timestamp ts, Locate loc, OrderRef old_ref, OrderRef new_ref,
                                Qty qty, Price px) {
    begin('U', loc, ts);
    put_u64(old_ref);
    put_u64(new_ref);
    put_u32(qty);
    put_u32(px);
    end();
    return *this;
}

ItchWriter& ItchWriter::trade(Timestamp ts, Locate loc, Side side, Qty qty, std::string_view symbol,
                              Price px, MatchNo match) {
    begin('P', loc, ts);
    put_u64(0); // Order reference: always zero on the wire
    put_side(side);
    put_u32(qty);
    put_alpha(symbol, kSymbolWidth);
    put_u32(px);
    put_u64(match);
    end();
    return *this;
}

ItchWriter& ItchWriter::cross_trade(Timestamp ts, Locate loc, std::uint64_t shares,
                                    std::string_view symbol, Price px, MatchNo match,
                                    char cross_type) {
    begin('Q', loc, ts);
    put_u64(shares);
    put_alpha(symbol, kSymbolWidth);
    put_u32(px);
    put_u64(match);
    put_char(cross_type);
    end();
    return *this;
}

ItchWriter& ItchWriter::broken_trade(Timestamp ts, Locate loc, MatchNo match) {
    begin('B', loc, ts);
    put_u64(match);
    end();
    return *this;
}

ItchWriter& ItchWriter::noii(Timestamp ts, Locate loc, std::string_view symbol,
                             const NoiiFields& fields) {
    begin('I', loc, ts);
    put_u64(fields.paired_shares);
    put_u64(fields.imbalance_shares);
    put_char(fields.imbalance_direction);
    put_alpha(symbol, kSymbolWidth);
    put_u32(fields.far_price);
    put_u32(fields.near_price);
    put_u32(fields.reference_price);
    put_char(fields.cross_type);
    put_char(fields.price_variation);
    end();
    return *this;
}

ItchWriter& ItchWriter::rpii(Timestamp ts, Locate loc, std::string_view symbol,
                             char interest_flag) {
    begin('N', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_char(interest_flag);
    end();
    return *this;
}

ItchWriter& ItchWriter::direct_listing(Timestamp ts, Locate loc, std::string_view symbol,
                                       const DirectListingFields& fields) {
    begin('O', loc, ts);
    put_alpha(symbol, kSymbolWidth);
    put_char(fields.open_eligibility);
    put_u32(fields.min_allowable_price);
    put_u32(fields.max_allowable_price);
    put_u32(fields.near_execution_price);
    put_u64(fields.near_execution_time);
    put_u32(fields.lower_collar);
    put_u32(fields.upper_collar);
    end();
    return *this;
}

ItchWriter& ItchWriter::record(std::span<const std::byte> payload) {
    if (payload.size() > 0xFFFF) {
        throw std::invalid_argument("ItchWriter: record payload exceeds the 16-bit length field");
    }
    put_u16(static_cast<std::uint16_t>(payload.size()));
    raw(payload);
    ++message_count_;
    return *this;
}

ItchWriter& ItchWriter::raw(std::span<const std::byte> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return *this;
}

void ItchWriter::write_file(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("ItchWriter: cannot open " + path.string());
    }
    // The one reinterpret_cast in the writer: iostreams take char*, and reading any object as
    // char is well defined.
    out.write(reinterpret_cast<const char*>(buffer_.data()), // NOLINT(*-reinterpret-cast)
              static_cast<std::streamsize>(buffer_.size()));
    if (!out) {
        throw std::runtime_error("ItchWriter: write failed for " + path.string());
    }
}

void ItchWriter::begin(char type, Locate loc, Timestamp ts) {
    if (ts > kMaxWireTimestamp) {
        throw std::invalid_argument("ItchWriter: timestamp does not fit in 6 bytes");
    }
    record_start_ = buffer_.size();
    open_type_ = type;
    put_u16(static_cast<std::uint16_t>(message_length(type)));
    put_char(type);
    put_u16(loc);
    put_u16(tracking_);
    put_u48(ts);
}

void ItchWriter::end() {
    // Guards the writer itself: a field added or dropped by mistake changes the length.
    const std::size_t written = buffer_.size() - record_start_ - kLengthPrefixSize;
    if (written != message_length(open_type_)) {
        throw std::logic_error(std::string("ItchWriter: message '") + open_type_ + "' is " +
                               std::to_string(written) + " bytes, specification says " +
                               std::to_string(message_length(open_type_)));
    }
    ++message_count_;
}

void ItchWriter::put_u8(std::uint8_t value) {
    buffer_.push_back(static_cast<std::byte>(value));
}

void ItchWriter::put_u16(std::uint16_t value) {
    put_u8(static_cast<std::uint8_t>(value >> 8));
    put_u8(static_cast<std::uint8_t>(value & 0xFFU));
}

void ItchWriter::put_u32(std::uint32_t value) {
    put_u16(static_cast<std::uint16_t>(value >> 16));
    put_u16(static_cast<std::uint16_t>(value & 0xFFFFU));
}

void ItchWriter::put_u48(std::uint64_t value) {
    put_u16(static_cast<std::uint16_t>((value >> 32) & 0xFFFFU));
    put_u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
}

void ItchWriter::put_u64(std::uint64_t value) {
    put_u32(static_cast<std::uint32_t>(value >> 32));
    put_u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
}

void ItchWriter::put_char(char value) {
    put_u8(static_cast<std::uint8_t>(value));
}

void ItchWriter::put_alpha(std::string_view text, std::size_t width) {
    if (text.size() > width) {
        throw std::invalid_argument("ItchWriter: '" + std::string(text) + "' is longer than " +
                                    std::to_string(width) + " characters");
    }
    for (const char c : text) {
        put_char(c);
    }
    for (std::size_t i = text.size(); i < width; ++i) {
        put_char(' ');
    }
}

void ItchWriter::put_side(Side side) {
    put_char(side == Side::Buy ? 'B' : 'S');
}

} // namespace itchbt::test
