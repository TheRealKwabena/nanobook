// nanobook — gen_itch: synthetic TotalView-ITCH 5.0 feed generator.
//
// Why this exists, rather than just telling people to download a session file:
//
//   * A real NASDAQ sample is 5-12 GB. A repository whose tests cannot run until
//     you have downloaded 12 GB is a repository whose tests do not run.
//   * Tests need determinism. The same seed must produce the same feed, byte for
//     byte, so a regression is a real regression and not yesterday's market.
//   * Most importantly: this generator maintains its own **independent shadow
//     book** using std::map — the slow, obviously-correct implementation — and
//     writes out the resulting top-of-book after every single message. That gives
//     a ground-truth file to differentially test the fast reconstruction against.
//     Two implementations that disagree localise a bug to one of them; a fast
//     implementation alone can only be checked against your own assumptions.
//
// The feed contains several symbols whose messages are interleaved, because
// single-symbol test data silently passes a book that ignores the locate filter
// entirely. Ground truth is emitted for the first symbol only.
//
// Message mix approximates a real large-cap session: adds and deletes dominate,
// executions are a few percent, and hidden-order trades ('P') are present
// specifically so the "must not touch the book" path is covered.

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "nanobook/byte_order.hpp"
#include "nanobook/itch_spec.hpp"
#include "nanobook/itch_writer.hpp"

using namespace nanobook;
using nanobook::itch::Price4;
using nanobook::itch::OrderRef;
using nanobook::itch::Shares;
using nanobook::itch::Side;

namespace {

constexpr Price4 kTick = 100;  // one cent, in ITCH's 1/10000 units

// ---------------------------------------------------------------------------
// Buffered writer. The generator is not latency-critical, but writing a few
// hundred MB one 36-byte fwrite at a time is needlessly slow.
// ---------------------------------------------------------------------------
class FeedWriter {
  public:
    explicit FeedWriter(const std::string& path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (f_ == nullptr) {
            std::fprintf(stderr, "gen_itch: cannot open %s: %s\n", path.c_str(), std::strerror(errno));
            std::exit(1);
        }
        buf_.reserve(1 << 20);
    }
    ~FeedWriter() { flush(); if (f_) std::fclose(f_); }

    // BinaryFILE framing: 2-byte big-endian length, then the message body.
    void write_msg(const std::byte* m, std::size_t len) {
        std::byte hdr[2];
        store_be16(hdr, static_cast<std::uint16_t>(len));
        buf_.insert(buf_.end(), hdr, hdr + 2);
        buf_.insert(buf_.end(), m, m + len);
        if (buf_.size() >= (1u << 20)) flush();
        bytes_ += len + 2;
        ++messages_;
    }

    void flush() {
        if (!buf_.empty() && f_ != nullptr) {
            std::fwrite(buf_.data(), 1, buf_.size(), f_);
            buf_.clear();
        }
    }

    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }

  private:
    std::FILE* f_ = nullptr;
    std::vector<std::byte> buf_;
    std::uint64_t bytes_ = 0;
    std::uint64_t messages_ = 0;
};

// ---------------------------------------------------------------------------
// Message encoding lives in nanobook/itch_writer.hpp, shared with the test
// suite. Local aliases keep the generation loop below readable.
// ---------------------------------------------------------------------------
using itch::MsgBuf;

inline std::size_t make_system_event(MsgBuf& m, std::uint64_t ts, char code) {
    return itch::write_system_event(m, ts, code);
}
inline std::size_t make_stock_directory(MsgBuf& m, std::uint16_t loc, std::uint64_t ts,
                                        const std::string& sym) {
    return itch::write_stock_directory(m, loc, ts, sym);
}
inline std::size_t make_add(MsgBuf& m, std::uint16_t loc, std::uint64_t ts, OrderRef ref, Side side,
                            Shares sh, const std::string& sym, Price4 px) {
    return itch::write_add_order(m, loc, ts, ref, side, sh, sym, px);
}
inline std::size_t make_add_mpid(MsgBuf& m, std::uint16_t loc, std::uint64_t ts, OrderRef ref,
                                 Side side, Shares sh, const std::string& sym, Price4 px) {
    return itch::write_add_order_mpid(m, loc, ts, ref, side, sh, sym, px);
}
inline std::size_t make_executed(MsgBuf& m, std::uint16_t loc, std::uint64_t ts, OrderRef ref,
                                 Shares sh, std::uint64_t match) {
    return itch::write_order_executed(m, loc, ts, ref, sh, match);
}
inline std::size_t make_executed_with_price(MsgBuf& m, std::uint16_t loc, std::uint64_t ts,
                                            OrderRef ref, Shares sh, std::uint64_t match,
                                            bool printable, Price4 px) {
    return itch::write_order_executed_with_price(m, loc, ts, ref, sh, match, printable, px);
}
inline std::size_t make_cancel(MsgBuf& m, std::uint16_t loc, std::uint64_t ts, OrderRef ref,
                               Shares sh) {
    return itch::write_order_cancel(m, loc, ts, ref, sh);
}
inline std::size_t make_delete(MsgBuf& m, std::uint16_t loc, std::uint64_t ts, OrderRef ref) {
    return itch::write_order_delete(m, loc, ts, ref);
}
inline std::size_t make_replace(MsgBuf& m, std::uint16_t loc, std::uint64_t ts, OrderRef old_ref,
                                OrderRef new_ref, Shares sh, Price4 px) {
    return itch::write_order_replace(m, loc, ts, old_ref, new_ref, sh, px);
}
inline std::size_t make_trade(MsgBuf& m, std::uint16_t loc, std::uint64_t ts, Side side, Shares sh,
                              const std::string& sym, Price4 px, std::uint64_t match) {
    return itch::write_trade(m, loc, ts, side, sh, sym, px, match);
}

