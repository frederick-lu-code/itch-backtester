#include "itchbt/record/schema.hpp"

#include <cstddef>
#include <initializer_list>

#include "itchbt/record/records.hpp"
#include "itchbt/sim/order_state.hpp"
#include "itchbt/sim/types.hpp"

namespace itchbt::record {

namespace {

struct Field {
    const char* name;
    std::size_t offset;
    std::size_t size;
};

// offsetof on these standard-layout structs is well defined; the macro keeps each field's
// name, offset and size together so none can be mistyped separately.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ITCHBT_FIELD(Record, member)                                                              \
    Field { #member, offsetof(Record, member), sizeof(Record::member) }

std::string record_json(const char* name, const char* file, RecordType type, std::size_t size,
                        std::initializer_list<Field> fields) {
    std::string json = std::string("    \"") + name + "\": {\"file\": \"" + file +
                       "\", \"type\": " + std::to_string(static_cast<unsigned>(type)) +
                       ", \"size\": " + std::to_string(size) + ", \"fields\": [";
    bool first = true;
    for (const Field& f : fields) {
        json += std::string(first ? "" : ", ") + "{\"name\": \"" + f.name +
                "\", \"offset\": " + std::to_string(f.offset) +
                ", \"size\": " + std::to_string(f.size) + "}";
        first = false;
    }
    return json + "]}";
}

template <typename Enum, typename Name>
std::string enum_json(const char* name, Enum last, Name&& name_of) {
    std::string json = std::string("    \"") + name + "\": [";
    for (unsigned v = 0; v <= static_cast<unsigned>(last); ++v) {
        json += std::string(v == 0 ? "\"" : ", \"") + name_of(static_cast<Enum>(v)) + "\"";
    }
    return json + "]";
}

const char* order_event_name(OrderEventKind kind) {
    switch (kind) {
    case OrderEventKind::Submitted: return "submitted";
    case OrderEventKind::Accepted: return "accepted";
    case OrderEventKind::Fill: return "fill";
    case OrderEventKind::CancelRequested: return "cancel_requested";
    case OrderEventKind::Cancelled: return "cancelled";
    case OrderEventKind::Rejected: return "rejected";
    case OrderEventKind::CancelRejected: return "cancel_rejected";
    }
    return "?";
}

} // namespace

std::string schema_json() {
    std::string json = "{\n  \"format_version\": " + std::to_string(kFormatVersion) +
                       ",\n  \"magic\": \"ITCHBT\",\n  \"header_size\": " +
                       std::to_string(sizeof(FileHeader)) + ",\n  \"records\": {\n";
    json += record_json("orders", "orders.bin", RecordType::Order, sizeof(OrderRecord),
                        {ITCHBT_FIELD(OrderRecord, ts), ITCHBT_FIELD(OrderRecord, order_id),
                         ITCHBT_FIELD(OrderRecord, shares_ahead), ITCHBT_FIELD(OrderRecord, price),
                         ITCHBT_FIELD(OrderRecord, qty), ITCHBT_FIELD(OrderRecord, leaves),
                         ITCHBT_FIELD(OrderRecord, locate), ITCHBT_FIELD(OrderRecord, event),
                         ITCHBT_FIELD(OrderRecord, state), ITCHBT_FIELD(OrderRecord, side),
                         ITCHBT_FIELD(OrderRecord, order_type), ITCHBT_FIELD(OrderRecord, reason),
                         ITCHBT_FIELD(OrderRecord, pad)}) +
            ",\n";
    json += record_json("fills", "fills.bin", RecordType::Fill, sizeof(FillRecord),
                        {ITCHBT_FIELD(FillRecord, ts), ITCHBT_FIELD(FillRecord, order_id),
                         ITCHBT_FIELD(FillRecord, shares_ahead_at_arrival),
                         ITCHBT_FIELD(FillRecord, price), ITCHBT_FIELD(FillRecord, qty),
                         ITCHBT_FIELD(FillRecord, leaves), ITCHBT_FIELD(FillRecord, order_price),
                         ITCHBT_FIELD(FillRecord, locate), ITCHBT_FIELD(FillRecord, side),
                         ITCHBT_FIELD(FillRecord, aggressive), ITCHBT_FIELD(FillRecord, pad)}) +
            ",\n";
    json += record_json("bbo", "bbo.bin", RecordType::Bbo, sizeof(BboRecord),
                        {ITCHBT_FIELD(BboRecord, ts), ITCHBT_FIELD(BboRecord, bid_qty),
                         ITCHBT_FIELD(BboRecord, ask_qty), ITCHBT_FIELD(BboRecord, bid_price),
                         ITCHBT_FIELD(BboRecord, ask_price), ITCHBT_FIELD(BboRecord, locate),
                         ITCHBT_FIELD(BboRecord, flags), ITCHBT_FIELD(BboRecord, pad)}) +
            ",\n";
    json += record_json("risk", "risk.bin", RecordType::Risk, sizeof(RiskRecord),
                        {ITCHBT_FIELD(RiskRecord, ts), ITCHBT_FIELD(RiskRecord, order_id),
                         ITCHBT_FIELD(RiskRecord, value), ITCHBT_FIELD(RiskRecord, locate),
                         ITCHBT_FIELD(RiskRecord, event), ITCHBT_FIELD(RiskRecord, reason),
                         ITCHBT_FIELD(RiskRecord, pad)}) +
            "\n  },\n  \"enums\": {\n";
    json += enum_json("order_event", OrderEventKind::CancelRejected, order_event_name) + ",\n";
    json += enum_json("order_state", sim::OrderState::Rejected,
                      [](sim::OrderState s) { return sim::to_string(s); }) +
            ",\n";
    json += enum_json("reject_reason", sim::RejectReason::TooLateToCancel,
                      [](sim::RejectReason r) { return sim::to_string(r); }) +
            ",\n";
    json += "    \"side\": [\"buy\", \"sell\"],\n    \"order_type\": [\"limit\", \"ioc\"],\n"
            "    \"risk_event\": [\"reject\", \"kill_switch\"],\n"
            "    \"kill_reason\": [\"none\", \"loss_limit\", \"reject_rate\"]\n  }\n}\n";
    return json;
}

} // namespace itchbt::record
