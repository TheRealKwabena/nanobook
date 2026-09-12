// nanobook — order_book.hpp
//
// Single-symbol limit order book reconstructed from the ITCH order stream.
//
// ITCH is a *differential* feed: there is no periodic full-book snapshot during
// the session. The book you hold is the accumulated result of every add, cancel,
// execute and replace since the opening bell, which means a single mishandled
// message silently corrupts every feature derived from the book for the rest of
// the day — and nothing in the feed will tell you. The counters exposed at the
// bottom of this class exist so that corruption is loud instead of silent.
//
// The message semantics that are easy to get wrong, and how they are handled:
//
//   A / F  Add Order. Introduces displayed liquidity. 'F' only adds an MPID
//          attribution field; the book treatment is identical.
//
//   E      Order Executed. A resting order traded at its *displayed* price.
//          Decrement by executed_shares. If it reaches zero the order is gone:
//          retire it from the map and decrement the level's order count.
//
//   C      Order Executed With Price. Same book effect as 'E' — and this is the
//          trap: the book must be decremented at the *order's* price, while the
//          tape prints at execution_price. Using the execution price to locate
//          the level looks right and corrupts the book.
//
//   X      Order Cancel. Carries shares *cancelled*, not shares remaining. A
//          partial: the order stays resting with its queue priority intact, so
//          the level's order count does not change unless it hits zero.
//
//   D      Order Delete. Removes whatever remains.
//
//   U      Order Replace. An atomic delete + add that also *changes the order
//          reference*. The old reference is retired permanently and queue
//          priority is lost. Forgetting to erase the old ref leaks a map entry
//          per replace — and replaces are a large fraction of the feed, so the
//          table grows unbounded and lookups degrade all session.
//
//   P      Trade (non-cross). An execution against a *hidden* order. There is no
//          book entry to decrement, because the liquidity was never displayed.
//          Touching the book here double-counts volume; this is the single most
//          common reconstruction bug.
//
//   Q      Cross Trade. Auction print. Also does not touch the continuous book.
#pragma once

#include <cstdint>

#include "nanobook/itch_spec.hpp"
#include "nanobook/order_map.hpp"
#include "nanobook/price_ladder.hpp"

namespace nanobook {

struct BookTop {
    itch::Price4  bid_price = 0;
    itch::Price4  ask_price = 0;
    std::int64_t  bid_shares = 0;
    std::int64_t  ask_shares = 0;
    std::uint32_t bid_orders = 0;
    std::uint32_t ask_orders = 0;
    bool          bid_valid = false;
    bool          ask_valid = false;
};

// Running tape aggregates, kept alongside the book because every microstructure
// feature needs both sides of the story.
struct TapeStats {
    std::uint64_t trades = 0;
    std::uint64_t shares_traded = 0;
    // Notional in price4 * shares. Held as unsigned 128-bit-ish width via
    // uint64 of (price4 * shares) / 10000 to avoid overflow on a full session:
    // ~10^10 shares * ~10^6 price4 would overflow 64 bits otherwise.
    std::uint64_t notional_dollars = 0;
    std::uint64_t hidden_trades = 0;   // 'P' — executions against hidden orders
    std::uint64_t hidden_shares = 0;
    itch::Price4  last_price = 0;
    std::uint64_t last_ts = 0;
};

class OrderBook {
  public:
    // `expected_orders` should exceed the peak live resting-order count so the
    // order map never rehashes mid-session. 2^19 covers a busy large-cap name.
    explicit OrderBook(itch::Price4 tick = 100, std::size_t expected_orders = 1u << 19)
        : orders_(expected_orders),
          bids_(itch::Side::Buy, tick),
          asks_(itch::Side::Sell, tick) {}

    // ---------------- message handlers ----------------

    void add_order(itch::OrderRef ref, itch::Side side, itch::Shares shares, itch::Price4 price) {
        if (shares == 0) { ++zero_share_adds_; return; }
        orders_.insert(ref, price, side, shares);
        if (!ladder(side).add(price, shares)) ++unrepresentable_price_;
        ++adds_;
        last_ts_ = ts_;
    }

