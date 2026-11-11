#pragma once

// A handler that folds every decoded field into a checksum.
//
// Used where decoding must actually happen: the parse benchmark (a truly empty handler lets the
// compiler delete the decoders) and the fuzzer (so every field read reaches a sanitizer).

#include "itchbt/itch/messages.hpp"

#include <cstdint>

namespace itchbt::itch {

struct ChecksumHandler {
    std::uint64_t sum = 0;
    std::uint64_t messages = 0;

    void on(const SystemEvent& m) noexcept {
        mix(m.h);
        mix(static_cast<unsigned char>(m.code));
    }
    void on(const StockDirectory& m) noexcept {
        mix(m.h);
        mix(m.symbol);
        mix(m.round_lot_size);
        mix(m.etp_leverage_factor);
        mix(static_cast<unsigned char>(m.market_category));
        mix(static_cast<unsigned char>(m.inverse_indicator));
    }
    void on(const TradingAction& m) noexcept {
        mix(m.h);
        mix(m.symbol);
        mix(static_cast<unsigned char>(m.state));
    }
    void on(const AddOrder& m) noexcept {
        mix(m.h);
        mix(m.ref);
        mix(static_cast<std::uint64_t>(m.side));
        mix(m.qty);
        mix(m.symbol);
        mix(m.price);
    }
    void on(const OrderExecuted& m) noexcept {
        mix(m.h);
        mix(m.ref);
        mix(m.qty);
        mix(m.match);
    }
    void on(const OrderExecutedWithPrice& m) noexcept {
        mix(m.h);
        mix(m.ref);
        mix(m.qty);
        mix(m.match);
        mix(m.printable ? 1U : 0U);
        mix(m.price);
    }
    void on(const OrderCancel& m) noexcept {
        mix(m.h);
        mix(m.ref);
        mix(m.qty);
    }
    void on(const OrderDelete& m) noexcept {
        mix(m.h);
        mix(m.ref);
    }
    void on(const OrderReplace& m) noexcept {
        mix(m.h);
        mix(m.old_ref);
        mix(m.new_ref);
        mix(m.qty);
        mix(m.price);
    }
    void on(const Trade& m) noexcept {
        mix(m.h);
        mix(m.qty);
        mix(m.symbol);
        mix(m.price);
        mix(m.match);
    }
    void on(const CrossTrade& m) noexcept {
        mix(m.h);
        mix(m.shares);
        mix(m.symbol);
        mix(m.price);
        mix(m.match);
    }
    void on(const BrokenTrade& m) noexcept {
        mix(m.h);
        mix(m.match);
    }
    void on_skipped(const Header& h) noexcept { mix(h); }

private:
    void mix(std::uint64_t v) noexcept { sum = (sum ^ v) * 0x9E3779B97F4A7C15ULL; }
    void mix(const Header& h) noexcept {
        ++messages;
        mix(h.ts);
        mix(h.locate);
        mix(h.tracking);
        mix(static_cast<unsigned char>(h.type));
    }
    void mix(const Symbol& s) noexcept {
        std::uint64_t packed = 0;
        for (const char c : s.chars) {
            packed = (packed << 8) | static_cast<unsigned char>(c);
        }
        mix(packed);
    }
};

} // namespace itchbt::itch
