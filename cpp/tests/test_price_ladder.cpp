// Dense price ladder. The tricky parts are the regrow paths: a ladder that
// silently loses levels when the price moves outside its initial window would
// look fine on a quiet morning and corrupt the book on a volatile afternoon.
#include <map>
#include <random>

#include "framework.hpp"
#include "nanobook/price_ladder.hpp"

using namespace nanobook;
using itch::Side;

NB_TEST(price_ladder, add_and_best) {
    PriceLadder bids(Side::Buy);
    CHECK(bids.add(1000000, 100));
    CHECK(bids.add(1000100, 200));
    CHECK(bids.add(999900, 300));

    LevelView lv{};
    CHECK(bids.best(lv));
    CHECK_EQ(lv.price, 1000100u);   // highest bid
    CHECK_EQ(lv.shares, 200);
    CHECK_EQ(bids.level_count(), 3u);

    PriceLadder asks(Side::Sell);
    asks.add(1000000, 100);
    asks.add(1000100, 200);
    asks.add(999900, 300);
    CHECK(asks.best(lv));
    CHECK_EQ(lv.price, 999900u);    // lowest ask
}

NB_TEST(price_ladder, remove_retires_the_level_at_zero) {
    PriceLadder l(Side::Buy);
    l.add(1000000, 500);
    CHECK(l.remove(1000000, 200, false));
    CHECK_EQ(l.shares_at(1000000), 300);
    CHECK_EQ(l.level_count(), 1u);
    CHECK(l.remove(1000000, 300, true));
    CHECK_EQ(l.level_count(), 0u);
    LevelView lv{};
    CHECK(!l.best(lv));
}

NB_TEST(price_ladder, order_count_tracks_resting_orders) {
    PriceLadder l(Side::Buy);
    l.add(1000000, 100);
    l.add(1000000, 100);
    l.add(1000000, 100);
    LevelView lv{};
    CHECK(l.best(lv));
    CHECK_EQ(lv.order_count, 3u);

    l.remove(1000000, 50, false);   // partial cancel: order stays
    CHECK(l.best(lv));
    CHECK_EQ(lv.order_count, 3u);

    l.remove(1000000, 50, true);    // order fully gone
    CHECK(l.best(lv));
    CHECK_EQ(lv.order_count, 2u);
}

NB_TEST(price_ladder, grows_upward_preserving_contents) {
    PriceLadder l(Side::Buy);
    l.add(1000000, 111);
    const std::uint64_t regrows_before = l.regrow_count();

    // Far above the initial window: a $100 stock printing at $5,000.
    CHECK(l.add(50000000, 222));
    CHECK(l.regrow_count() > regrows_before);

    CHECK_EQ(l.shares_at(1000000), 111);   // old level survived the move
    CHECK_EQ(l.shares_at(50000000), 222);
    CHECK_EQ(l.level_count(), 2u);
    LevelView lv{};
    CHECK(l.best(lv));
    CHECK_EQ(lv.price, 50000000u);
}

NB_TEST(price_ladder, low_priced_name_needs_no_downward_regrow) {
    // For any stock under ~$655 the initial window clamps its base to zero, so
    // the whole plausible price range is already covered and grow_down never
    // fires. Worth pinning: it is why regrows are rare in practice.
    PriceLadder l(Side::Buy);
    l.add(1000000, 111);                    // $100
    const std::uint64_t regrows_before = l.regrow_count();
    CHECK(l.add(5000, 333));                // $0.50 — a collapse, still in window
    CHECK_EQ(l.regrow_count(), regrows_before);
    CHECK_EQ(l.shares_at(1000000), 111);
    CHECK_EQ(l.shares_at(5000), 333);
}

NB_TEST(price_ladder, grows_downward_preserving_contents) {
    // A high-priced name does centre its window above zero, so a large drop
    // falls below the base and forces a downward regrow with a content shift.
    PriceLadder l(Side::Buy);
    l.add(50000000, 111);                   // $5,000 — base lands near $4,672
    const std::uint64_t regrows_before = l.regrow_count();

    CHECK(l.add(40000000, 333));            // $4,000 — below the base
    CHECK(l.regrow_count() > regrows_before);

    CHECK_EQ(l.shares_at(50000000), 111);   // survived the shift
    CHECK_EQ(l.shares_at(40000000), 333);
    CHECK_EQ(l.level_count(), 2u);
    LevelView lv{};
    CHECK(l.best(lv));
    CHECK_EQ(lv.price, 50000000u);          // still the best bid
}