    // Shared by 'E' and 'C'. `book_price` is always the resting order's price,
    // never a 'C' message's execution price.
    void execute_order(itch::OrderRef ref, itch::Shares exec_shares) {
        OrderEntry* e = orders_.find(ref);
        if (e == nullptr) { ++unknown_ref_exec_; return; }

        const itch::Shares filled = std::min(exec_shares, e->shares);
        if (filled < exec_shares) ++overfill_;  // feed says more filled than rests

        e->shares -= filled;
        const bool gone = (e->shares == 0);
        ladder(e->side()).remove(e->price(), filled, gone);
        if (gone) orders_.erase(ref);
        ++executes_;
        last_ts_ = ts_;
    }

    void cancel_order(itch::OrderRef ref, itch::Shares cancelled_shares) {
        OrderEntry* e = orders_.find(ref);
        if (e == nullptr) { ++unknown_ref_cancel_; return; }

        const itch::Shares removed = std::min(cancelled_shares, e->shares);
        if (removed < cancelled_shares) ++overcancel_;

        e->shares -= removed;
        const bool gone = (e->shares == 0);
        ladder(e->side()).remove(e->price(), removed, gone);
        if (gone) orders_.erase(ref);
        ++cancels_;
        last_ts_ = ts_;
    }

    void delete_order(itch::OrderRef ref) {
        OrderEntry* e = orders_.find(ref);
        if (e == nullptr) { ++unknown_ref_delete_; return; }
        ladder(e->side()).remove(e->price(), e->shares, /*order_gone=*/true);
        orders_.erase(ref);
        ++deletes_;
        last_ts_ = ts_;
    }

    // Atomic delete-then-add. The side is inherited from the original order:
    // 'U' does not carry a buy/sell indicator, so losing the original entry
    // means losing the side — another reason the old ref must be looked up
    // before it is erased.
    void replace_order(itch::OrderRef old_ref, itch::OrderRef new_ref,
                       itch::Shares shares, itch::Price4 price) {
        OrderEntry* e = orders_.find(old_ref);
        if (e == nullptr) { ++unknown_ref_replace_; return; }

        const itch::Side side = e->side();
        ladder(side).remove(e->price(), e->shares, /*order_gone=*/true);
        orders_.erase(old_ref);

        if (shares > 0) {
            orders_.insert(new_ref, price, side, shares);
            if (!ladder(side).add(price, shares)) ++unrepresentable_price_;
        }
        ++replaces_;
        last_ts_ = ts_;
    }

    // Tape-only. Deliberately does not touch the book: 'P' is an execution
    // against a hidden order that was never in the displayed book.
    void record_hidden_trade(itch::Shares shares, itch::Price4 price) {
        ++tape_.hidden_trades;
        tape_.hidden_shares += shares;
        record_print(shares, price);
    }

    // Call for printable 'E'/'C' executions to accumulate the tape.
    void record_print(itch::Shares shares, itch::Price4 price) {
        ++tape_.trades;
        tape_.shares_traded += shares;
        tape_.notional_dollars +=
            static_cast<std::uint64_t>(shares) * price / itch::kPriceScale;
        tape_.last_price = price;
        tape_.last_ts = ts_;
    }

    void set_timestamp(std::uint64_t ns) noexcept { ts_ = ns; }
    [[nodiscard]] std::uint64_t timestamp() const noexcept { return ts_; }

    // ---------------- book queries ----------------

    [[nodiscard]] BookTop top() const noexcept {
        BookTop t;
        LevelView lv{};
        if (bids_.best(lv)) {
            t.bid_valid = true; t.bid_price = lv.price;
            t.bid_shares = lv.shares; t.bid_orders = lv.order_count;
        }
        if (asks_.best(lv)) {
            t.ask_valid = true; t.ask_price = lv.price;
            t.ask_shares = lv.shares; t.ask_orders = lv.order_count;
        }
        return t;
    }

    // Twice the midpoint, kept integral: a one-cent spread has a midpoint on a
    // half-cent, and rounding it away biases every mid-reversion signal built on
    // top. Callers divide by 2 (or by 20000.0) at the last possible moment.
    [[nodiscard]] bool mid_x2(itch::Price4& out) const noexcept {
        const BookTop t = top();
        if (!t.bid_valid || !t.ask_valid) return false;
        out = t.bid_price + t.ask_price;
        return true;
    }

