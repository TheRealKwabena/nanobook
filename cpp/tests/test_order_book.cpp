// Order book state machine.
//
// These are the tests that justify the project. Every case below is a real
// reconstruction bug that produces a book which looks plausible, passes an
// end-of-day sanity check, and silently poisons every feature derived from it.
#include "framework.hpp"
#include "nanobook/order_book.hpp"

using namespace nanobook;
using itch::Side;

namespace {

// $100.00 and $100.01 in ITCH's 4-implied-decimal integers.
constexpr itch::Price4 kP100_00 = 1000000;
constexpr itch::Price4 kP100_01 = 1000100;
constexpr itch::Price4 kP100_02 = 1000200;
constexpr itch::Price4 kP99_99  =  999900;

}  // namespace

NB_TEST(order_book, add_then_top_of_book) {
    OrderBook b;
    b.add_order(1, Side::Buy, 500, kP100_00);
    b.add_order(2, Side::Sell, 300, kP100_01);

    const BookTop t = b.top();
    CHECK(t.bid_valid && t.ask_valid);
    CHECK_EQ(t.bid_price, kP100_00);
    CHECK_EQ(t.ask_price, kP100_01);
    CHECK_EQ(t.bid_shares, 500);
    CHECK_EQ(t.ask_shares, 300);
    CHECK_EQ(t.bid_orders, 1u);

    std::int64_t spr = 0;
    CHECK(b.spread(spr));
    CHECK_EQ(spr, 100);  // one cent

    itch::Price4 m2 = 0;
    CHECK(b.mid_x2(m2));
    CHECK_EQ(m2, kP100_00 + kP100_01);  // held doubled so the half-cent survives
    CHECK(!b.is_crossed());
}

NB_TEST(order_book, aggregates_multiple_orders_at_one_level) {
    OrderBook b;
    b.add_order(1, Side::Buy, 100, kP100_00);
    b.add_order(2, Side::Buy, 200, kP100_00);
    b.add_order(3, Side::Buy, 300, kP100_00);
    const BookTop t = b.top();
    CHECK_EQ(t.bid_shares, 600);
    CHECK_EQ(t.bid_orders, 3u);
}

NB_TEST(order_book, best_price_moves_when_a_level_empties) {
    OrderBook b;
    b.add_order(1, Side::Buy, 100, kP100_00);
    b.add_order(2, Side::Buy, 100, kP99_99);
    CHECK_EQ(b.top().bid_price, kP100_00);
    b.delete_order(1);
    // Must fall through to the next live level, not report an empty book.
    CHECK_EQ(b.top().bid_price, kP99_99);
    CHECK_EQ(b.top().bid_shares, 100);
    b.delete_order(2);
    CHECK(!b.top().bid_valid);
}

// ---------------------------------------------------------------------------
// Trap 1: 'X' carries shares CANCELLED, not shares remaining.
// ---------------------------------------------------------------------------
NB_TEST(order_book, cancel_subtracts_the_cancelled_quantity) {
    OrderBook b;
    b.add_order(1, Side::Buy, 500, kP100_00);
    b.cancel_order(1, 200);  // 200 cancelled => 300 should remain

    CHECK_EQ(b.top().bid_shares, 300);
    // The order is still resting with its queue priority, so the level's order
    // count must NOT have decremented.
    CHECK_EQ(b.top().bid_orders, 1u);
    CHECK_EQ(b.orders().size(), 1u);
}

NB_TEST(order_book, cancel_to_zero_removes_the_order) {
    OrderBook b;
    b.add_order(1, Side::Buy, 500, kP100_00);
    b.cancel_order(1, 500);
    CHECK(!b.top().bid_valid);
    CHECK_EQ(b.orders().size(), 0u);  // must not leak a map entry
}

// ---------------------------------------------------------------------------
// Trap 2: a partial fill leaves the order resting.
// ---------------------------------------------------------------------------
NB_TEST(order_book, partial_fill_leaves_order_resting) {
    OrderBook b;
    b.add_order(1, Side::Sell, 500, kP100_01);
    b.execute_order(1, 150);
    CHECK_EQ(b.top().ask_shares, 350);
    CHECK_EQ(b.top().ask_orders, 1u);
    CHECK_EQ(b.orders().size(), 1u);

    b.execute_order(1, 350);  // remainder
    CHECK(!b.top().ask_valid);
    CHECK_EQ(b.orders().size(), 0u);
}

