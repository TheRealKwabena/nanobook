// nanobook — iex_spec.hpp
//
// IEX DEEP 1.0 wire format (Investors Exchange DEEP Specification v1.08).
//
// Why a second protocol: NASDAQ TotalView costs thousands a month, so ITCH work
// runs on one-off historical samples. IEX publishes its full depth-of-book feed
// free on a T+1 basis, which makes a *daily* pipeline possible. Supporting both
// also proves the book data structures are not welded to one exchange's format.
//
// HOW DEEP DIFFERS FROM ITCH — all four of these are silent-corruption traps:
//
//  1. BYTE ORDER. DEEP is little-endian. ITCH is big-endian. On arm64/x86 the LE
//     decode is a bare unaligned load with no swap.
//
//  2. PRICES ARE SIGNED 64-BIT. ITCH uses uint32. Same 4 implied decimals, but a
//     DEEP price must be range-checked before it can index a ladder, and several
//     fields legitimately carry 0 to mean "no price".
//
//  3. UPDATES ARE ABSOLUTE, NOT DELTAS. A Price Level Update carries the
//     *aggregate size at that price after the update*; size 0 removes the level.
//     ITCH sends add/cancel/execute deltas that the book accumulates. Routing a
//     DEEP message through delta logic diverges from the real book in seconds.
//     This is what PriceLadder::set_level exists for.
//
//  4. THE BOOK IS ONLY VALID AT TRANSACTION BOUNDARIES. See Event Flags below.
//     This one has no ITCH equivalent at all and is the subtlest of the four.
//
// Also structurally different: DEEP is *aggregated*. It carries no order
// references and no per-level order counts — "does not indicate the number or
// size of individual orders at any price level" — so there is no L3, no queue
// position, and OrderMap is unused on this path. Non-displayed orders and the
// reserve portions of orders do not appear at all.
//
// SYMBOLS, NOT LOCATES. Every DEEP message carries an 8-byte ticker string; there
// is no per-session integer id like ITCH's stock locate. So symbol filtering is
// an 8-byte compare rather than an integer compare. The compare is done against
// a pre-padded 8-byte key so it is a single 64-bit word comparison, not a strcmp.
#pragma once

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <string_view>

#include "nanobook/byte_order.hpp"
#include "nanobook/types.hpp"

namespace nanobook::iex {

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------
enum class MsgType : std::uint8_t {
    SystemEvent              = 0x53,  // 'S'
    SecurityDirectory        = 0x44,  // 'D'
    TradingStatus            = 0x48,  // 'H'
    RetailLiquidityIndicator = 0x49,  // 'I'
    OperationalHaltStatus    = 0x4f,  // 'O'
    ShortSalePriceTestStatus = 0x50,  // 'P'
    SecurityEvent            = 0x45,  // 'E'
    PriceLevelUpdateBuy      = 0x38,  // '8'
    PriceLevelUpdateSell     = 0x35,  // '5'
    TradeReport              = 0x54,  // 'T'
    OfficialPrice            = 0x58,  // 'X'
    TradeBreak               = 0x42,  // 'B'
    AuctionInformation       = 0x41,  // 'A'
};

// Spec'd message data length, or 0 for a type we do not know.
//
// Unlike ITCH there is no framing that depends on this: IEX-TP prefixes every
// message with its own length. The table is used to *validate* the framed length,
// which catches a version mismatch (a DEEP 1.1 file fed to a 1.0 decoder) at the
// first message rather than as mysterious garbage hours in.
[[nodiscard]] constexpr std::size_t message_length(std::uint8_t type) noexcept {
    switch (type) {
        case 0x53: return 10;  // System Event
        case 0x44: return 31;  // Security Directory
        case 0x48: return 22;  // Trading Status
        case 0x49: return 18;  // Retail Liquidity Indicator
        case 0x4f: return 18;  // Operational Halt Status
        case 0x50: return 19;  // Short Sale Price Test Status
        case 0x45: return 18;  // Security Event
        case 0x38: return 30;  // Price Level Update, buy side
        case 0x35: return 30;  // Price Level Update, sell side
        case 0x54: return 38;  // Trade Report
        case 0x58: return 26;  // Official Price
        case 0x42: return 38;  // Trade Break
        case 0x41: return 80;  // Auction Information
        default:   return 0;
    }
}

// System Event codes.
inline constexpr std::uint8_t kStartOfMessages      = 0x4f;  // 'O'
inline constexpr std::uint8_t kStartOfSystemHours   = 0x53;  // 'S'
inline constexpr std::uint8_t kStartOfRegularHours  = 0x52;  // 'R'
inline constexpr std::uint8_t kEndOfRegularHours    = 0x4d;  // 'M'
inline constexpr std::uint8_t kEndOfSystemHours     = 0x45;  // 'E'
inline constexpr std::uint8_t kEndOfMessages        = 0x43;  // 'C'

// Security Event codes.
inline constexpr std::uint8_t kOpeningProcessComplete = 0x4f;  // 'O'
inline constexpr std::uint8_t kClosingProcessComplete = 0x43;  // 'C'

// ---------------------------------------------------------------------------
// An 8-byte symbol, comparable as one 64-bit word.
//
// DEEP has no locate, so every message must be matched against the watchlist by
// ticker. Padding the wanted symbols once at startup turns that into an integer
// compare per message instead of a bounded strcmp.
// ---------------------------------------------------------------------------
class Symbol8 {
  public:
    Symbol8() = default;

