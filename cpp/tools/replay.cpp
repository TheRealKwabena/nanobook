// nanobook — replay: reconstruct a symbol's book from an ITCH feed, verify it
// against ground truth, and report throughput and tail latency.
//
// This is the tool that makes the rest of the repository checkable. It does two
// things that are normally left out of order-book projects:
//
//   1. VERIFIES. With --truth it compares the reconstructed top-of-book against
//      an independently computed reference after *every single message*, not at
//      the end of the run. A book that is right at the close and wrong at 10:15
//      is wrong, and end-state checks cannot see that.
//
//   2. REPORTS ITS OWN INTEGRITY. Unknown order references, negative level
//      quantities, overfills, crossed books and order-map probe depth are all
//      printed. A performance number from a run with nonzero corruption counters
//      is meaningless, so the numbers are shown together.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nanobook/book_builder.hpp"
#include "nanobook/itch_parser.hpp"
#include "nanobook/latency.hpp"

using namespace nanobook;

namespace {

// ---------------------------------------------------------------------------
// Streams the ground-truth CSV and checks one row per book event.
// ---------------------------------------------------------------------------
class TruthVerifier {
  public:
    explicit TruthVerifier(const std::string& path) {
        if (path.empty()) return;
        f_ = std::fopen(path.c_str(), "rb");
        if (f_ == nullptr) {
            std::fprintf(stderr, "replay: cannot open truth file %s: %s\n",
                         path.c_str(), std::strerror(errno));
            std::exit(1);
        }
        char line[256];
        if (std::fgets(line, sizeof line, f_) == nullptr) {  // discard header
            std::fprintf(stderr, "replay: truth file is empty\n");
            std::exit(1);
        }
        active_ = true;
    }
    ~TruthVerifier() { if (f_) std::fclose(f_); }

    TruthVerifier(const TruthVerifier&) = delete;
    TruthVerifier& operator=(const TruthVerifier&) = delete;

    void on_book_event(const OrderBook& book, char type, std::uint64_t ts) {
        if (!active_) return;

        char line[256];
        if (std::fgets(line, sizeof line, f_) == nullptr) {
            ++extra_events_;
            return;
        }
        ++rows_;

        // seq,ts,type,bid,bid_shares,ask,ask_shares
        std::uint64_t t_seq = 0, t_ts = 0;
        char t_type = 0;
        std::uint32_t t_bid = 0, t_ask = 0;
        std::int64_t t_bid_sh = 0, t_ask_sh = 0;
        if (std::sscanf(line, "%" SCNu64 ",%" SCNu64 ",%c,%u,%" SCNd64 ",%u,%" SCNd64,
                        &t_seq, &t_ts, &t_type, &t_bid, &t_bid_sh, &t_ask, &t_ask_sh) != 7) {
            ++parse_errors_;
            return;
        }

        const BookTop top = book.top();
        const std::uint32_t got_bid = top.bid_valid ? top.bid_price : 0;
        const std::uint32_t got_ask = top.ask_valid ? top.ask_price : 0;
        const std::int64_t got_bid_sh = top.bid_valid ? top.bid_shares : 0;
        const std::int64_t got_ask_sh = top.ask_valid ? top.ask_shares : 0;

        const bool ok = (t_type == type) && (t_ts == ts) &&
                        (t_bid == got_bid) && (t_ask == got_ask) &&
                        (t_bid_sh == got_bid_sh) && (t_ask_sh == got_ask_sh);
        if (ok) return;

        ++mismatches_;
        if (mismatches_ <= kMaxReported) {
            std::fprintf(stderr,
                "MISMATCH row %" PRIu64 " (seq %" PRIu64 ")\n"
                "  expected: type=%c ts=%" PRIu64 " bid=%u x%" PRId64 "  ask=%u x%" PRId64 "\n"
                "  actual  : type=%c ts=%" PRIu64 " bid=%u x%" PRId64 "  ask=%u x%" PRId64 "\n",
                rows_, t_seq,
                t_type, t_ts, t_bid, t_bid_sh, t_ask, t_ask_sh,
                type, ts, got_bid, got_bid_sh, got_ask, got_ask_sh);
        }
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::uint64_t mismatches() const noexcept { return mismatches_; }
    [[nodiscard]] std::uint64_t extra_events() const noexcept { return extra_events_; }
    [[nodiscard]] std::uint64_t parse_errors() const noexcept { return parse_errors_; }

    // Any truth rows the replay never reached.
    [[nodiscard]] std::uint64_t remaining_rows() {
        if (!active_) return 0;
        std::uint64_t n = 0;
        char line[256];
        while (std::fgets(line, sizeof line, f_) != nullptr) ++n;
        return n;
    }

  private:
    static constexpr std::uint64_t kMaxReported = 10;
    std::FILE* f_ = nullptr;
    bool active_ = false;
    std::uint64_t rows_ = 0, mismatches_ = 0, extra_events_ = 0, parse_errors_ = 0;
};

// Wraps the builder so the block timer sees every framed message, including the
// ones filtered out by symbol — that filter cost is part of the real workload.
template <class Inner>
struct TimedHandler {
    Inner& inner;
    BlockTimer& timer;

