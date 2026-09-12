// Hierarchical occupancy bitmap. The two-level CLZ/CTZ logic has four boundary
// cases that are easy to get wrong (word edges, summary-word edges, the
// not-a-multiple-of-64 tail, and the empty bitmap), so this is tested
// differentially against std::set across sizes chosen to straddle all of them.
#include <random>
#include <set>

#include "framework.hpp"
#include "nanobook/bitset_index.hpp"

using namespace nanobook;

NB_TEST(bitset_index, empty_reports_none) {
    BitsetIndex b(128);
    CHECK_EQ(b.lowest(), BitsetIndex::kNone);
    CHECK_EQ(b.highest(), BitsetIndex::kNone);
    CHECK_EQ(b.next_above(0), BitsetIndex::kNone);
    CHECK_EQ(b.next_below(127), BitsetIndex::kNone);
    CHECK_EQ(b.popcount(), 0u);
}

NB_TEST(bitset_index, single_bit_at_word_boundaries) {
    // 63/64 and 4095/4096 straddle the L0 and L1 word edges respectively.
    for (std::size_t i : {std::size_t{0}, std::size_t{63}, std::size_t{64}, std::size_t{65},
                          std::size_t{4095}, std::size_t{4096}, std::size_t{4097}}) {
        BitsetIndex b(8192);
        b.set(i);
        CHECK_EQ(b.lowest(), static_cast<std::int64_t>(i));
        CHECK_EQ(b.highest(), static_cast<std::int64_t>(i));
        CHECK_EQ(b.popcount(), 1u);
        CHECK(b.test(i));
        b.reset(i);
        CHECK_EQ(b.lowest(), BitsetIndex::kNone);
    }
}

NB_TEST(bitset_index, summary_bit_clears_only_when_word_empties) {
    // The classic bug: clearing the L1 summary bit on any reset, which makes the
    // summary under-report and hides live levels from best-price lookups.
    BitsetIndex b(256);
    b.set(10);
    b.set(20);   // same L0 word
    b.reset(10);
    CHECK_EQ(b.lowest(), 20);
    CHECK_EQ(b.highest(), 20);
}

NB_TEST(bitset_index, differential_against_std_set) {
    for (std::size_t n : {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65},
                          std::size_t{127}, std::size_t{128}, std::size_t{4096},
                          std::size_t{5000}, std::size_t{70000}}) {
        BitsetIndex b(n);
        std::set<std::size_t> ref;
        std::mt19937_64 rng(9001 + n);

        for (int iter = 0; iter < 20000; ++iter) {
            const std::size_t i = rng() % n;
            if (rng() & 1) { b.set(i); ref.insert(i); }
            else { b.reset(i); ref.erase(i); }

            if (b.test(i) != (ref.count(i) > 0)) { CHECK(false); break; }

            if (iter % 89 != 0) continue;

            const std::int64_t want_lo = ref.empty() ? BitsetIndex::kNone
                                                     : static_cast<std::int64_t>(*ref.begin());
            const std::int64_t want_hi = ref.empty() ? BitsetIndex::kNone
                                                     : static_cast<std::int64_t>(*ref.rbegin());
            CHECK_EQ(b.lowest(), want_lo);
            CHECK_EQ(b.highest(), want_hi);
            CHECK_EQ(b.popcount(), ref.size());

            const std::size_t q = rng() % n;
            auto ub = ref.upper_bound(q);
            CHECK_EQ(b.next_above(q),
                     ub == ref.end() ? BitsetIndex::kNone : static_cast<std::int64_t>(*ub));
            auto lb = ref.lower_bound(q);
            CHECK_EQ(b.next_below(q),
                     lb == ref.begin() ? BitsetIndex::kNone
                                       : static_cast<std::int64_t>(*std::prev(lb)));
        }
    }
}

NB_TEST(bitset_index, walks_a_sparse_range_in_order) {
    // A sweep through a wide, sparse ladder: this is the pattern that a naive
    // linear scan handles in O(range) and the summary handles in O(set bits).
    BitsetIndex b(65536);
    std::vector<std::size_t> want;
    for (std::size_t i = 7; i < 65536; i += 1013) { b.set(i); want.push_back(i); }

    std::vector<std::size_t> got;
    for (std::int64_t i = b.lowest(); i >= 0; i = b.next_above(static_cast<std::size_t>(i))) {
        got.push_back(static_cast<std::size_t>(i));
    }
    CHECK_EQ(got.size(), want.size());
    CHECK(got == want);

    // And the same walk downward.
    std::vector<std::size_t> rev;
    for (std::int64_t i = b.highest(); i >= 0; i = b.next_below(static_cast<std::size_t>(i))) {
        rev.push_back(static_cast<std::size_t>(i));
    }
    std::reverse(rev.begin(), rev.end());
    CHECK(rev == want);
}