    // From a human ticker: left-justified, space-padded to 8 bytes.
    explicit Symbol8(std::string_view s) noexcept {
        char buf[8];
        for (std::size_t i = 0; i < 8; ++i) buf[i] = i < s.size() ? s[i] : ' ';
        std::memcpy(&word_, buf, 8);
    }

    // Straight from a message's symbol field.
    [[nodiscard]] static Symbol8 from_wire(const std::byte* p) noexcept {
        Symbol8 s;
        std::memcpy(&s.word_, p, 8);
        return s;
    }

    [[nodiscard]] bool operator==(const Symbol8& o) const noexcept { return word_ == o.word_; }
    [[nodiscard]] bool operator!=(const Symbol8& o) const noexcept { return word_ != o.word_; }
    [[nodiscard]] std::uint64_t word() const noexcept { return word_; }

    // Trimmed view into a caller-supplied buffer (the wire bytes are transient in
    // a streaming decoder, so this cannot hand back a pointer into them).
    [[nodiscard]] std::string_view str(char (&out)[9]) const noexcept {
        std::memcpy(out, &word_, 8);
        out[8] = '\0';
        std::size_t n = 8;
        while (n > 0 && out[n - 1] == ' ') --n;
        return {out, n};
    }

  private:
    std::uint64_t word_ = 0;
};

// ---------------------------------------------------------------------------
// Price conversion.
//
// DEEP prices are signed 64-bit. The ladder needs an unsigned 32-bit index base,
// and every real equity price fits (uint32 max at 4 decimals is $429,496.7295).
// Range-check rather than assume: a negative or oversized price means either a
// "no price" sentinel or a decode that has gone wrong, and both must be visible.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool price_to_p4(Price8 wire, Price4& out) noexcept {
    if (wire <= 0) return false;                                  // 0 == no price
    if (wire > static_cast<Price8>(0x7FFFFFFF)) return false;     // absurd; also
                                                                  // keeps the side
                                                                  // bit in OrderEntry
                                                                  // free
    out = static_cast<Price4>(wire);
    return true;
}

// ---------------------------------------------------------------------------
// Message views. Non-owning wrappers over a pointer to the message's first byte,
// so documented spec offsets appear verbatim below.
// ---------------------------------------------------------------------------

class MsgView {
  public:
    explicit MsgView(const std::byte* p) noexcept : p_(p) {}

