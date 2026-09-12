// nanobook — price_ladder.hpp
//
// One side of the book: aggregated resting quantity per price level.
//
// Design choice: a dense array indexed by price, not a std::map<Price, Level>.
//
// A balanced tree is the textbook answer and the wrong one here. Every node is a
// separate allocation, an insert is O(log n) pointer-chasing through cold cache
// lines, and finding the best price means walking to the end of the tree. Real
// equity books are extremely narrow — an S&P name spends the whole session
// inside a few dollars — so the price axis can simply be an array.
//
//   index i  <->  price  base_ + i * tick_
//
// Add and cancel become an array store at a computed index: O(1), one cache
// line, no allocation. Best-price lookup delegates to the L0/L1 occupancy
// bitmap. The cost is memory proportional to the price *range* rather than the
// number of live levels, which for a 65,536-slot penny ladder is 1.5 MB of
// quantity plus 8 KB of bitmap — and the working set actually touched is the few
// hundred cents around the touch, so it stays resident in L1/L2.
//
// Tick size: ITCH prices carry 4 implied decimals, and SEC Reg NMS Rule 612
// forbids displaying quotes in sub-penny increments for stocks priced at or
// above $1.00. So tick_ = 100 (one cent) is correct for essentially every name.
// Sub-dollar stocks may quote in $0.0001, and for those the ladder must be built
// with tick = 1; off-tick prices are counted and reported rather than silently
// rounded, because rounding a price is how a backtest invents free money.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nanobook/bitset_index.hpp"
#include "nanobook/itch_spec.hpp"

namespace nanobook {

struct LevelView {
    itch::Price4  price;
    std::int64_t  shares;
    std::uint32_t order_count;
};

class PriceLadder {
  public:
    // 65,536 penny slots == a $655.36 price range, which covers any single
    // symbol's intraday excursion with room to spare. The ladder still grows
    // automatically if a price lands outside the window.
    static constexpr std::size_t kInitialSlots = 1u << 16;

    explicit PriceLadder(itch::Side side, itch::Price4 tick = 100)
        : side_(side), tick_(tick) {
        assert(tick_ > 0);
    }

    [[nodiscard]] itch::Side side() const noexcept { return side_; }
    [[nodiscard]] itch::Price4 tick() const noexcept { return tick_; }
    [[nodiscard]] std::size_t slot_count() const noexcept { return qty_.size(); }
    [[nodiscard]] std::size_t level_count() const noexcept { return occupied_.popcount(); }
    [[nodiscard]] std::uint64_t off_tick_prices() const noexcept { return off_tick_; }
    [[nodiscard]] std::uint64_t regrow_count() const noexcept { return regrows_; }
    [[nodiscard]] std::uint64_t negative_qty_events() const noexcept { return negative_qty_; }

    // Add `shares` at `price`, creating the level if needed.
    // Returns false if the price is not representable on this ladder's tick.
    bool add(itch::Price4 price, itch::Shares shares) {
        const std::int64_t idx = ensure_index(price);
        if (idx < 0) return false;
        const auto i = static_cast<std::size_t>(idx);
        if (qty_[i] == 0) occupied_.set(i);
        qty_[i] += static_cast<std::int64_t>(shares);
        ++count_[i];
        return true;
    }

    // Remove `shares` at `price`. `order_gone` retires one order from the level's
    // count (true for delete/full-fill, false for a partial cancel or partial
    // fill, where the order stays resting).
    bool remove(itch::Price4 price, itch::Shares shares, bool order_gone) {
        const std::int64_t idx = index_of(price);
        // Off-tick, or outside the window entirely: we never had this price.
        if (idx < 0) { ++missing_level_; return false; }
        const auto i = static_cast<std::size_t>(idx);
        // In range but empty. This is a distinct failure from a level going
        // negative: it means we never saw the add at all — a genuine feed gap —
        // and attributing it to `negative_qty` would blame the arithmetic for a
        // missing message.
        if (qty_[i] == 0) { ++missing_level_; return false; }

        qty_[i] -= static_cast<std::int64_t>(shares);
        if (order_gone && count_[i] > 0) --count_[i];

        if (qty_[i] < 0) {
            // Book integrity is broken; clamp so downstream features stay finite,
            // but count it loudly. A nonzero total here invalidates the run.
            ++negative_qty_;
            qty_[i] = 0;
        }
        if (qty_[i] == 0) {
            occupied_.reset(i);
            count_[i] = 0;
        }
        return true;
    }

    // Best price on this side: highest bid, lowest ask.
    [[nodiscard]] bool best(LevelView& out) const noexcept {
        const std::int64_t i = (side_ == itch::Side::Buy) ? occupied_.highest() : occupied_.lowest();
        if (i < 0) return false;
        out = level_at(static_cast<std::size_t>(i));
        return true;
    }

    // Fill `out` with up to `max_levels` levels from the touch inward-out.
    // Returns the number written.
    std::size_t top_of_book(LevelView* out, std::size_t max_levels) const noexcept {
        std::int64_t i = (side_ == itch::Side::Buy) ? occupied_.highest() : occupied_.lowest();
        std::size_t n = 0;
        while (i >= 0 && n < max_levels) {
            out[n++] = level_at(static_cast<std::size_t>(i));
            i = (side_ == itch::Side::Buy) ? occupied_.next_below(static_cast<std::size_t>(i))
                                           : occupied_.next_above(static_cast<std::size_t>(i));
        }
        return n;
    }

