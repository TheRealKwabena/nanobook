// nanobook — order_map.hpp
//
// Maps a 64-bit ITCH order reference to the resting order's (price, side,
// shares). This is the hottest lookup in the engine: every E/C/X/D/U message —
// roughly 55% of the feed — begins with one.
//
// Why not std::unordered_map?
//   * It is a chained hash map: each bucket holds a pointer to a separately
//     allocated node, so a lookup costs a minimum of two dependent cache misses
//     (bucket array, then node) and a hit on a collision chain costs more.
//   * Every Add Order allocates a node and every Delete frees one. At ~10M adds
//     per symbol-day that is 10M trips through the allocator, on the hot path.
//   * A 16-byte payload lands in a node with ~32 bytes of overhead.
//
// This table is a flat array of 16-byte entries with linear probing. A hit is
// normally one cache miss, and the 4-entries-per-cache-line layout means a short
// probe sequence is nearly free. No allocation in the steady state.
//
// MEASURED, not assumed (tools/bench_structures.cpp, Apple M5, 20M ops against a
// 200k live-order steady state, 55% lookup / 23% erase / 22% insert):
//
//   nanobook::OrderMap    9.92 ns/op
//   std::unordered_map   12.70 ns/op     => 1.28x
//
// 1.28x on the mean is a real but modest win, and worth stating plainly: libc++'s
// unordered_map is not slow. The reasons to keep this table are the ones the mean
// does not show — it performs no allocation at all in the steady state, so there
// is no allocator lock and no malloc slow path in the tail, and the load factor
// and probe distribution are observable (see max_probe_run) rather than opaque.
// For a quoting strategy the bounded tail is worth more than the mean.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nanobook/itch_spec.hpp"

namespace nanobook {

// ---------------------------------------------------------------------------
// Hashing
//
// ITCH order references are assigned close to sequentially. Identity-hashing
// them into a power-of-two table would be fine for *insertion* order, but
// deletions leave the live set clustered in strided runs, which linear probing
// handles badly. splitmix64's finalizer decorrelates the low bits at a cost of
// three multiplies and three shifts — a couple of cycles, fully pipelined, and
// it turns a pathological probe distribution into a flat one.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::uint64_t mix64(std::uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// ---------------------------------------------------------------------------
// Entry layout — exactly 16 bytes, so 4 entries share a 64-byte cache line.
//
// Side is packed into the top bit of the price word. This is safe because ITCH
// prices are uint32 with 4 implied decimals, so the largest representable price
// is $429,496.7295 and every real equity price is far below 2^31. The invariant
// is asserted on insert rather than assumed.
// ---------------------------------------------------------------------------
struct OrderEntry {
    itch::OrderRef ref;              // 0 == empty slot (no real ITCH ref is 0)
    std::uint32_t  price_and_side;   // bit 31 = side (1 == Sell), bits 0..30 = price
    itch::Shares   shares;           // shares still resting

    static constexpr std::uint32_t kSideBit = 0x80000000u;

    [[nodiscard]] itch::Price4 price() const noexcept { return price_and_side & ~kSideBit; }
    [[nodiscard]] itch::Side   side() const noexcept {
        return (price_and_side & kSideBit) ? itch::Side::Sell : itch::Side::Buy;
    }
    void set_price_side(itch::Price4 p, itch::Side s) noexcept {
        assert(p < kSideBit && "ITCH price must fit in 31 bits");
        price_and_side = p | (s == itch::Side::Sell ? kSideBit : 0u);
    }
};

static_assert(sizeof(OrderEntry) == 16, "OrderEntry must stay at 16 bytes");
static_assert(alignof(OrderEntry) == 8);

class OrderMap {
  public:
    static constexpr itch::OrderRef kEmpty = 0;

    // `initial_capacity` is rounded up to a power of two. Size it for the
    // expected peak resting-order count so the steady state never rehashes:
    // a busy large-cap symbol-day peaks around 2^19 live orders.
    explicit OrderMap(std::size_t initial_capacity = 1u << 16) {
        std::size_t cap = 1;
        while (cap < initial_capacity) cap <<= 1;
        slots_.assign(cap, OrderEntry{kEmpty, 0, 0});
        mask_ = cap - 1;
        // Grow at 70% load. Linear probing degrades sharply past ~0.8; 0.7
        // keeps the mean probe count under 2 while wasting little memory.
        grow_at_ = cap - cap / 4 - cap / 20;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] double load_factor() const noexcept {
        return static_cast<double>(size_) / static_cast<double>(slots_.size());
    }