// ---------------------------------------------------------------------------
// Shadow book — deliberately the naive implementation. std::map keyed by price,
// std::unordered_map for orders. Slow and transparently correct, which is
// exactly what a test oracle should be.
// ---------------------------------------------------------------------------
struct ShadowOrder {
    Side   side;
    Price4 price;
    Shares shares;
};

class ShadowBook {
  public:
    void add(OrderRef ref, Side side, Shares shares, Price4 px) {
        live_[ref] = ShadowOrder{side, px, shares};
        side_map(side)[px] += shares;
        refs_.push_back(ref);
        pos_[ref] = refs_.size() - 1;
    }

    // Reduce an order by `n` shares; removes it if it reaches zero.
    void reduce(OrderRef ref, Shares n) {
        auto it = live_.find(ref);
        if (it == live_.end()) return;
        ShadowOrder& o = it->second;
        const Shares took = std::min(n, o.shares);
        o.shares -= took;
        auto& lv = side_map(o.side);
        auto lit = lv.find(o.price);
        if (lit != lv.end()) {
            lit->second -= took;
            if (lit->second <= 0) lv.erase(lit);
        }
        if (o.shares == 0) drop(ref);
    }

    void remove(OrderRef ref) {
        auto it = live_.find(ref);
        if (it == live_.end()) return;
        reduce(ref, it->second.shares);
    }

    [[nodiscard]] const ShadowOrder* get(OrderRef ref) const {
        auto it = live_.find(ref);
        return it == live_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] bool best_bid(Price4& px, std::int64_t& sz) const {
        if (bids_.empty()) return false;
        auto it = std::prev(bids_.end());  // highest price
        px = it->first; sz = it->second;
        return true;
    }

    [[nodiscard]] bool best_ask(Price4& px, std::int64_t& sz) const {
        if (asks_.empty()) return false;
        auto it = asks_.begin();  // lowest price
        px = it->first; sz = it->second;
        return true;
    }

    [[nodiscard]] std::size_t live_count() const noexcept { return live_.size(); }
    [[nodiscard]] const std::vector<OrderRef>& refs() const noexcept { return refs_; }

    // Uniformly sample a live order reference, or 0 if the book is empty.
    template <class Rng>
    [[nodiscard]] OrderRef sample(Rng& rng) const {
        if (refs_.empty()) return 0;
        return refs_[rng() % refs_.size()];
    }

  private:
    std::map<Price4, std::int64_t>& side_map(Side s) { return s == Side::Buy ? bids_ : asks_; }

    // O(1) removal from the sampling vector: swap the victim with the last
    // element and pop. Keeps uniform sampling cheap as orders churn.
    void drop(OrderRef ref) {
        auto pit = pos_.find(ref);
        if (pit != pos_.end()) {
            const std::size_t i = pit->second;
            const OrderRef moved = refs_.back();
            refs_[i] = moved;
            pos_[moved] = i;
            refs_.pop_back();
            pos_.erase(ref);
        }
        live_.erase(ref);
    }

    std::map<Price4, std::int64_t> bids_, asks_;
    std::unordered_map<OrderRef, ShadowOrder> live_;
    std::vector<OrderRef> refs_;
    std::unordered_map<OrderRef, std::size_t> pos_;
};

// ---------------------------------------------------------------------------

struct SymbolState {
    std::string  name;
    std::uint16_t locate;
    Price4       ref_mid;   // random-walk anchor the quotes are placed around
    ShadowBook   book;
};