    void on_message(const itch::MsgView& m) { inner.on_message(m); timer.tick(); }
    void on_system_event(const itch::SystemEvent& m) { inner.on_system_event(m); }
    void on_stock_directory(const itch::StockDirectory& m) { inner.on_stock_directory(m); }
    void on_trading_action(const itch::StockTradingAction& m) { inner.on_trading_action(m); }
    void on_add_order(const itch::AddOrder& m) { inner.on_add_order(m); }
    void on_add_order_mpid(const itch::AddOrderMpid& m) { inner.on_add_order_mpid(m); }
    void on_order_executed(const itch::OrderExecuted& m) { inner.on_order_executed(m); }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        inner.on_order_executed_with_price(m);
    }
    void on_order_cancel(const itch::OrderCancel& m) { inner.on_order_cancel(m); }
    void on_order_delete(const itch::OrderDelete& m) { inner.on_order_delete(m); }
    void on_order_replace(const itch::OrderReplace& m) { inner.on_order_replace(m); }
    void on_trade(const itch::TradeNonCross& m) { inner.on_trade(m); }
    void on_cross_trade(const itch::CrossTrade& m) { inner.on_cross_trade(m); }
    void on_broken_trade(const itch::BrokenTrade& m) { inner.on_broken_trade(m); }
    void on_noii(const itch::Noii& m) { inner.on_noii(m); }
    void on_other(const itch::MsgView& m) { inner.on_other(m); }
};

void usage() {
    std::fprintf(stderr,
        "replay — reconstruct an order book from an ITCH 5.0 feed\n\n"
        "usage: replay --feed FILE --symbol TICKER [options]\n"
        "  --feed FILE     ITCH feed (BinaryFILE framing)        [required]\n"
        "  --symbol TICK   symbol to reconstruct                 [required]\n"
        "  --truth FILE    verify against ground-truth CSV       [optional]\n"
        "  --tick N        price tick in 1/10000 units           [100 = 1 cent]\n"
        "  --depth N       book levels to print at the end       [5]\n"
        "  --raw           no length prefix (MoldUDP64 payload)  [off]\n"
        "  --quiet         suppress the final book printout\n");
}

void print_price(char* buf, std::size_t n, itch::Price4 p) {
    std::snprintf(buf, n, "%u.%04u", p / itch::kPriceScale, p % itch::kPriceScale);
}

}  // namespace

