// nanobook — bitset_index.hpp
//
// An occupancy bitmap over price-ladder slots, with a second-level summary so
// that "which is the best bid?" is O(1) rather than a linear scan.
//
// The problem it solves: a price ladder is a dense array indexed by price, and
// most of it is empty. When the best bid is consumed, a naive book walks
// downward one slot at a time looking for the next non-empty level. That is
// usually 1-2 steps, but when a large order sweeps several levels — exactly the
// moment a latency-sensitive strategy cares about — it degenerates into a scan
// over hundreds of empty cents. Tail latency, not mean latency, is what gets you
// picked off, so the worst case is the one that matters.
//
// Structure, for a 65,536-slot ladder:
//
//   L0: 1024 words, one bit per slot      (8 KB — fits comfortably in L1d)
//   L1:   16 words, one bit per L0 word   (128 B — two cache lines)
//
// find-best becomes: scan <=16 L1 words, one CTZ/CLZ to pick the L0 word, one
// more to pick the bit. Three dependent operations, no data-dependent branching
// on ladder contents.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nanobook {

class BitsetIndex {
  public:
    // Sentinel for "no bit set". Signed so callers can compare against >= 0.
    static constexpr std::int64_t kNone = -1;

    explicit BitsetIndex(std::size_t n_bits = 0) { resize(n_bits); }

    void resize(std::size_t n_bits) {
        n_bits_ = n_bits;
        l0_.assign((n_bits + 63) / 64, 0);
        l1_.assign((l0_.size() + 63) / 64, 0);
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_bits_; }

    void clear_all() noexcept {
        std::fill(l0_.begin(), l0_.end(), 0ULL);
        std::fill(l1_.begin(), l1_.end(), 0ULL);
    }

    void set(std::size_t i) noexcept {
        assert(i < n_bits_);
        const std::size_t w = i >> 6;
        l0_[w] |= bit(i & 63);
        l1_[w >> 6] |= bit(w & 63);
    }

    void reset(std::size_t i) noexcept {
        assert(i < n_bits_);
        const std::size_t w = i >> 6;
        l0_[w] &= ~bit(i & 63);
        // Only clear the summary bit once the whole L0 word is empty, otherwise
        // the summary would under-report and best-price lookups would skip live
        // levels.
        if (l0_[w] == 0) l1_[w >> 6] &= ~bit(w & 63);
    }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        assert(i < n_bits_);
        return (l0_[i >> 6] & bit(i & 63)) != 0;
    }

    // Lowest set bit overall — the best ask on an ascending ladder.
    [[nodiscard]] std::int64_t lowest() const noexcept {
        for (std::size_t s = 0; s < l1_.size(); ++s) {
            if (l1_[s] == 0) continue;
            const std::size_t w = (s << 6) + static_cast<std::size_t>(__builtin_ctzll(l1_[s]));
            return static_cast<std::int64_t>((w << 6) + static_cast<std::size_t>(__builtin_ctzll(l0_[w])));
        }
        return kNone;
    }

    // Highest set bit overall — the best bid on an ascending ladder.
    [[nodiscard]] std::int64_t highest() const noexcept {
        for (std::size_t s = l1_.size(); s-- > 0;) {
            if (l1_[s] == 0) continue;
            const std::size_t w = (s << 6) + static_cast<std::size_t>(63 - __builtin_clzll(l1_[s]));
            return static_cast<std::int64_t>((w << 6) +
                                             static_cast<std::size_t>(63 - __builtin_clzll(l0_[w])));
        }
        return kNone;
    }

    // Next set bit strictly above `i`. Walks depth outward from the touch.
    [[nodiscard]] std::int64_t next_above(std::size_t i) const noexcept {
        if (i + 1 >= n_bits_) return kNone;
        std::size_t w = i >> 6;
        // Mask off the current bit and everything below it in this word.
        std::uint64_t rest = l0_[w] & ~mask_up_to(i & 63);
        if (rest != 0) {
            return static_cast<std::int64_t>((w << 6) + static_cast<std::size_t>(__builtin_ctzll(rest)));
        }
        // Find the next non-empty L0 word using the summary.
        std::size_t s = w >> 6;
        std::uint64_t srest = l1_[s] & ~mask_up_to(w & 63);
        while (srest == 0) {
            if (++s >= l1_.size()) return kNone;
            srest = l1_[s];
        }
        w = (s << 6) + static_cast<std::size_t>(__builtin_ctzll(srest));
        return static_cast<std::int64_t>((w << 6) + static_cast<std::size_t>(__builtin_ctzll(l0_[w])));
    }

    // Next set bit strictly below `i`.
    [[nodiscard]] std::int64_t next_below(std::size_t i) const noexcept {
        if (i == 0) return kNone;
        std::size_t w = i >> 6;
        std::uint64_t rest = l0_[w] & mask_below(i & 63);
        if (rest != 0) {
            return static_cast<std::int64_t>((w << 6) +
                                            static_cast<std::size_t>(63 - __builtin_clzll(rest)));
        }
        std::size_t s = w >> 6;
        std::uint64_t srest = l1_[s] & mask_below(w & 63);
        while (srest == 0) {
            if (s == 0) return kNone;
            --s;
            srest = l1_[s];
        }
        w = (s << 6) + static_cast<std::size_t>(63 - __builtin_clzll(srest));
        return static_cast<std::int64_t>((w << 6) +
                                        static_cast<std::size_t>(63 - __builtin_clzll(l0_[w])));
    }

    [[nodiscard]] std::size_t popcount() const noexcept {
        std::size_t n = 0;
        for (std::uint64_t w : l0_) n += static_cast<std::size_t>(__builtin_popcountll(w));
        return n;
    }

  private:
    [[nodiscard]] static constexpr std::uint64_t bit(std::size_t b) noexcept { return 1ULL << b; }

    // Bits 0..b inclusive.
    [[nodiscard]] static constexpr std::uint64_t mask_up_to(std::size_t b) noexcept {
        return b >= 63 ? ~0ULL : ((1ULL << (b + 1)) - 1);
    }

    // Bits 0..b-1 (strictly below b).
    [[nodiscard]] static constexpr std::uint64_t mask_below(std::size_t b) noexcept {
        return b == 0 ? 0ULL : ((1ULL << b) - 1);
    }

    std::vector<std::uint64_t> l0_;
    std::vector<std::uint64_t> l1_;
    std::size_t n_bits_ = 0;
};

}  // namespace nanobook
