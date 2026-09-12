// nanobook — book_builder.hpp
//
// Glue between the ITCH parser and a single-symbol OrderBook.
//
// Symbol filtering uses the *stock locate* rather than the ticker string. The
// locate is a per-session uint16 assigned to each symbol in the Stock Directory
// ('R') messages at the head of the file; every subsequent message carries it in
// the common header. So after one string comparison at startup, filtering costs
// a single integer compare per message instead of an 8-byte memcmp — worth it at
// 300M messages a day.
//
// This also sidesteps a real trap: the order-modifying messages (E, C, X, D, U)
// do NOT carry the ticker at all. Only the order reference and the locate. A
// builder that tried to filter by ticker string would have to track every order
// reference in the file just to know which ones to ignore.
#pragma once

#include <string>
#include <string_view>

#include "nanobook/itch_parser.hpp"
#include "nanobook/order_book.hpp"

namespace nanobook {

// Observers see the book immediately after each message is applied. Default is
// a no-op that the compiler removes entirely.
struct NullObserver {
    void on_book_event(const OrderBook&, char /*msg_type*/, std::uint64_t /*ts*/) {}
};

template <class Observer = NullObserver>
class BookBuilder : public HandlerBase {
  public:
    BookBuilder(std::string symbol, Observer& obs, itch::Price4 tick = 100,
                std::size_t expected_orders = 1u << 19)
        : symbol_(std::move(symbol)), obs_(&obs), book_(tick, expected_orders) {}

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] OrderBook& book() noexcept { return book_; }
    [[nodiscard]] bool resolved() const noexcept { return locate_ != kUnresolved; }
    [[nodiscard]] int  locate() const noexcept { return locate_; }
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] std::uint64_t applied() const noexcept { return applied_; }
    [[nodiscard]] char session_state() const noexcept { return session_state_; }
    [[nodiscard]] char trading_state() const noexcept { return trading_state_; }

    // Pin the locate directly, for files that begin mid-session with no Stock
    // Directory block.
    void force_locate(itch::Locate l) noexcept { locate_ = static_cast<int>(l); }

    // ---- parser hooks ----

    void on_stock_directory(const itch::StockDirectory& m) {
        ++directory_entries_;
        if (m.stock() == std::string_view(symbol_)) {
            locate_ = static_cast<int>(m.locate());
            round_lot_ = m.round_lot_size();
        }
    }

    void on_system_event(const itch::SystemEvent& m) { session_state_ = m.event_code(); }

    void on_trading_action(const itch::StockTradingAction& m) {
        if (!mine(m)) return;
        trading_state_ = m.trading_state();
    }

    void on_add_order(const itch::AddOrder& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        book_.add_order(m.order_ref(), m.side(), m.shares(), m.price());
        emit('A', m.timestamp());
    }

    // 'F' carries the same fields plus an MPID; identical book effect.
    void on_add_order_mpid(const itch::AddOrderMpid& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        book_.add_order(m.order_ref(), m.side(), m.shares(), m.price());
        emit('F', m.timestamp());
    }

    void on_order_executed(const itch::OrderExecuted& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        // Capture the resting price before the execution possibly retires the
        // order, so the tape print records the price it actually traded at.
        const itch::Price4 px = resting_price(m.order_ref());
        book_.execute_order(m.order_ref(), m.executed_shares());
        book_.record_print(m.executed_shares(), px);
        emit('E', m.timestamp());
    }

    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        // The book decrements at the order's resting price (handled inside
        // execute_order via the order map); the tape prints at execution_price,
        // and only when the print is marked printable.
        book_.execute_order(m.order_ref(), m.executed_shares());
        if (m.printable()) book_.record_print(m.executed_shares(), m.execution_price());
        emit('C', m.timestamp());
    }

    void on_order_cancel(const itch::OrderCancel& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        book_.cancel_order(m.order_ref(), m.cancelled_shares());
        emit('X', m.timestamp());
    }

    void on_order_delete(const itch::OrderDelete& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        book_.delete_order(m.order_ref());
        emit('D', m.timestamp());
    }

    void on_order_replace(const itch::OrderReplace& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        book_.replace_order(m.original_order_ref(), m.new_order_ref(), m.shares(), m.price());
        emit('U', m.timestamp());
    }

    // Execution against a hidden order: tape only, never the book.
    void on_trade(const itch::TradeNonCross& m) {
        if (!mine(m)) return;
        book_.set_timestamp(m.timestamp());
        book_.record_hidden_trade(m.shares(), m.price());
        emit('P', m.timestamp());
    }

    void on_cross_trade(const itch::CrossTrade& m) {
        if (!mine(m)) return;
        ++crosses_;
        emit('Q', m.timestamp());
    }

    void on_noii(const itch::Noii& m) {
        if (!mine(m)) return;
        ++noii_;
    }

    [[nodiscard]] std::uint64_t crosses() const noexcept { return crosses_; }
    [[nodiscard]] std::uint64_t noii_messages() const noexcept { return noii_; }
    [[nodiscard]] std::uint64_t directory_entries() const noexcept { return directory_entries_; }
    [[nodiscard]] std::uint32_t round_lot() const noexcept { return round_lot_; }

  private:
    static constexpr int kUnresolved = -1;

    [[nodiscard]] bool mine(const itch::MsgView& m) const noexcept {
        return locate_ != kUnresolved && static_cast<int>(m.locate()) == locate_;
    }

    [[nodiscard]] itch::Price4 resting_price(itch::OrderRef ref) noexcept {
        const OrderEntry* e = book_.orders_mutable().find(ref);
        return e != nullptr ? e->price() : 0;
    }

    void emit(char t, std::uint64_t ts) {
        ++applied_;
        obs_->on_book_event(book_, t, ts);
    }

    std::string symbol_;
    Observer* obs_;
    OrderBook book_;
    int  locate_ = kUnresolved;
    char session_state_ = '?';
    char trading_state_ = '?';
    std::uint32_t round_lot_ = 100;
    std::uint64_t applied_ = 0;
    std::uint64_t crosses_ = 0;
    std::uint64_t noii_ = 0;
    std::uint64_t directory_entries_ = 0;
};

}  // namespace nanobook