    [[nodiscard]] bool spread(std::int64_t& out) const noexcept {
        const BookTop t = top();
        if (!t.bid_valid || !t.ask_valid) return false;
        out = static_cast<std::int64_t>(t.ask_price) - static_cast<std::int64_t>(t.bid_price);
        return true;
    }

    // A locked (bid == ask) or crossed (bid > ask) book is legitimate but
    // transient on a single venue. Persistent crossing means reconstruction is
    // broken.
    [[nodiscard]] bool is_crossed() const noexcept {
        const BookTop t = top();
        return t.bid_valid && t.ask_valid && t.bid_price > t.ask_price;
    }

    [[nodiscard]] const PriceLadder& bids() const noexcept { return bids_; }
    [[nodiscard]] const PriceLadder& asks() const noexcept { return asks_; }
    [[nodiscard]] const OrderMap& orders() const noexcept { return orders_; }
    // OrderMap::find is non-const because it counts probes for benchmarking;
    // this exposes it for read-only price lookups on the hot path.
    [[nodiscard]] OrderMap& orders_mutable() noexcept { return orders_; }
    [[nodiscard]] const TapeStats& tape() const noexcept { return tape_; }

    void clear() {
        orders_.clear();
        bids_.clear();
        asks_.clear();
    }

    // ---------------- integrity counters ----------------
    //
    // Every one of these should be zero (or explainable) on a clean session file
    // that starts at the opening bell. Nonzero `unknown_ref_*` early in a file is
    // expected and harmless if the file begins mid-session, because the adds for
    // those orders happened before our first byte.

    struct Diagnostics {
        std::uint64_t adds, executes, cancels, deletes, replaces;
        std::uint64_t unknown_ref_exec, unknown_ref_cancel, unknown_ref_delete, unknown_ref_replace;
        std::uint64_t overfill, overcancel, zero_share_adds, unrepresentable_price;
        std::uint64_t negative_level_qty, missing_level, off_tick, duplicate_order_refs;
        std::size_t   live_orders, bid_levels, ask_levels;
        std::size_t   order_map_capacity, order_map_max_probe;
        double        order_map_load_factor, order_map_mean_probes;
    };

    [[nodiscard]] Diagnostics diagnostics() const {
        const double lookups = static_cast<double>(orders_.total_lookups());
        return Diagnostics{
            adds_, executes_, cancels_, deletes_, replaces_,
            unknown_ref_exec_, unknown_ref_cancel_, unknown_ref_delete_, unknown_ref_replace_,
            overfill_, overcancel_, zero_share_adds_, unrepresentable_price_,
            bids_.negative_qty_events() + asks_.negative_qty_events(),
            bids_.missing_level_events() + asks_.missing_level_events(),
            bids_.off_tick_prices() + asks_.off_tick_prices(),
            orders_.duplicate_inserts(),
            orders_.size(), bids_.level_count(), asks_.level_count(),
            orders_.capacity(), orders_.max_probe_run(),
            orders_.load_factor(),
            lookups > 0 ? static_cast<double>(orders_.total_probes()) / lookups : 0.0,
        };
    }

  private:
    [[nodiscard]] PriceLadder& ladder(itch::Side s) noexcept {
        return s == itch::Side::Buy ? bids_ : asks_;
    }

    OrderMap    orders_;
    PriceLadder bids_;
    PriceLadder asks_;
    TapeStats   tape_;

    std::uint64_t ts_ = 0;
    std::uint64_t last_ts_ = 0;

    std::uint64_t adds_ = 0, executes_ = 0, cancels_ = 0, deletes_ = 0, replaces_ = 0;
    std::uint64_t unknown_ref_exec_ = 0, unknown_ref_cancel_ = 0;
    std::uint64_t unknown_ref_delete_ = 0, unknown_ref_replace_ = 0;
    std::uint64_t overfill_ = 0, overcancel_ = 0;
    std::uint64_t zero_share_adds_ = 0, unrepresentable_price_ = 0;
};

}  // namespace nanobook
