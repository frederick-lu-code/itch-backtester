#pragma once

// On-disk record layouts for a run directory.
//
// Each file is a 24-byte FileHeader followed by fixed-width little-endian records. The same
// layouts are declared as NumPy dtypes in analysis/schema.py, and a test checks the two agree
// on every field offset and size.
//
// The structs use natural alignment with explicit padding fields, so they are "packed" in the
// sense that matters (no compiler-inserted gaps, checked by static_assert) without needing
// #pragma pack, which would make every field access unaligned.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace itchbt::record {

static_assert(std::endian::native == std::endian::little,
              "run files are little-endian and are written by copying structs");

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::array<char, 8> kMagic = {'I', 'T', 'C', 'H', 'B', 'T', '\0', '\0'};

enum class RecordType : std::uint32_t { Order = 1, Fill = 2, Bbo = 3, Risk = 4 };

struct FileHeader {
    std::array<char, 8> magic = kMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t record_type = 0;
    std::uint32_t record_size = 0;
    std::uint32_t reserved = 0;
};

/// What happened to an order. One record per event; an order has several over its life.
enum class OrderEventKind : std::uint8_t {
    Submitted = 0, // the strategy sent it (time: when it was sent)
    Accepted = 1,  // live on the simulated exchange
    Fill = 2,
    CancelRequested = 3, // the strategy asked to cancel (time: when it asked)
    Cancelled = 4,
    Rejected = 5,
    CancelRejected = 6,
};

/// orders.bin
struct OrderRecord {
    std::uint64_t ts = 0; // simulation time, nanoseconds since midnight
    std::uint64_t order_id = 0;
    std::uint64_t shares_ahead = 0; // queue position when accepted (Accepted only)
    std::uint32_t price = 0;        // limit price, 1/10000 USD
    std::uint32_t qty = 0;          // event quantity: order size, fill size or cancelled size
    std::uint32_t leaves = 0;       // shares still working after the event
    std::uint16_t locate = 0;
    std::uint8_t event = 0;      // OrderEventKind
    std::uint8_t state = 0;      // sim::OrderState after the event
    std::uint8_t side = 0;       // 0 buy, 1 sell
    std::uint8_t order_type = 0; // 0 limit, 1 IOC
    std::uint8_t reason = 0;     // sim::RejectReason, for Rejected and CancelRejected
    std::array<std::uint8_t, 5> pad{};
};

/// fills.bin
struct FillRecord {
    std::uint64_t ts = 0;
    std::uint64_t order_id = 0;
    std::uint64_t shares_ahead_at_arrival = 0;
    std::uint32_t price = 0; // fill price
    std::uint32_t qty = 0;
    std::uint32_t leaves = 0;
    std::uint32_t order_price = 0; // the order's limit price
    std::uint16_t locate = 0;
    std::uint8_t side = 0;
    std::uint8_t aggressive = 0; // 1 if the order took liquidity on arrival
    std::array<std::uint8_t, 4> pad{};
};

/// bbo.bin: every top-of-book change for the symbols the strategy subscribed to.
struct BboRecord {
    std::uint64_t ts = 0;
    std::uint64_t bid_qty = 0;
    std::uint64_t ask_qty = 0;
    std::uint32_t bid_price = 0;
    std::uint32_t ask_price = 0;
    std::uint16_t locate = 0;
    std::uint8_t flags = 0; // kBboHasBid | kBboHasAsk | kBboTradable
    std::array<std::uint8_t, 5> pad{};
};

inline constexpr std::uint8_t kBboHasBid = 1;
inline constexpr std::uint8_t kBboHasAsk = 2;
/// The symbol could trade at that moment: session open (or extended hours on) and not halted.
/// Positions are marked to the mid only on records with this bit.
inline constexpr std::uint8_t kBboTradable = 4;

enum class RiskEventKind : std::uint8_t { Reject = 0, KillSwitch = 1 };

/// risk.bin
struct RiskRecord {
    std::uint64_t ts = 0;
    std::uint64_t order_id = 0; // Reject only
    std::int64_t value = 0;     // KillSwitch: total PnL x 2 at the trip, price units x shares
    std::uint16_t locate = 0;
    std::uint8_t event = 0;  // RiskEventKind
    std::uint8_t reason = 0; // Reject: sim::RejectReason. KillSwitch: risk::KillReason
    std::array<std::uint8_t, 4> pad{};
};

static_assert(sizeof(FileHeader) == 24);
static_assert(sizeof(OrderRecord) == 48);
static_assert(sizeof(FillRecord) == 48);
static_assert(sizeof(BboRecord) == 40);
static_assert(sizeof(RiskRecord) == 32);
static_assert(std::is_trivially_copyable_v<OrderRecord> &&
              std::has_unique_object_representations_v<OrderRecord>);
static_assert(std::is_trivially_copyable_v<FillRecord> &&
              std::has_unique_object_representations_v<FillRecord>);
static_assert(std::is_trivially_copyable_v<BboRecord> &&
              std::has_unique_object_representations_v<BboRecord>);
static_assert(std::is_trivially_copyable_v<RiskRecord> &&
              std::has_unique_object_representations_v<RiskRecord>);

} // namespace itchbt::record