int main(int argc, char** argv) {
    std::string feed, symbol, truth_path;
    itch::Price4 tick = 100;
    std::size_t depth = 5;
    bool raw = false, quiet = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); std::exit(1); }
            return argv[++i];
        };
        if (!std::strcmp(a, "--feed")) feed = next();
        else if (!std::strcmp(a, "--symbol")) symbol = next();
        else if (!std::strcmp(a, "--truth")) truth_path = next();
        else if (!std::strcmp(a, "--tick")) tick = static_cast<itch::Price4>(std::strtoul(next(), nullptr, 10));
        else if (!std::strcmp(a, "--depth")) depth = std::strtoul(next(), nullptr, 10);
        else if (!std::strcmp(a, "--raw")) raw = true;
        else if (!std::strcmp(a, "--quiet")) quiet = true;
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) { usage(); return 0; }
        else { std::fprintf(stderr, "replay: unknown argument %s\n", a); usage(); return 1; }
    }
    if (feed.empty() || symbol.empty()) { usage(); return 1; }

    MappedFile mf(feed);
    TruthVerifier verifier(truth_path);
    BookBuilder<TruthVerifier> builder(symbol, verifier, tick);
    BlockTimer timer;
    TimedHandler<BookBuilder<TruthVerifier>> handler{builder, timer};

    timer.start();
    const ParseStats st = parse(mf, handler, raw ? Framing::Raw : Framing::BinaryFile);
    timer.stop();

    // ---------------- feed ----------------
    const double mb = static_cast<double>(mf.size()) / (1024.0 * 1024.0);
    const double secs = static_cast<double>(timer.run_ns()) / 1e9;
    std::printf("feed\n");
    std::printf("  file                %s (%.1f MB)\n", feed.c_str(), mb);
    std::printf("  messages parsed     %" PRIu64 "\n", st.messages);
    std::printf("  framing errors      %" PRIu64 "\n", st.framing_errors);
    std::printf("  unknown types       %" PRIu64 "\n", st.unknown_type);
    std::printf("  length mismatches   %" PRIu64 "\n", st.length_mismatch);
    std::printf("  truncated tail      %" PRIu64 " bytes\n", st.truncated_tail);
    std::printf("  message mix        ");
    for (char t : {'A','F','E','C','X','D','U','P','Q','R','H','S','I'}) {
        if (st.count(t) > 0) {
            std::printf(" %c=%.1f%%", t,
                        100.0 * static_cast<double>(st.count(t)) / static_cast<double>(st.messages));
        }
    }
    std::printf("\n\n");

    // ---------------- throughput ----------------
    std::printf("throughput  (whole-run timing: one clock read either side, so instrument cost is nil)\n");
    std::printf("  wall time           %.3f s\n", secs);
    std::printf("  messages/sec        %.2f M\n",
                secs > 0 ? static_cast<double>(st.messages) / secs / 1e6 : 0.0);
    std::printf("  ns/message (mean)   %.1f\n",
                st.messages ? static_cast<double>(timer.run_ns()) / static_cast<double>(st.messages) : 0.0);
    std::printf("  feed rate           %.0f MB/s\n\n", secs > 0 ? mb / secs : 0.0);

    // ---------------- tail ----------------
    std::printf("per-message cost, from the distribution across %" PRIu64 "-message blocks\n",
                timer.block_size());
    std::printf("  (clock granularity is %.1f ns, so a single message cannot be timed directly;\n",
                timer.tick_ns());
    std::printf("   each block spans ~%.0f ticks. the upper tail is where stalls show up.)\n",
                timer.blocks() ? (static_cast<double>(timer.run_ns()) / static_cast<double>(timer.blocks())) / timer.tick_ns() : 0.0);
    std::printf("  blocks sampled      %" PRIu64 "\n", timer.blocks());
    for (double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
        std::printf("  p%-6.4g            %8.1f ns/msg\n", p, timer.block_percentile_ns(p));
    }
    std::printf("  max block           %8.1f ns/msg\n\n",
                static_cast<double>(timer.hist().max()) / 1000.0);

    // ---------------- book integrity ----------------
    const auto d = builder.book().diagnostics();
    std::printf("book  (%s", symbol.c_str());
    if (builder.resolved()) std::printf(", locate %d", builder.locate());
    else std::printf(", NOT FOUND in the stock directory");
    std::printf(")\n");
    std::printf("  events applied      %" PRIu64 "\n", builder.applied());
    std::printf("  add/exec/cancel/del/repl  %" PRIu64 " / %" PRIu64 " / %" PRIu64 " / %" PRIu64 " / %" PRIu64 "\n",
                d.adds, d.executes, d.cancels, d.deletes, d.replaces);
    std::printf("  live orders at end  %zu\n", d.live_orders);
    std::printf("  levels bid/ask      %zu / %zu\n", d.bid_levels, d.ask_levels);
    std::printf("  session / trading   %c / %c\n", builder.session_state(), builder.trading_state());
    std::printf("  tape trades/shares  %" PRIu64 " / %" PRIu64 "\n",
                builder.book().tape().trades, builder.book().tape().shares_traded);
    std::printf("  hidden ('P') trades %" PRIu64 " (%" PRIu64 " shares, book untouched)\n",
                builder.book().tape().hidden_trades, builder.book().tape().hidden_shares);

    std::printf("\norder map\n");
    std::printf("  capacity            %zu (load %.2f)\n", d.order_map_capacity, d.order_map_load_factor);
    std::printf("  mean probes/lookup  %.3f\n", d.order_map_mean_probes);
    std::printf("  longest probe run   %zu\n", d.order_map_max_probe);

    const std::uint64_t corruption =
        d.unknown_ref_exec + d.unknown_ref_cancel + d.unknown_ref_delete + d.unknown_ref_replace +
        d.overfill + d.overcancel + d.negative_level_qty + d.missing_level + d.off_tick +
        d.unrepresentable_price + d.duplicate_order_refs;

    std::printf("\nintegrity  (all zero == clean reconstruction)\n");
    std::printf("  unknown ref  E/X/D/U   %" PRIu64 " / %" PRIu64 " / %" PRIu64 " / %" PRIu64 "\n",
                d.unknown_ref_exec, d.unknown_ref_cancel, d.unknown_ref_delete, d.unknown_ref_replace);
    std::printf("  overfill / overcancel  %" PRIu64 " / %" PRIu64 "\n", d.overfill, d.overcancel);
    std::printf("  negative level qty     %" PRIu64 "\n", d.negative_level_qty);
    std::printf("  removal, no such level %" PRIu64 "\n", d.missing_level);
    std::printf("  off-tick prices        %" PRIu64 "\n", d.off_tick);
    std::printf("  duplicate order refs   %" PRIu64 "\n", d.duplicate_order_refs);
    std::printf("  crossed book at end    %s\n", builder.book().is_crossed() ? "YES" : "no");
    std::printf("  TOTAL                  %" PRIu64 "\n", corruption);

    // ---------------- final book ----------------
    if (!quiet && depth > 0) {
        std::vector<LevelView> b(depth), a(depth);
        const std::size_t nb = builder.book().bids().top_of_book(b.data(), depth);
        const std::size_t na = builder.book().asks().top_of_book(a.data(), depth);
        std::printf("\nbook at end of feed (top %zu)\n", depth);
        std::printf("      %12s %8s %6s  |  %12s %8s %6s\n",
                    "bid", "shares", "ords", "ask", "shares", "ords");
        char pb[32], pa[32];
        for (std::size_t i = 0; i < depth; ++i) {
            std::printf("  %2zu  ", i);
            if (i < nb) { print_price(pb, sizeof pb, b[i].price);
                          std::printf("%12s %8" PRId64 " %6u", pb, b[i].shares, b[i].order_count); }
            else std::printf("%12s %8s %6s", "-", "-", "-");
            std::printf("  |  ");
            if (i < na) { print_price(pa, sizeof pa, a[i].price);
                          std::printf("%12s %8" PRId64 " %6u", pa, a[i].shares, a[i].order_count); }
            else std::printf("%12s %8s %6s", "-", "-", "-");
            std::printf("\n");
        }
        std::int64_t spr = 0;
        if (builder.book().spread(spr)) {
            std::printf("  spread %.4f (%.0f ticks)\n",
                        static_cast<double>(spr) / itch::kPriceScale,
                        static_cast<double>(spr) / static_cast<double>(tick));
        }
    }

    // ---------------- verification verdict ----------------
    int exit_code = 0;
    if (verifier.active()) {
        const std::uint64_t left = verifier.remaining_rows();
        std::printf("\nverification against ground truth\n");
        std::printf("  rows compared       %" PRIu64 "\n", verifier.rows());
        std::printf("  mismatches          %" PRIu64 "\n", verifier.mismatches());
        std::printf("  events beyond truth %" PRIu64 "\n", verifier.extra_events());
        std::printf("  truth rows unused   %" PRIu64 "\n", left);
        std::printf("  csv parse errors    %" PRIu64 "\n", verifier.parse_errors());
        const bool pass = verifier.mismatches() == 0 && verifier.extra_events() == 0 &&
                          left == 0 && verifier.parse_errors() == 0 && verifier.rows() > 0;
        std::printf("  RESULT              %s\n", pass ? "PASS — byte-exact match on every event"
                                                       : "FAIL");
        if (!pass) exit_code = 1;
    }
    if (corruption != 0) exit_code = 1;
    return exit_code;
}