    [[nodiscard]] std::uint8_t type() const noexcept { return load_le8(p_); }
    // Nanoseconds since the POSIX epoch, UTC — NOT since midnight as in ITCH.
    // Signed on the wire; negative would mean pre-1970 and is nonsense here, but
    // the signedness is preserved so a corrupt field looks wrong instead of huge.
    [[nodiscard]] std::int64_t timestamp() const noexcept { return load_le64s(p_ + 2); }
    [[nodiscard]] const std::byte* raw() const noexcept { return p_; }

  protected:
    const std::byte* p_;
};

// Views for messages that carry a symbol at offset 10 (almost all of them).
class SymbolMsg : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] Symbol8 symbol() const noexcept { return Symbol8::from_wire(p_ + 10); }
};

// 'S' (0x53) — System Event. Market/feed-wide; carries no symbol.
class SystemEvent : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] std::uint8_t event() const noexcept { return load_le8(p_ + 1); }
};

// 'D' (0x44) — Security Directory.
class SecurityDirectory : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t flags() const noexcept { return load_le8(p_ + 1); }
    [[nodiscard]] Shares  round_lot_size() const noexcept { return load_le32(p_ + 18); }
    [[nodiscard]] Price8  adjusted_poc_price() const noexcept { return load_le64s(p_ + 22); }
    [[nodiscard]] std::uint8_t luld_tier() const noexcept { return load_le8(p_ + 30); }
};

// 'H' (0x48) — Trading Status. 'T' trading, 'H' halted, 'P' paused, 'O' order
// acceptance period.
class TradingStatus : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t status() const noexcept { return load_le8(p_ + 1); }
    [[nodiscard]] std::string_view reason(char (&out)[5]) const noexcept {
        std::memcpy(out, p_ + 18, 4);
        out[4] = '\0';
        std::size_t n = 4;
        while (n > 0 && out[n - 1] == ' ') --n;
        return {out, n};
    }
};

// 'O' (0x4f) — Operational Halt Status.
class OperationalHaltStatus : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t status() const noexcept { return load_le8(p_ + 1); }
};

// 'P' (0x50) — Short Sale Price Test Status (Reg SHO Rule 201).
class ShortSalePriceTestStatus : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] bool         in_effect() const noexcept { return load_le8(p_ + 1) != 0; }
    [[nodiscard]] std::uint8_t detail() const noexcept { return load_le8(p_ + 18); }
};

// 'I' (0x49) — Retail Liquidity Indicator.
class RetailLiquidityIndicator : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t indicator() const noexcept { return load_le8(p_ + 1); }
};

// 'E' (0x45) — Security Event. Receipt of one implies every preceding Price Level
// Update for that security has been transmitted, which is how the opening and
// closing processes are known to be complete.
class SecurityEvent : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t event() const noexcept { return load_le8(p_ + 1); }
};

// '8' (0x38) buy / '5' (0x35) sell — Price Level Update.
//
// The workhorse. `size` is the AGGREGATE size at this price after the update, not
// a delta; a size of 0 removes the level.
//
// EVENT FLAGS — the trap with no ITCH equivalent. A single order book event may
// need to change several price levels atomically (one aggressive order sweeping
// three levels, say). DEEP describes that as a run of PLUs with the flag OFF
// followed by one with the flag ON. The spec is explicit that the book retains
// its prior BBO throughout, and that an intermediate BBO "never truly existed".
//
// So any feature derived from the touch — spread, mid, imbalance — must only be
// sampled when `event_complete()` is true. Sampling mid-transition manufactures
// quotes that were never on the market, and they will look like real, tradeable
// mispricings to a backtest.
class PriceLevelUpdate : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;

    [[nodiscard]] Side side() const noexcept {
        return type() == static_cast<std::uint8_t>(MsgType::PriceLevelUpdateBuy) ? Side::Buy
                                                                                 : Side::Sell;
    }
    [[nodiscard]] std::uint8_t event_flags() const noexcept { return load_le8(p_ + 1); }
    // True on the final PLU of an atomic book transaction. Only then is the BBO
    // meaningful.
    [[nodiscard]] bool event_complete() const noexcept { return (event_flags() & 0x1) != 0; }

    [[nodiscard]] Shares size() const noexcept { return load_le32(p_ + 18); }
    [[nodiscard]] Price8 price() const noexcept { return load_le64s(p_ + 22); }
};