// ---------------------------------------------------------------------------
// Trap 3: 'C' (Executed With Price) must decrement the book at the ORDER's
// resting price, not at the price the trade printed at.
// ---------------------------------------------------------------------------
NB_TEST(order_book, executed_with_price_decrements_at_the_resting_price) {
    OrderBook b;
    b.add_order(1, Side::Buy, 400, kP100_00);
    b.add_order(2, Side::Buy, 400, kP99_99);

    // A 'C' message for order 1 printing at 99.99 — a different price from where
    // the order rests. The book must take the shares off 100.00.
    b.execute_order(1, 400);
    b.record_print(400, kP99_99);  // tape prints away from the book price

    CHECK_EQ(b.top().bid_price, kP99_99);   // 100.00 level is now gone
    CHECK_EQ(b.top().bid_shares, 400);      // 99.99 still holds its own 400
    CHECK_EQ(b.tape().last_price, kP99_99); // tape recorded the print price
}

// ---------------------------------------------------------------------------
// Trap 4: 'U' (Replace) changes the order reference. The old one must be retired
// or the order map leaks an entry per replace, all session long.
// ---------------------------------------------------------------------------
NB_TEST(order_book, replace_retires_the_old_reference) {
    OrderBook b;
    b.add_order(1, Side::Buy, 500, kP100_00);
    b.replace_order(1, 2, 300, kP99_99);

    CHECK_EQ(b.orders().size(), 1u);  // one live order, not two
    CHECK(b.orders_mutable().find(1) == nullptr);
    CHECK(b.orders_mutable().find(2) != nullptr);
    CHECK_EQ(b.top().bid_price, kP99_99);
    CHECK_EQ(b.top().bid_shares, 300);
    CHECK_EQ(b.top().bid_orders, 1u);
}

NB_TEST(order_book, replace_inherits_side_from_the_original_order) {
    // 'U' carries no buy/sell indicator. The side has to come from the original
    // entry, which means looking it up *before* erasing it.
    OrderBook b;
    b.add_order(1, Side::Sell, 500, kP100_02);
    b.replace_order(1, 2, 500, kP100_01);
    const BookTop t = b.top();
    CHECK(t.ask_valid);
    CHECK(!t.bid_valid);           // it must not have flipped to the bid side
    CHECK_EQ(t.ask_price, kP100_01);
}

NB_TEST(order_book, many_replaces_do_not_grow_the_order_map) {
    OrderBook b;
    b.add_order(1, Side::Buy, 100, kP100_00);
    itch::OrderRef cur = 1;
    for (itch::OrderRef nxt = 2; nxt < 20000; ++nxt) {
        b.replace_order(cur, nxt, 100, kP100_00);
        cur = nxt;
    }
    CHECK_EQ(b.orders().size(), 1u);
    CHECK_EQ(b.top().bid_orders, 1u);
    CHECK_EQ(b.top().bid_shares, 100);
}

// ---------------------------------------------------------------------------
// Trap 5: 'P' is an execution against a HIDDEN order. There is no book entry to
// decrement; touching the book here double-counts.
// ---------------------------------------------------------------------------
NB_TEST(order_book, hidden_trade_does_not_touch_the_book) {
    OrderBook b;
    b.add_order(1, Side::Buy, 500, kP100_00);
    b.add_order(2, Side::Sell, 500, kP100_01);
    const BookTop before = b.top();

    b.record_hidden_trade(300, kP100_00);

    const BookTop after = b.top();
    CHECK_EQ(after.bid_shares, before.bid_shares);
    CHECK_EQ(after.ask_shares, before.ask_shares);
    CHECK_EQ(after.bid_price, before.bid_price);
    CHECK_EQ(b.orders().size(), 2u);
    // ...but it IS on the tape.
    CHECK_EQ(b.tape().hidden_trades, 1u);
    CHECK_EQ(b.tape().hidden_shares, 300u);
}

