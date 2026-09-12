// nanobook — itch_spec.hpp
//
// NASDAQ TotalView-ITCH 5.0 wire format.
//
// Every message shares an 11-byte header:
//
//   off  len  field
//   ---  ---  --------------------------------------------------------------
//     0    1  message type (ASCII)
//     1    2  stock locate    — per-session integer id for the symbol. Indexing
//                               an array by this is far cheaper than hashing the
//                               8-byte ticker, so the book router uses it.
//     3    2  tracking number — NASDAQ internal; ignored.
//     5    6  timestamp       — nanoseconds since midnight US/Eastern.
//
// Body layouts follow, one view struct per message type we care about. Each view
// is a non-owning wrapper over a pointer into the mmap'd feed: constructing one
// is free, and field accessors compile to a single unaligned load + byte swap.
// Nothing here allocates or copies.
//
// Reference: Nasdaq TotalView-ITCH 5.0 specification, 2023-07 revision.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "nanobook/byte_order.hpp"

namespace nanobook::itch {

// ---------------------------------------------------------------------------
// Scalar types
// ---------------------------------------------------------------------------

// ITCH prices are uint32 with 4 implied decimal places: 1234500 == $123.4500.
// We keep them in that integer form end-to-end and never convert to double
// inside the engine — a double cannot represent 0.01 exactly, and an order book
// that rounds is an order book that silently loses shares.
using Price4 = std::uint32_t;

using OrderRef = std::uint64_t;
using Shares   = std::uint32_t;
using Locate   = std::uint16_t;
using Nanos    = std::uint64_t;

inline constexpr Price4 kPriceScale = 10000;

// A price field of all-ones means "market order / no price" in several messages.
inline constexpr Price4 kNoPrice = 0xFFFFFFFFu;

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] inline Side side_from_char(char c) noexcept {
    return c == 'B' ? Side::Buy : Side::Sell;
}

[[nodiscard]] inline char side_to_char(Side s) noexcept {
    return s == Side::Buy ? 'B' : 'S';
}

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------

enum class MsgType : char {
    SystemEvent             = 'S',
    StockDirectory          = 'R',
    StockTradingAction      = 'H',
    RegSHORestriction       = 'Y',
    MarketParticipantPos    = 'L',
    MwcbDeclineLevel        = 'V',
    MwcbStatus              = 'W',
    IpoQuotingPeriodUpdate  = 'K',
    LuldAuctionCollar       = 'J',
    OperationalHalt         = 'h',
    AddOrder                = 'A',
    AddOrderMpid            = 'F',
    OrderExecuted           = 'E',
    OrderExecutedWithPrice  = 'C',
    OrderCancel             = 'X',
    OrderDelete             = 'D',
    OrderReplace            = 'U',
    TradeNonCross           = 'P',
    CrossTrade              = 'Q',
    BrokenTrade             = 'B',
    Noii                    = 'I',
    Rpii                    = 'N',
};

inline constexpr std::size_t kHeaderLen = 11;

// Wire length of every ITCH 5.0 message, indexed by message type byte.
// Returns 0 for unknown types.
//
// We need this even for messages we do not decode: the BinaryFILE framing gives
// us a length prefix, but a *stream* (MoldUDP64 / raw TCP) does not, so correct
// framing depends on the table. Keeping it complete also lets the parser assert
// that the framed length matches the spec length, which catches a truncated or
// misaligned file immediately rather than 40 million messages later.
[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        default:  return 0;
    }
}

// ---------------------------------------------------------------------------
// Message views
//
// Each view stores only a pointer. `p_` points at the message type byte, so all
// documented spec offsets are used verbatim below — no off-by-header arithmetic
// to get wrong when cross-checking against the PDF.
// ---------------------------------------------------------------------------

class MsgView {
  public:
    explicit MsgView(const std::byte* p) noexcept : p_(p) {}

    [[nodiscard]] char      type() const noexcept { return static_cast<char>(p_[0]); }
    [[nodiscard]] Locate    locate() const noexcept { return load_be16(p_ + 1); }
    [[nodiscard]] Nanos     timestamp() const noexcept { return load_be48(p_ + 5); }
    [[nodiscard]] const std::byte* raw() const noexcept { return p_; }