NB_TEST(price_ladder, repeated_regrows_stay_consistent) {
    // Walk the price down and then up through several window boundaries, keeping
    // a reference map in step. This is the test that catches an off-by-one in the
    // shift arithmetic, which a single regrow would not expose.
    PriceLadder l(Side::Buy);
    std::map<itch::Price4, std::int64_t> ref;
    itch::Price4 px = 20000000;  // $2000
    for (int i = 0; i < 400; ++i) {
        px = (px > 300000) ? px - 40000 : px + 7000000;   // sweep down, then jump up
        const std::int64_t sh = 100 + i;
        CHECK(l.add(px, static_cast<itch::Shares>(sh)));
        ref[px] += sh;
    }
    CHECK_EQ(l.level_count(), ref.size());
    for (const auto& [p, s] : ref) CHECK_EQ(l.shares_at(p), s);
    LevelView lv{};
    CHECK(l.best(lv));
    CHECK_EQ(lv.price, ref.rbegin()->first);
}

NB_TEST(price_ladder, off_tick_price_is_rejected_and_counted) {
    // Sub-penny prices cannot sit on a penny ladder. Rounding them would silently
    // move liquidity to a price at which it does not exist, so the ladder refuses
    // and counts instead.
    PriceLadder l(Side::Buy, 100);
    CHECK(l.add(1000000, 100));
    CHECK(!l.add(1000050, 100));          // half a cent
    CHECK_EQ(l.off_tick_prices(), 1u);
    CHECK_EQ(l.level_count(), 1u);

    // A sub-dollar name needs a finer ladder, and then it is representable.
    PriceLadder fine(Side::Buy, 1);
    CHECK(fine.add(1000050, 100));
    CHECK_EQ(fine.off_tick_prices(), 0u);
    CHECK_EQ(fine.shares_at(1000050), 100);
}

NB_TEST(price_ladder, negative_quantity_is_clamped_and_counted) {
    PriceLadder l(Side::Buy);
    l.add(1000000, 100);
    l.remove(1000000, 500, true);   // remove more than exists
    CHECK_EQ(l.negative_qty_events(), 1u);
    CHECK_EQ(l.shares_at(1000000), 0);
    CHECK_EQ(l.level_count(), 0u);
}

NB_TEST(price_ladder, removal_from_absent_level_is_counted) {
    PriceLadder l(Side::Buy);
    l.add(1000000, 100);
    CHECK(!l.remove(999900, 50, true));
    CHECK_EQ(l.missing_level_events(), 1u);
}

NB_TEST(price_ladder, shares_within_respects_tick_distance) {
    PriceLadder bids(Side::Buy);
    bids.add(1000000, 10);
    bids.add(999900, 20);
    bids.add(999800, 40);
    bids.add(990000, 80);   // 100 ticks away

    CHECK_EQ(bids.shares_within(0), 10);
    CHECK_EQ(bids.shares_within(1), 30);
    CHECK_EQ(bids.shares_within(2), 70);
    CHECK_EQ(bids.shares_within(5), 70);     // gap: stops before the far level
    CHECK_EQ(bids.shares_within(100), 150);
}

NB_TEST(price_ladder, differential_against_std_map) {
    for (unsigned seed : {1u, 2u, 3u}) {
        PriceLadder l(Side::Buy);
        std::map<itch::Price4, std::int64_t> ref;
        std::mt19937_64 rng(seed);

        for (int i = 0; i < 40000; ++i) {
            // Cluster prices near $100, as a real book does.
            const auto px = static_cast<itch::Price4>(
                1000000 + (static_cast<std::int64_t>(rng() % 401) - 200) * 100);
            if ((rng() & 1) || ref.empty()) {
                const auto sh = static_cast<itch::Shares>(1 + rng() % 1000);
                l.add(px, sh);
                ref[px] += sh;
            } else {
                auto it = ref.find(px);
                if (it == ref.end()) continue;
                const auto take = static_cast<itch::Shares>(1 + rng() % static_cast<std::uint64_t>(it->second));
                l.remove(px, take, true);
                it->second -= take;
                if (it->second <= 0) ref.erase(it);
            }
        }

        CHECK_EQ(l.level_count(), ref.size());
        for (const auto& [p, s] : ref) CHECK_EQ(l.shares_at(p), s);

        LevelView lv{};
        if (ref.empty()) {
            CHECK(!l.best(lv));
        } else {
            CHECK(l.best(lv));
            CHECK_EQ(lv.price, ref.rbegin()->first);   // Buy ladder => highest
            CHECK_EQ(lv.shares, ref.rbegin()->second);
        }
        CHECK_EQ(l.negative_qty_events(), 0u);
    }
}
