// nanobook — bench_structures: does the custom data structure actually earn its
// place next to the standard-library one?
//
// Every "we wrote our own hash map" claim should come with a number, because
// about half the time the standard container wins and the custom code is just
// risk. Two head-to-head benchmarks:
//
//   1. ORDER LOOKUP.  OrderMap vs std::unordered_map<uint64_t, Order>, driven by
//      a realistic order-reference workload: near-sequential 64-bit keys, a
//      stable live set, and sustained insert/erase churn — a trading session's
//      access pattern, not a synthetic uniform one.
//
//   2. BOOK UPDATE.  PriceLadder vs std::map<Price, Level>, the textbook order
//      book. Prices cluster near the touch as they do in a real book.
//
// Each is run several times and the best wall time is reported, since the
// interesting quantity is the achievable cost, not the mean of a distribution
// polluted by scheduler noise.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "nanobook/latency.hpp"
#include "nanobook/order_map.hpp"
#include "nanobook/price_ladder.hpp"

using namespace nanobook;
using itch::Side;

namespace {

struct StdOrder {
    itch::Price4 price;
    itch::Shares shares;
    Side side;
};

// Deterministic operation script, generated once and replayed against both
// implementations so each sees byte-identical work.
struct Op {
    enum Kind : std::uint8_t { Insert, Find, Erase } kind;
    itch::OrderRef ref;
    itch::Price4 price;
    itch::Shares shares;
};

std::vector<Op> build_order_script(std::size_t n_ops, std::size_t live_target, std::uint64_t seed) {
    std::vector<Op> ops;
    ops.reserve(n_ops);
    std::mt19937_64 rng(seed);
    std::vector<itch::OrderRef> live;
    // Order references in a real session are assigned near-sequentially.
    itch::OrderRef next = 1;

    while (ops.size() < n_ops) {
        if (live.size() < live_target) {
            ops.push_back({Op::Insert, next, 1000000, 100});
            live.push_back(next++);
            continue;
        }
        // Steady state: every modify message is a lookup; adds and deletes keep
        // the live set roughly constant.
        const int r = static_cast<int>(rng() % 100);
        if (r < 55) {
            ops.push_back({Op::Find, live[rng() % live.size()], 0, 0});
        } else if (r < 78) {
            const std::size_t i = rng() % live.size();
            ops.push_back({Op::Erase, live[i], 0, 0});
            live[i] = live.back();
            live.pop_back();
        } else {
            ops.push_back({Op::Insert, next, 1000000, 100});
            live.push_back(next++);
        }
    }
    return ops;
}

// Returned so the optimiser cannot delete the lookups whose cost we are timing.
std::uint64_t run_order_map(const std::vector<Op>& ops, std::size_t cap) {
    OrderMap m(cap);
    std::uint64_t checksum = 0;
    for (const Op& op : ops) {
        switch (op.kind) {
            case Op::Insert: m.insert(op.ref, op.price, Side::Buy, op.shares); break;
            case Op::Find: {
                const OrderEntry* e = m.find(op.ref);
                if (e != nullptr) checksum += e->shares;
                break;
            }
            case Op::Erase: m.erase(op.ref); break;
        }
    }
    return checksum;
}

std::uint64_t run_unordered_map(const std::vector<Op>& ops, std::size_t cap) {
    std::unordered_map<itch::OrderRef, StdOrder> m;
    m.reserve(cap);
    std::uint64_t checksum = 0;
    for (const Op& op : ops) {
        switch (op.kind) {
            case Op::Insert: m[op.ref] = StdOrder{op.price, op.shares, Side::Buy}; break;
            case Op::Find: {
                auto it = m.find(op.ref);
                if (it != m.end()) checksum += it->second.shares;
                break;
            }
            case Op::Erase: m.erase(op.ref); break;
        }
    }
    return checksum;
}

// ---------------------------------------------------------------------------

struct LadderOp {
    bool add;
    itch::Price4 price;
    itch::Shares shares;
};

std::vector<LadderOp> build_ladder_script(std::size_t n_ops, std::uint64_t seed) {
    std::vector<LadderOp> ops;
    ops.reserve(n_ops);
    std::mt19937_64 rng(seed);
    itch::Price4 mid = 1000000;
    std::vector<itch::Price4> occupied;

    while (ops.size() < n_ops) {
        // Random-walk the mid, and place near it with a geometric depth profile,
        // which is how real resting liquidity is distributed.
        if ((rng() & 0x3F) == 0) mid += ((rng() & 1) ? 100 : -100);
        std::uint32_t d = 0;
        while (d < 20 && (rng() % 100) < 55) ++d;
        const itch::Price4 px = mid + (((rng() & 1) ? 1 : -1) * static_cast<std::int32_t>(d) * 100);

        if (occupied.size() < 4000 || (rng() % 100) < 50) {
            ops.push_back({true, px, static_cast<itch::Shares>(100 + rng() % 900)});
            occupied.push_back(px);
        } else {
            const std::size_t i = rng() % occupied.size();
            ops.push_back({false, occupied[i], 100});
            occupied[i] = occupied.back();
            occupied.pop_back();
        }
    }
    return ops;
}

std::uint64_t run_price_ladder(const std::vector<LadderOp>& ops) {
    PriceLadder l(Side::Buy, 100);
    std::uint64_t checksum = 0;
    LevelView lv{};
    for (const LadderOp& op : ops) {
        if (op.add) l.add(op.price, op.shares);
        else l.remove(op.price, op.shares, true);
        // Best-price lookup after every update: exactly what a book consumer does.
        if (l.best(lv)) checksum += static_cast<std::uint64_t>(lv.price);
    }
    return checksum;
}

std::uint64_t run_std_map(const std::vector<LadderOp>& ops) {
    std::map<itch::Price4, std::int64_t> l;
    std::uint64_t checksum = 0;
    for (const LadderOp& op : ops) {
        if (op.add) {
            l[op.price] += op.shares;
        } else {
            auto it = l.find(op.price);
            if (it != l.end()) {
                it->second -= op.shares;
                if (it->second <= 0) l.erase(it);
            }
        }
        if (!l.empty()) checksum += static_cast<std::uint64_t>(std::prev(l.end())->first);
    }
    return checksum;
}

template <class F>
double best_ns_per_op(F&& f, std::size_t n_ops, int reps) {
    double best = 1e30;
    std::uint64_t sink = 0;
    for (int i = 0; i < reps; ++i) {
        const std::uint64_t t0 = now_ns();
        sink += f();
        const std::uint64_t t1 = now_ns();
        best = std::min(best, static_cast<double>(t1 - t0) / static_cast<double>(n_ops));
    }
    // Consume the checksum so nothing is optimised away.
    if (sink == 0xFFFFFFFFFFFFFFFFull) std::fprintf(stderr, "(unreachable)\n");
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n_ops = 20000000;
    std::size_t live = 200000;
    int reps = 3;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) n_ops = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--live") && i + 1 < argc) live = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
    }

    std::printf("nanobook structure benchmarks  (best of %d, %zu ops each)\n\n", reps, n_ops);

    // ---- order lookup ----
    std::printf("order reference map — %zu live orders, near-sequential 64-bit keys,\n", live);
    std::printf("55%% lookups / 23%% erases / 22%% inserts (a session's steady state)\n");
    const std::vector<Op> ops = build_order_script(n_ops, live, 20240912);
    std::size_t cap = 1;
    while (cap < live * 2) cap <<= 1;

    const double nb = best_ns_per_op([&] { return run_order_map(ops, cap); }, n_ops, reps);
    const double um = best_ns_per_op([&] { return run_unordered_map(ops, cap); }, n_ops, reps);
    std::printf("  nanobook::OrderMap        %6.2f ns/op\n", nb);
    std::printf("  std::unordered_map        %6.2f ns/op\n", um);
    std::printf("  speedup                   %6.2fx\n\n", um / nb);

    // ---- book update ----
    const std::size_t l_ops = n_ops / 4;
    std::printf("price ladder — updates clustered near the touch, best-price query after each\n");
    const std::vector<LadderOp> lops = build_ladder_script(l_ops, 777);
    const double pl = best_ns_per_op([&] { return run_price_ladder(lops); }, l_ops, reps);
    const double sm = best_ns_per_op([&] { return run_std_map(lops); }, l_ops, reps);
    std::printf("  nanobook::PriceLadder     %6.2f ns/op\n", pl);
    std::printf("  std::map                  %6.2f ns/op\n", sm);
    std::printf("  speedup                   %6.2fx\n", sm / pl);

    return 0;
}