  protected:
    // Fixed-width ASCII fields are space-padded on the right. Return a trimmed
    // view into the mmap — no copy, no allocation. Valid as long as the feed is
    // mapped, which outlives every callback.
    [[nodiscard]] std::string_view ascii(std::size_t off, std::size_t len) const noexcept {
        const auto* s = reinterpret_cast<const char*>(p_ + off);
        while (len > 0 && s[len - 1] == ' ') --len;
        return {s, len};
    }

    const std::byte* p_;
};

// 'S' — System Event. Marks start/end of messages, system hours, market hours.
class SystemEvent : public MsgView {
  public:
    using MsgView::MsgView;
    // 'O' start of messages, 'S' start of system hours, 'Q' start of market
    // hours, 'M' end of market hours, 'E' end of system hours, 'C' end of
    // messages. The book is only meaningful between 'Q' and 'M'.
    [[nodiscard]] char event_code() const noexcept { return static_cast<char>(p_[11]); }
};

// 'R' — Stock Directory. One per symbol at start of day; this is how a locate
// id becomes a ticker.
class StockDirectory : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] std::string_view stock() const noexcept { return ascii(11, 8); }
    [[nodiscard]] char market_category() const noexcept { return static_cast<char>(p_[19]); }
    [[nodiscard]] char financial_status() const noexcept { return static_cast<char>(p_[20]); }
    [[nodiscard]] std::uint32_t round_lot_size() const noexcept { return load_be32(p_ + 21); }
    [[nodiscard]] bool round_lots_only() const noexcept { return static_cast<char>(p_[25]) == 'Y'; }
    [[nodiscard]] char issue_classification() const noexcept { return static_cast<char>(p_[26]); }
    [[nodiscard]] std::string_view issue_subtype() const noexcept { return ascii(27, 2); }
    [[nodiscard]] char authenticity() const noexcept { return static_cast<char>(p_[29]); }
    [[nodiscard]] char short_sale_threshold() const noexcept { return static_cast<char>(p_[30]); }
    [[nodiscard]] char ipo_flag() const noexcept { return static_cast<char>(p_[31]); }
    [[nodiscard]] char luld_tier() const noexcept { return static_cast<char>(p_[32]); }
    [[nodiscard]] char etp_flag() const noexcept { return static_cast<char>(p_[33]); }
    [[nodiscard]] std::uint32_t etp_leverage() const noexcept { return load_be32(p_ + 34); }
    [[nodiscard]] bool inverse() const noexcept { return static_cast<char>(p_[38]) == 'Y'; }
};

// 'H' — Stock Trading Action. Halts and resumes.
class StockTradingAction : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] std::string_view stock() const noexcept { return ascii(11, 8); }
    // 'H' halted, 'P' paused, 'Q' quotation-only, 'T' trading.
    [[nodiscard]] char trading_state() const noexcept { return static_cast<char>(p_[19]); }
    // p_[20] is reserved.
    [[nodiscard]] std::string_view reason() const noexcept { return ascii(21, 4); }
};

// 'A' — Add Order, no MPID attribution. The workhorse: ~40% of a session's
// messages, and the only message that introduces liquidity into the book.
class AddOrder : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] OrderRef order_ref() const noexcept { return load_be64(p_ + 11); }
    [[nodiscard]] Side     side() const noexcept { return side_from_char(static_cast<char>(p_[19])); }
    [[nodiscard]] Shares   shares() const noexcept { return load_be32(p_ + 20); }
    [[nodiscard]] std::string_view stock() const noexcept { return ascii(24, 8); }
    [[nodiscard]] Price4   price() const noexcept { return load_be32(p_ + 32); }
};

// 'F' — Add Order with MPID. Identical prefix to 'A' plus a 4-char attribution,
// so it inherits and only adds the extra field.
class AddOrderMpid : public AddOrder {
  public:
    using AddOrder::AddOrder;
    [[nodiscard]] std::string_view attribution() const noexcept { return ascii(36, 4); }
};

// 'E' — Order Executed. A resting order traded at its displayed price.
class OrderExecuted : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] OrderRef order_ref() const noexcept { return load_be64(p_ + 11); }
    [[nodiscard]] Shares   executed_shares() const noexcept { return load_be32(p_ + 19); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return load_be64(p_ + 23); }
};