// 'T' (0x54) — Trade Report. One per individual fill, displayed or non-displayed.
// Routed executions are not reported.
class TradeReport : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t sale_condition_flags() const noexcept { return load_le8(p_ + 1); }
    [[nodiscard]] Shares       size() const noexcept { return load_le32(p_ + 18); }
    [[nodiscard]] Price8       price() const noexcept { return load_le64s(p_ + 22); }
    [[nodiscard]] std::int64_t trade_id() const noexcept { return load_le64s(p_ + 30); }

    // Sale condition flag bits (Appendix A). Only the ones that change how a
    // trade should be counted are named here.
    [[nodiscard]] bool intermarket_sweep() const noexcept {
        return (sale_condition_flags() & 0x80) != 0;
    }
    [[nodiscard]] bool extended_hours() const noexcept {
        return (sale_condition_flags() & 0x40) != 0;
    }
    [[nodiscard]] bool odd_lot() const noexcept { return (sale_condition_flags() & 0x20) != 0; }
    // Trades not subject to Rule 611 and single-price-cross trades are excluded
    // from last-sale and high/low by convention.
    [[nodiscard]] bool trade_through_exempt() const noexcept {
        return (sale_condition_flags() & 0x10) != 0;
    }
    [[nodiscard]] bool single_price_cross() const noexcept {
        return (sale_condition_flags() & 0x08) != 0;
    }
};

// 'B' (0x42) — Trade Break. Same layout as Trade Report; `trade_id` refers back
// to the broken print.
class TradeBreak : public TradeReport {
  public:
    using TradeReport::TradeReport;
};

// 'X' (0x58) — Official Price. 'Q' opening, 'M' closing.
class OfficialPrice : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t price_type() const noexcept { return load_le8(p_ + 1); }
    [[nodiscard]] Price8       price() const noexcept { return load_le64s(p_ + 18); }
};

// 'A' (0x41) — Auction Information. IEX-listed securities only, once a second
// during the lock-in window. Rich signal for open/close strategies.
class AuctionInformation : public SymbolMsg {
  public:
    using SymbolMsg::SymbolMsg;
    [[nodiscard]] std::uint8_t auction_type() const noexcept { return load_le8(p_ + 1); }
    [[nodiscard]] Shares  paired_shares() const noexcept { return load_le32(p_ + 18); }
    [[nodiscard]] Price8  reference_price() const noexcept { return load_le64s(p_ + 22); }
    [[nodiscard]] Price8  indicative_clearing_price() const noexcept { return load_le64s(p_ + 30); }
    [[nodiscard]] Shares  imbalance_shares() const noexcept { return load_le32(p_ + 38); }
    [[nodiscard]] std::uint8_t imbalance_side() const noexcept { return load_le8(p_ + 42); }
    [[nodiscard]] std::uint8_t extension_number() const noexcept { return load_le8(p_ + 43); }
    // Seconds since the epoch, not nanoseconds — the only Event Time field in DEEP.
    [[nodiscard]] std::uint32_t scheduled_auction_time() const noexcept { return load_le32(p_ + 44); }
    [[nodiscard]] Price8  auction_book_clearing_price() const noexcept { return load_le64s(p_ + 48); }
    [[nodiscard]] Price8  collar_reference_price() const noexcept { return load_le64s(p_ + 56); }
    [[nodiscard]] Price8  lower_auction_collar() const noexcept { return load_le64s(p_ + 64); }
    [[nodiscard]] Price8  upper_auction_collar() const noexcept { return load_le64s(p_ + 72); }
};

}  // namespace nanobook::iex