void usage() {
    std::fprintf(stderr,
        "gen_itch — synthetic TotalView-ITCH 5.0 feed generator\n\n"
        "usage: gen_itch --out FILE [options]\n"
        "  --out FILE        output feed (BinaryFILE framing)           [required]\n"
        "  --truth FILE      ground-truth top-of-book CSV for symbol #1 [optional]\n"
        "  --messages N      order-stream messages to emit              [1000000]\n"
        "  --symbols N       number of interleaved symbols (1-8)        [3]\n"
        "  --symbol NAME     ticker for symbol #1 (the truth target)    [NBSYN]\n"
        "  --price DOLLARS   starting price for symbol #1               [100.00]\n"
        "  --seed N          RNG seed (determinism)                     [42]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path, truth_path, sym1 = "NBSYN";
    std::uint64_t n_messages = 1000000;
    int n_symbols = 3;
    double start_price = 100.00;
    std::uint64_t seed = 42;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); std::exit(1); }
            return argv[++i];
        };
        if (!std::strcmp(a, "--out")) out_path = next();
        else if (!std::strcmp(a, "--truth")) truth_path = next();
        else if (!std::strcmp(a, "--messages")) n_messages = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(a, "--symbols")) n_symbols = std::atoi(next());
        else if (!std::strcmp(a, "--symbol")) sym1 = next();
        else if (!std::strcmp(a, "--price")) start_price = std::atof(next());
        else if (!std::strcmp(a, "--seed")) seed = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) { usage(); return 0; }
        else { std::fprintf(stderr, "gen_itch: unknown argument %s\n", a); usage(); return 1; }
    }
    if (out_path.empty()) { usage(); return 1; }
    if (n_symbols < 1 || n_symbols > 8) { std::fprintf(stderr, "gen_itch: --symbols must be 1-8\n"); return 1; }

    FeedWriter w(out_path);
    std::FILE* truth = nullptr;
    if (!truth_path.empty()) {
        truth = std::fopen(truth_path.c_str(), "wb");
        if (truth == nullptr) {
            std::fprintf(stderr, "gen_itch: cannot open %s: %s\n", truth_path.c_str(), std::strerror(errno));
            return 1;
        }
        // Ground truth after every message: the reconstruction must match this
        // row for row, not merely at the end of the day.
        std::fprintf(truth, "seq,ts,type,bid,bid_shares,ask,ask_shares\n");
    }

    std::mt19937_64 rng(seed);

    // Symbol #1 is the truth target; the rest exist to interleave traffic.
    static const char* kDecoys[] = {"ZXTRA", "QQFOO", "MMBAR", "PPBAZ", "TTQUX", "VVCOR", "WWGRP"};
    std::vector<SymbolState> syms;
    syms.reserve(static_cast<std::size_t>(n_symbols));
    for (int i = 0; i < n_symbols; ++i) {
        SymbolState s;
        s.name = (i == 0) ? sym1 : kDecoys[(i - 1) % 7];
        s.locate = static_cast<std::uint16_t>(1000 + i);  // arbitrary, as in a real session
        const double px = (i == 0) ? start_price : 20.0 + 40.0 * (i + 1);
        s.ref_mid = static_cast<Price4>(px * itch::kPriceScale / kTick) * kTick;
        syms.push_back(std::move(s));
    }

    MsgBuf m;
    std::uint64_t ts = 4ULL * 3600 * 1000000000ULL;  // 04:00:00 ET, pre-market
    std::uint64_t next_ref = 1;                       // 0 is the OrderMap empty sentinel
    std::uint64_t match_no = 1;
    std::uint64_t seq = 0;

    auto emit = [&](std::size_t len) { w.write_msg(m.b, len); };

    // --- session preamble -------------------------------------------------
    emit(make_system_event(m, ts, 'O'));  // start of messages
    for (const auto& s : syms) {
        ts += 1000;
        emit(make_stock_directory(m, s.locate, ts, s.name));
    }
    ts += 1000;
    emit(make_system_event(m, ts, 'S'));  // start of system hours
    ts = 9ULL * 3600 * 1000000000ULL + 30ULL * 60 * 1000000000ULL;  // 09:30:00
    emit(make_system_event(m, ts, 'Q'));  // start of market hours

    // Record a truth row for the target symbol. `type` is the message that was
    // just applied.
    auto write_truth = [&](char type) {
        if (truth == nullptr) return;
        Price4 bp = 0, ap = 0;
        std::int64_t bs = 0, as = 0;
        const bool hb = syms[0].book.best_bid(bp, bs);
        const bool ha = syms[0].book.best_ask(ap, as);
        std::fprintf(truth, "%" PRIu64 ",%" PRIu64 ",%c,%u,%" PRId64 ",%u,%" PRId64 "\n",
                     seq, ts, type, hb ? bp : 0, hb ? bs : 0, ha ? ap : 0, ha ? as : 0);
    };

    // --- order stream -----------------------------------------------------
    // Weights approximate a large-cap session. Adds and deletes dominate because
    // most posted liquidity is cancelled rather than filled.
    struct { char type; int weight; } kMix[] = {
        {'A', 38}, {'F', 3}, {'D', 33}, {'U', 12}, {'X', 6}, {'E', 5}, {'C', 1}, {'P', 2},
    };
    int total_weight = 0;
    for (const auto& e : kMix) total_weight += e.weight;

    std::uint64_t emitted_by_type[128] = {};

    for (std::uint64_t i = 0; i < n_messages; ++i) {
        // Inter-arrival time: heavy-tailed, so the timestamp series looks like a
        // real feed (bursts around events, quiet stretches between).
        ts += 1 + static_cast<std::uint64_t>(-std::log(
                  std::max(1e-12, std::generate_canonical<double, 32>(rng))) * 3000.0);

        SymbolState& s = syms[rng() % syms.size()];
        const bool is_target = (&s == &syms[0]);

        // Random-walk the placement anchor.
        if ((rng() & 0x3F) == 0) {
            const bool up = (rng() & 1) != 0;
            if (up) s.ref_mid += kTick;
            else if (s.ref_mid > 100 * kTick) s.ref_mid -= kTick;
        }

        // Choose an action.
        int r = static_cast<int>(rng() % static_cast<std::uint64_t>(total_weight));
        char action = 'A';
        for (const auto& e : kMix) { if (r < e.weight) { action = e.type; break; } r -= e.weight; }

        // Any order-dependent action degrades to an add when the book is empty.
        if (action != 'A' && action != 'F' && action != 'P' && s.book.live_count() == 0) action = 'A';

        Price4 bb = 0, ba = 0;
        std::int64_t bbs = 0, bas = 0;
        const bool have_bid = s.book.best_bid(bb, bbs);
        const bool have_ask = s.book.best_ask(ba, bas);

        switch (action) {
            case 'A':
            case 'F': {
                const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
                // Distance from the touch, geometric: most new orders join at or
                // near the best price, a long tail sits deeper.
                std::uint32_t d = 0;
                while (d < 30 && (rng() % 100) < 55) ++d;

                Price4 px;
                if (side == Side::Buy) {
                    const Price4 anchor = have_ask ? ba - kTick : s.ref_mid;
                    px = anchor - d * kTick;
                    // Never post a bid at or above the best offer: a real venue
                    // would have matched it, so a crossed book would be fiction.
                    if (have_ask && px >= ba) px = ba - kTick;
                } else {
                    const Price4 anchor = have_bid ? bb + kTick : s.ref_mid;
                    px = anchor + d * kTick;
                    if (have_bid && px <= bb) px = bb + kTick;
                }
                if (px < kTick) px = kTick;

                // Mostly round lots, with a realistic minority of odd lots.
                const Shares shares = ((rng() % 100) < 85)
                                          ? static_cast<Shares>(100 * (1 + rng() % 10))
                                          : static_cast<Shares>(1 + rng() % 99);

                const OrderRef ref = next_ref++;
                s.book.add(ref, side, shares, px);
                const std::size_t len = (action == 'A')
                    ? make_add(m, s.locate, ts, ref, side, shares, s.name, px)
                    : make_add_mpid(m, s.locate, ts, ref, side, shares, s.name, px);
                emit(len);
                break;
            }
            case 'D': {
                const OrderRef ref = s.book.sample(rng);
                s.book.remove(ref);
                emit(make_delete(m, s.locate, ts, ref));
                break;
            }
            case 'X': {
                const OrderRef ref = s.book.sample(rng);
                const ShadowOrder* o = s.book.get(ref);
                if (o == nullptr) { --i; continue; }
                // Partial cancel: leave at least one share so the order stays
                // resting, which exercises the "order survives" path. Every so
                // often cancel the lot, which exercises removal via 'X'.
                Shares n = o->shares > 1 ? static_cast<Shares>(1 + rng() % (o->shares - 1))
                                         : o->shares;
                if ((rng() % 10) == 0) n = o->shares;
                s.book.reduce(ref, n);
                emit(make_cancel(m, s.locate, ts, ref, n));
                break;
            }
            case 'E':
            case 'C': {
                // Executions land at the touch, so sample a handful of resting
                // orders and trade the one closest to the best price.
                OrderRef best_ref = 0;
                std::int64_t best_dist = INT64_MAX;
                for (int k = 0; k < 8; ++k) {
                    const OrderRef cand = s.book.sample(rng);
                    const ShadowOrder* o = s.book.get(cand);
                    if (o == nullptr) continue;
                    const std::int64_t dist =
                        (o->side == Side::Buy)
                            ? (have_bid ? static_cast<std::int64_t>(bb) - static_cast<std::int64_t>(o->price) : 0)
                            : (have_ask ? static_cast<std::int64_t>(o->price) - static_cast<std::int64_t>(ba) : 0);
                    if (dist < best_dist) { best_dist = dist; best_ref = cand; }
                }
                const ShadowOrder* o = s.book.get(best_ref);
                if (o == nullptr) { --i; continue; }

                const Shares n = o->shares > 1 ? static_cast<Shares>(1 + rng() % o->shares)
                                               : o->shares;
                const Price4 order_px = o->price;
                s.book.reduce(best_ref, n);
                if (action == 'E') {
                    emit(make_executed(m, s.locate, ts, best_ref, n, match_no++));
                } else {
                    // 'C' prints away from the order's display price, and is
                    // sometimes non-printable (excluded from volume).
                    const bool printable = (rng() % 5) != 0;
                    emit(make_executed_with_price(m, s.locate, ts, best_ref, n, match_no++,
                                                  printable, order_px));
                }
                break;
            }
            case 'U': {
                const OrderRef old_ref = s.book.sample(rng);
                const ShadowOrder* o = s.book.get(old_ref);
                if (o == nullptr) { --i; continue; }
                const Side side = o->side;
                std::uint32_t d = 0;
                while (d < 10 && (rng() % 100) < 50) ++d;
                Price4 px = (side == Side::Buy)
                    ? ((have_ask ? ba - kTick : s.ref_mid) - d * kTick)
                    : ((have_bid ? bb + kTick : s.ref_mid) + d * kTick);
                if (side == Side::Buy && have_ask && px >= ba) px = ba - kTick;
                if (side == Side::Sell && have_bid && px <= bb) px = bb + kTick;
                if (px < kTick) px = kTick;

                const Shares shares = static_cast<Shares>(100 * (1 + rng() % 10));
                const OrderRef new_ref = next_ref++;

                s.book.remove(old_ref);
                s.book.add(new_ref, side, shares, px);
                emit(make_replace(m, s.locate, ts, old_ref, new_ref, shares, px));
                break;
            }
            case 'P': {
                // Execution against a hidden order: the book must not change.
                const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
                Price4 px = s.ref_mid;
                if (have_bid && have_ask) px = (bb + ba) / 2 / kTick * kTick;
                const Shares shares = static_cast<Shares>(100 * (1 + rng() % 5));
                emit(make_trade(m, s.locate, ts, side, shares, s.name, px, match_no++));
                break;
            }
            default: break;
        }

        ++emitted_by_type[static_cast<unsigned char>(action)];
        if (is_target) { write_truth(action); ++seq; }
    }

    // --- session close ----------------------------------------------------
    ts = 16ULL * 3600 * 1000000000ULL;  // 16:00:00
    emit(make_system_event(m, ts, 'M'));  // end of market hours
    ts += 1000;
    emit(make_system_event(m, ts, 'E'));  // end of system hours
    ts += 1000;
    emit(make_system_event(m, ts, 'C'));  // end of messages
    w.flush();
    if (truth != nullptr) std::fclose(truth);

    std::printf("gen_itch: wrote %s\n", out_path.c_str());
    std::printf("  messages   %" PRIu64 "  (%.2f MB)\n", w.messages(),
                static_cast<double>(w.bytes()) / (1024.0 * 1024.0));
    std::printf("  symbols    %d  (truth target: %s, locate %u)\n", n_symbols,
                syms[0].name.c_str(), syms[0].locate);
    std::printf("  mix       ");
    for (const auto& e : kMix) {
        std::printf(" %c=%.1f%%", e.type,
                    100.0 * static_cast<double>(emitted_by_type[static_cast<unsigned char>(e.type)]) /
                        static_cast<double>(n_messages));
    }
    std::printf("\n");
    std::printf("  resting orders left in symbol #1: %zu\n", syms[0].book.live_count());
    if (!truth_path.empty()) {
        std::printf("  ground truth: %s (%" PRIu64 " rows)\n", truth_path.c_str(), seq);
    }
    return 0;
}
