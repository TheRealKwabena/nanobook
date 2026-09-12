// Open-addressing order map. The interesting property is the one that a
// tombstone-based table would fail: after a full session of churn, probe runs
// must stay short. That is tested explicitly rather than assumed.
#include <random>
#include <unordered_map>

#include "framework.hpp"
#include "nanobook/order_map.hpp"

using namespace nanobook;
using itch::Side;

NB_TEST(order_map, insert_find_erase) {
    OrderMap m(64);
    m.insert(1, 1000000, Side::Buy, 500);
    m.insert(2, 1000100, Side::Sell, 300);

    OrderEntry* a = m.find(1);
    CHECK(a != nullptr);
    CHECK_EQ(a->price(), 1000000u);
    CHECK(a->side() == Side::Buy);
    CHECK_EQ(a->shares, 500u);

    OrderEntry* b = m.find(2);
    CHECK(b != nullptr);
    CHECK(b->side() == Side::Sell);

    CHECK_EQ(m.size(), 2u);
    CHECK(m.erase(1));
    CHECK(m.find(1) == nullptr);
    CHECK(m.find(2) != nullptr);   // erasing one must not lose its probe neighbour
    CHECK_EQ(m.size(), 1u);
    CHECK(!m.erase(1));            // erasing twice is not an error, just false
}

NB_TEST(order_map, side_packed_in_price_word_round_trips) {
    // Side lives in bit 31 of the price word to keep the entry at 16 bytes. A
    // price at the top of the legal ITCH range must survive that packing.
    OrderMap m(16);
    const itch::Price4 high = 0x7FFFFFFFu - 1;
    m.insert(11, high, Side::Sell, 7);
    m.insert(12, high, Side::Buy, 7);
    CHECK_EQ(m.find(11)->price(), high);
    CHECK(m.find(11)->side() == Side::Sell);
    CHECK_EQ(m.find(12)->price(), high);
    CHECK(m.find(12)->side() == Side::Buy);
}

NB_TEST(order_map, entry_stays_sixteen_bytes) {
    // Four entries per cache line is the whole point of the custom table; if a
    // field is added carelessly this is the test that objects.
    CHECK_EQ(sizeof(OrderEntry), 16u);
}

NB_TEST(order_map, grows_without_losing_entries) {
    OrderMap m(16);  // deliberately tiny, to force several rehashes
    constexpr std::uint64_t kN = 10000;
    for (std::uint64_t i = 1; i <= kN; ++i) {
        m.insert(i, static_cast<itch::Price4>(1000000 + i), (i & 1) ? Side::Buy : Side::Sell,
                 static_cast<itch::Shares>(i % 1000 + 1));
    }
    CHECK_EQ(m.size(), kN);
    for (std::uint64_t i = 1; i <= kN; ++i) {
        OrderEntry* e = m.find(i);
        CHECK(e != nullptr);
        if (e == nullptr) break;
        CHECK_EQ(e->price(), static_cast<itch::Price4>(1000000 + i));
        CHECK(e->side() == ((i & 1) ? Side::Buy : Side::Sell));
    }
}

NB_TEST(order_map, differential_against_unordered_map) {
    OrderMap m(256);
    std::unordered_map<itch::OrderRef, std::pair<itch::Price4, itch::Shares>> ref;
    std::mt19937_64 rng(4242);
    std::vector<itch::OrderRef> live;

    for (int iter = 0; iter < 200000; ++iter) {
        const int op = static_cast<int>(rng() % 100);
        if (op < 55 || live.empty()) {
            const itch::OrderRef r = 1 + rng() % 500000;
            if (ref.count(r)) continue;  // avoid the duplicate-insert path here
            const auto px = static_cast<itch::Price4>(1 + rng() % 2000000);
            const auto sh = static_cast<itch::Shares>(1 + rng() % 10000);
            m.insert(r, px, Side::Buy, sh);
            ref[r] = {px, sh};
            live.push_back(r);
        } else {
            const std::size_t i = rng() % live.size();
            const itch::OrderRef r = live[i];
            live[i] = live.back();
            live.pop_back();
            CHECK_EQ(m.erase(r), ref.erase(r) == 1);
        }
    }

    CHECK_EQ(m.size(), ref.size());
    for (const auto& [r, v] : ref) {
        OrderEntry* e = m.find(r);
        CHECK(e != nullptr);
        if (e == nullptr) break;
        CHECK_EQ(e->price(), v.first);
        CHECK_EQ(e->shares, v.second);
    }
    // Absent keys must report absent, not collide into a neighbour's entry.
    std::uint64_t false_hits = 0;
    for (itch::OrderRef r = 500001; r < 501000; ++r) {
        if (m.find(r) != nullptr) ++false_hits;
    }
    CHECK_EQ(false_hits, 0u);
}

NB_TEST(order_map, churn_does_not_degrade_probe_runs) {
    // The tombstone test. A table that marks deletions instead of back-shifting
    // degrades toward a linear scan under sustained insert/erase churn at a
    // stable live-set size — exactly a trading session's access pattern.
    OrderMap m(1u << 14);
    std::mt19937_64 rng(777);
    itch::OrderRef next = 1;
    std::vector<itch::OrderRef> live;

    // Fill to a steady state of ~8000 live orders, then churn 20x that volume.
    for (int i = 0; i < 8000; ++i) {
        m.insert(next, 1000000, Side::Buy, 100);
        live.push_back(next++);
    }
    const std::size_t probe_when_fresh = m.max_probe_run();

    for (int i = 0; i < 160000; ++i) {
        const std::size_t victim = rng() % live.size();
        m.erase(live[victim]);
        m.insert(next, 1000000, Side::Buy, 100);
        live[victim] = next++;
    }

    CHECK_EQ(m.size(), live.size());
    const std::size_t probe_after_churn = m.max_probe_run();
    // Order references keep increasing, so the table never sees the same key
    // twice; only back-shift deletion keeps this bounded.
    CHECK_MSG(probe_after_churn < 60,
              "longest probe run after churn = " + std::to_string(probe_after_churn) +
                  " (fresh was " + std::to_string(probe_when_fresh) + ")");
    CHECK_MSG(m.total_probes() / m.total_lookups() < 3,
              "mean probes per lookup = " +
                  std::to_string(static_cast<double>(m.total_probes()) /
                                 static_cast<double>(m.total_lookups())));
}

NB_TEST(order_map, duplicate_insert_is_counted_not_silent) {
    OrderMap m(32);
    m.insert(5, 100, Side::Buy, 10);
    CHECK_EQ(m.duplicate_inserts(), 0u);
    m.insert(5, 200, Side::Sell, 20);
    CHECK_EQ(m.duplicate_inserts(), 1u);
    CHECK_EQ(m.size(), 1u);              // not two entries for one reference
    CHECK_EQ(m.find(5)->price(), 200u);  // last write wins
}

NB_TEST(order_map, hash_mixes_sequential_references) {
    // ITCH order references arrive near-sequentially. Identity hashing would put
    // them in consecutive slots, which is fine until deletions make the live set
    // strided. Check the finaliser actually decorrelates.
    OrderMap m(1u << 13);
    for (itch::OrderRef r = 1000000; r < 1000000 + 4000; ++r) {
        m.insert(r, 1000000, Side::Buy, 100);
    }
    CHECK_MSG(m.max_probe_run() < 40,
              "sequential refs gave a probe run of " + std::to_string(m.max_probe_run()));
}