    // Total resting shares within `depth` ticks of the touch. The natural
    // denominator for book-pressure features.
    [[nodiscard]] std::int64_t shares_within(std::size_t depth_ticks) const noexcept {
        std::int64_t i = (side_ == itch::Side::Buy) ? occupied_.highest() : occupied_.lowest();
        if (i < 0) return 0;
        const auto touch = static_cast<std::size_t>(i);
        std::int64_t total = 0;
        while (i >= 0) {
            const auto u = static_cast<std::size_t>(i);
            const std::size_t dist = (side_ == itch::Side::Buy) ? touch - u : u - touch;
            if (dist > depth_ticks) break;
            total += qty_[u];
            i = (side_ == itch::Side::Buy) ? occupied_.next_below(u) : occupied_.next_above(u);
        }
        return total;
    }

    [[nodiscard]] std::int64_t shares_at(itch::Price4 price) const noexcept {
        const std::int64_t i = index_of(price);
        return i < 0 ? 0 : qty_[static_cast<std::size_t>(i)];
    }

    void clear() noexcept {
        std::fill(qty_.begin(), qty_.end(), 0);
        std::fill(count_.begin(), count_.end(), 0u);
        occupied_.clear_all();
    }

    [[nodiscard]] itch::Price4 price_at_index(std::size_t i) const noexcept {
        return base_ + static_cast<itch::Price4>(i * tick_);
    }

  private:
    [[nodiscard]] LevelView level_at(std::size_t i) const noexcept {
        return LevelView{price_at_index(i), qty_[i], count_[i]};
    }

    // Index of `price`, or -1 if unrepresentable or outside the current window.
    // Const: never grows.
    [[nodiscard]] std::int64_t index_of(itch::Price4 price) const noexcept {
        if (qty_.empty() || price < base_) return -1;
        const itch::Price4 off = price - base_;
        if (off % tick_ != 0) return -1;
        const std::size_t i = off / tick_;
        return i < qty_.size() ? static_cast<std::int64_t>(i) : -1;
    }

    // Index of `price`, growing or rebasing the ladder if needed.
    [[nodiscard]] std::int64_t ensure_index(itch::Price4 price) {
        if (qty_.empty()) {
            // Centre the initial window on the first price we see, so the book
            // can move either way without an immediate regrow.
            const itch::Price4 half = static_cast<itch::Price4>(kInitialSlots / 2) * tick_;
            base_ = (price > half) ? align_down(price - half) : 0;
            qty_.assign(kInitialSlots, 0);
            count_.assign(kInitialSlots, 0u);
            occupied_.resize(kInitialSlots);
        }

        if (price < base_) { grow_down(price); }
        else if (const itch::Price4 off = price - base_; off / tick_ >= qty_.size()) { grow_up(price); }

        const std::int64_t i = index_of(price);
        if (i < 0) ++off_tick_;  // sub-penny price on a penny ladder
        return i;
    }

    [[nodiscard]] itch::Price4 align_down(itch::Price4 p) const noexcept { return p - (p % tick_); }

    // Extend the window downward, shifting existing contents up. Rare: a few
    // times per symbol-day at most, so the memmove cost is irrelevant.
    void grow_down(itch::Price4 price) {
        const itch::Price4 new_base = align_down(price) >= margin() ? align_down(price) - margin() : 0;
        const std::size_t shift = (base_ - new_base) / tick_;
        const std::size_t new_slots = qty_.size() + shift;
        rebuild(new_base, new_slots, shift);
    }

    // Extend the window upward; existing contents keep their indices.
    void grow_up(itch::Price4 price) {
        const std::size_t needed = (price - base_) / tick_ + 1;
        const std::size_t new_slots = std::max(needed + margin() / tick_, qty_.size() * 2);
        rebuild(base_, new_slots, 0);
    }

    [[nodiscard]] itch::Price4 margin() const noexcept {
        return static_cast<itch::Price4>(kInitialSlots / 2) * tick_;
    }

    void rebuild(itch::Price4 new_base, std::size_t new_slots, std::size_t shift) {
        std::vector<std::int64_t> nq(new_slots, 0);
        std::vector<std::uint32_t> nc(new_slots, 0u);
        for (std::size_t i = 0; i < qty_.size(); ++i) {
            nq[i + shift] = qty_[i];
            nc[i + shift] = count_[i];
        }
        qty_.swap(nq);
        count_.swap(nc);
        base_ = new_base;
        occupied_.resize(new_slots);
        occupied_.clear_all();
        for (std::size_t i = 0; i < qty_.size(); ++i) {
            if (qty_[i] != 0) occupied_.set(i);
        }
        ++regrows_;
    }

    itch::Side  side_;
    itch::Price4 tick_;
    itch::Price4 base_ = 0;
    std::vector<std::int64_t>  qty_;
    std::vector<std::uint32_t> count_;
    BitsetIndex occupied_;
    std::uint64_t off_tick_ = 0;
    std::uint64_t regrows_ = 0;
    std::uint64_t negative_qty_ = 0;
    std::uint64_t missing_level_ = 0;

  public:
    [[nodiscard]] std::uint64_t missing_level_events() const noexcept { return missing_level_; }
};

}  // namespace nanobook