// 'C' — Order Executed With Price. Same, but the trade printed at a price other
// than the order's display price (e.g. a cross), so the tape price differs from
// the book price. `printable() == false` means do not include it in volume/VWAP.
class OrderExecutedWithPrice : public OrderExecuted {
  public:
    using OrderExecuted::OrderExecuted;
    [[nodiscard]] bool   printable() const noexcept { return static_cast<char>(p_[31]) == 'Y'; }
    [[nodiscard]] Price4 execution_price() const noexcept { return load_be32(p_ + 32); }
};

// 'X' — Order Cancel. A *partial* reduction. Note the trap: this carries the
// number of shares cancelled, not the shares remaining.
class OrderCancel : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] OrderRef order_ref() const noexcept { return load_be64(p_ + 11); }
    [[nodiscard]] Shares   cancelled_shares() const noexcept { return load_be32(p_ + 19); }
};

// 'D' — Order Delete. Full removal of whatever remains.
class OrderDelete : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] OrderRef order_ref() const noexcept { return load_be64(p_ + 11); }
};

// 'U' — Order Replace. Atomic delete + add that also *changes the order
// reference*. The old ref is retired and must never be reused; queue priority
// is lost. Getting this wrong leaks order-map entries all day.
class OrderReplace : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] OrderRef original_order_ref() const noexcept { return load_be64(p_ + 11); }
    [[nodiscard]] OrderRef new_order_ref() const noexcept { return load_be64(p_ + 19); }
    [[nodiscard]] Shares   shares() const noexcept { return load_be32(p_ + 27); }
    [[nodiscard]] Price4   price() const noexcept { return load_be32(p_ + 31); }
};

// 'P' — Trade (non-cross). A trade against a *hidden* order, so there is no
// corresponding book entry to decrement. Use for tape/volume only; touching the
// book here would double-count.
class TradeNonCross : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] OrderRef order_ref() const noexcept { return load_be64(p_ + 11); }
    [[nodiscard]] Side     side() const noexcept { return side_from_char(static_cast<char>(p_[19])); }
    [[nodiscard]] Shares   shares() const noexcept { return load_be32(p_ + 20); }
    [[nodiscard]] std::string_view stock() const noexcept { return ascii(24, 8); }
    [[nodiscard]] Price4   price() const noexcept { return load_be32(p_ + 32); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return load_be64(p_ + 36); }
};

// 'Q' — Cross Trade. Opening/closing/halt auction print. Shares is 8 bytes here.
class CrossTrade : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] std::uint64_t shares() const noexcept { return load_be64(p_ + 11); }
    [[nodiscard]] std::string_view stock() const noexcept { return ascii(19, 8); }
    [[nodiscard]] Price4 cross_price() const noexcept { return load_be32(p_ + 27); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return load_be64(p_ + 31); }
    // 'O' opening, 'C' closing, 'H' halt/IPO, 'I' intraday.
    [[nodiscard]] char cross_type() const noexcept { return static_cast<char>(p_[39]); }
};

// 'B' — Broken Trade. The print is busted; a backtest that already acted on the
// trade must unwind it.
class BrokenTrade : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] std::uint64_t match_number() const noexcept { return load_be64(p_ + 11); }
};

// 'I' — Net Order Imbalance Indicator. Auction-only; rich alpha for open/close
// strategies, which is why it is decoded even though the book ignores it.
class Noii : public MsgView {
  public:
    using MsgView::MsgView;
    [[nodiscard]] std::uint64_t paired_shares() const noexcept { return load_be64(p_ + 11); }
    [[nodiscard]] std::uint64_t imbalance_shares() const noexcept { return load_be64(p_ + 19); }
    [[nodiscard]] char imbalance_direction() const noexcept { return static_cast<char>(p_[27]); }
    [[nodiscard]] std::string_view stock() const noexcept { return ascii(28, 8); }
    [[nodiscard]] Price4 far_price() const noexcept { return load_be32(p_ + 36); }
    [[nodiscard]] Price4 near_price() const noexcept { return load_be32(p_ + 40); }
    [[nodiscard]] Price4 reference_price() const noexcept { return load_be32(p_ + 44); }
    [[nodiscard]] char cross_type() const noexcept { return static_cast<char>(p_[48]); }
    [[nodiscard]] char price_variation_indicator() const noexcept { return static_cast<char>(p_[49]); }
};

}  // namespace nanobook::itch