// ---------------------------------------------------------------------------
// Feed defects must be counted, never papered over.
// ---------------------------------------------------------------------------
NB_TEST(order_book, unknown_references_are_counted) {
    OrderBook b;
    b.execute_order(999, 100);
    b.cancel_order(998, 100);
    b.delete_order(997);
    b.replace_order(996, 1000, 100, kP100_00);

    const auto d = b.diagnostics();
    CHECK_EQ(d.unknown_ref_exec, 1u);
    CHECK_EQ(d.unknown_ref_cancel, 1u);
    CHECK_EQ(d.unknown_ref_delete, 1u);
    CHECK_EQ(d.unknown_ref_replace, 1u);
    CHECK_EQ(d.live_orders, 0u);  // the failed replace must not have added one
}

NB_TEST(order_book, overfill_is_clamped_and_counted) {
    OrderBook b;
    b.add_order(1, Side::Buy, 100, kP100_00);
    b.execute_order(1, 500);  // feed claims more filled than was resting

    const auto d = b.diagnostics();
    CHECK_EQ(d.overfill, 1u);
    CHECK_EQ(d.negative_level_qty, 0u);  // clamped before it could go negative
    CHECK(!b.top().bid_valid);
    CHECK_EQ(d.live_orders, 0u);
}

NB_TEST(order_book, zero_share_add_is_rejected) {
    OrderBook b;
    b.add_order(1, Side::Buy, 0, kP100_00);
    CHECK(!b.top().bid_valid);
    CHECK_EQ(b.orders().size(), 0u);
}

NB_TEST(order_book, replace_to_zero_shares_just_removes) {
    OrderBook b;
    b.add_order(1, Side::Buy, 100, kP100_00);
    b.replace_order(1, 2, 0, kP100_00);
    CHECK(!b.top().bid_valid);
    CHECK_EQ(b.orders().size(), 0u);
}

// ---------------------------------------------------------------------------
// Depth and aggregates, which the research layer consumes.
// ---------------------------------------------------------------------------
NB_TEST(order_book, depth_walks_outward_from_the_touch) {
    OrderBook b;
    for (int i = 0; i < 5; ++i) {
        b.add_order(static_cast<itch::OrderRef>(10 + i), Side::Buy,
                    static_cast<itch::Shares>(100 * (i + 1)),
                    kP100_00 - static_cast<itch::Price4>(i) * 100);
        b.add_order(static_cast<itch::OrderRef>(20 + i), Side::Sell,
                    static_cast<itch::Shares>(50 * (i + 1)),
                    kP100_01 + static_cast<itch::Price4>(i) * 100);
    }

    LevelView bid[5], ask[5];
    CHECK_EQ(b.bids().top_of_book(bid, 5), 5u);
    CHECK_EQ(b.asks().top_of_book(ask, 5), 5u);

    // Bids descend from the touch, asks ascend.
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(bid[i].price, kP100_00 - static_cast<itch::Price4>(i) * 100);
        CHECK_EQ(bid[i].shares, 100 * (i + 1));
        CHECK_EQ(ask[i].price, kP100_01 + static_cast<itch::Price4>(i) * 100);
        CHECK_EQ(ask[i].shares, 50 * (i + 1));
    }

    // shares_within(0) is the touch only; within(2) is three price levels.
    CHECK_EQ(b.bids().shares_within(0), 100);
    CHECK_EQ(b.bids().shares_within(2), 100 + 200 + 300);
    CHECK_EQ(b.asks().shares_within(2), 50 + 100 + 150);
}

NB_TEST(order_book, tape_accumulates_volume_and_notional) {
    OrderBook b;
    b.add_order(1, Side::Buy, 1000, kP100_00);
    b.execute_order(1, 400);
    b.record_print(400, kP100_00);
    b.execute_order(1, 600);
    b.record_print(600, kP100_00);

    CHECK_EQ(b.tape().trades, 2u);
    CHECK_EQ(b.tape().shares_traded, 1000u);
    // 1000 shares at $100 == $100,000
    CHECK_EQ(b.tape().notional_dollars, 100000u);
}

NB_TEST(order_book, crossed_book_is_detected) {
    OrderBook b;
    b.add_order(1, Side::Buy, 100, kP100_02);
    b.add_order(2, Side::Sell, 100, kP100_00);  // ask below bid
    CHECK(b.is_crossed());
}