    // Cumulative probes across all find/insert operations, for benchmarking the
    // hash quality. Cheap enough to leave compiled in.
    [[nodiscard]] std::uint64_t total_probes() const noexcept { return probes_; }
    [[nodiscard]] std::uint64_t total_lookups() const noexcept { return lookups_; }

    // Returns nullptr if absent. The pointer is invalidated by any insert that
    // triggers a rehash, so callers must not hold it across an Add Order.
    [[nodiscard]] OrderEntry* find(itch::OrderRef ref) noexcept {
        assert(ref != kEmpty);
        std::size_t i = mix64(ref) & mask_;
        ++lookups_;
        for (;;) {
            ++probes_;
            OrderEntry& e = slots_[i];
            if (e.ref == ref) return &e;
            if (e.ref == kEmpty) return nullptr;  // probe run ends => absent
            i = (i + 1) & mask_;
        }
    }

    void insert(itch::OrderRef ref, itch::Price4 price, itch::Side side, itch::Shares shares) {
        assert(ref != kEmpty && "order reference 0 collides with the empty sentinel");
        if (size_ >= grow_at_) rehash(slots_.size() * 2);
        std::size_t i = mix64(ref) & mask_;
        ++lookups_;
        for (;;) {
            ++probes_;
            OrderEntry& e = slots_[i];
            if (e.ref == kEmpty) {
                e.ref = ref;
                e.set_price_side(price, side);
                e.shares = shares;
                ++size_;
                return;
            }
            // A duplicate reference means the feed replayed an add, or we failed
            // to retire a previous ref. Overwrite and count it — silently
            // dropping it would corrupt the book invisibly.
            if (e.ref == ref) {
                e.set_price_side(price, side);
                e.shares = shares;
                ++duplicate_inserts_;
                return;
            }
            i = (i + 1) & mask_;
        }
    }

    // Backward-shift deletion. The naive alternative — marking a tombstone —
    // means probe sequences never shorten, so a symbol-day of adds and deletes
    // degrades the table into a linear scan. Shifting the tail of the probe run
    // back into the hole keeps the table tombstone-free forever.
    bool erase(itch::OrderRef ref) noexcept {
        OrderEntry* found = find(ref);
        if (found == nullptr) return false;

        std::size_t hole = static_cast<std::size_t>(found - slots_.data());
        slots_[hole].ref = kEmpty;
        --size_;

        std::size_t i = (hole + 1) & mask_;
        while (slots_[i].ref != kEmpty) {
            const std::size_t ideal = mix64(slots_[i].ref) & mask_;
            // Move slots_[i] down into the hole only if doing so does not place
            // it before its ideal slot — i.e. if `ideal` is not inside the
            // cyclic interval (hole, i].
            if (!cyclic_in(ideal, hole, i)) {
                slots_[hole] = slots_[i];
                slots_[i].ref = kEmpty;
                hole = i;
            }
            i = (i + 1) & mask_;
        }
        return true;
    }

    void clear() noexcept {
        std::fill(slots_.begin(), slots_.end(), OrderEntry{kEmpty, 0, 0});
        size_ = 0;
    }

    [[nodiscard]] std::uint64_t duplicate_inserts() const noexcept { return duplicate_inserts_; }

    // Longest probe run in the table. Used by tests to assert the hash is not
    // degenerate, and reported by the replay tool.
    [[nodiscard]] std::size_t max_probe_run() const noexcept {
        std::size_t worst = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].ref == kEmpty) continue;
            const std::size_t ideal = mix64(slots_[i].ref) & mask_;
            worst = std::max(worst, (i - ideal) & mask_);
        }
        return worst + 1;
    }

  private:
    // Is `x` in the cyclic half-open interval (lo, hi]?
    [[nodiscard]] static bool cyclic_in(std::size_t x, std::size_t lo, std::size_t hi) noexcept {
        if (lo < hi) return x > lo && x <= hi;
        return x > lo || x <= hi;  // interval wraps the end of the table
    }

    void rehash(std::size_t new_cap) {
        std::vector<OrderEntry> old;
        old.swap(slots_);
        slots_.assign(new_cap, OrderEntry{kEmpty, 0, 0});
        mask_ = new_cap - 1;
        grow_at_ = new_cap - new_cap / 4 - new_cap / 20;
        size_ = 0;
        ++rehashes_;
        for (const OrderEntry& e : old) {
            if (e.ref == kEmpty) continue;
            insert(e.ref, e.price(), e.side(), e.shares);
        }
    }

    std::vector<OrderEntry> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t grow_at_ = 0;
    std::uint64_t probes_ = 0;
    std::uint64_t lookups_ = 0;
    std::uint64_t duplicate_inserts_ = 0;
    std::uint64_t rehashes_ = 0;
};

}  // namespace nanobook
