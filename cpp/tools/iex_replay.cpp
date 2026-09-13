// nanobook — iex_replay: reconstruct books from an IEX DEEP pcap and emit a
// microstructure feature table.
//
// Reads from a file or from stdin, which is the point: a day of DEEP is 11-12 GB
// gzipped, so the daily pipeline runs
//
//     curl -sL "$URL" | gunzip | iex_replay --pcap - --symbols AAPL,MSFT --out day.csv
//
// and never writes the capture to disk. Peak memory is one 1 MiB stream buffer
// plus one ladder per watched symbol.
//
// Features are sampled on a fixed time grid and only ever at completed book
// transactions, so no row contains a BBO that never existed on the market.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nanobook/iex_book.hpp"
#include "nanobook/iex_transport.hpp"
#include "nanobook/latency.hpp"
#include "nanobook/stream_buffer.hpp"

using namespace nanobook;
using namespace nanobook::iex;

namespace {

// Depth window for the book-pressure features, in ticks from the touch.
constexpr std::size_t kDepthTicks = 4;

class CsvSampler {
  public:
    CsvSampler(const std::string& path, const std::vector<std::string>& symbols)
        : symbols_(symbols) {
        if (path.empty()) return;
        f_ = (path == "-") ? stdout : std::fopen(path.c_str(), "wb");
        if (f_ == nullptr) {
            std::fprintf(stderr, "iex_replay: cannot open %s: %s\n", path.c_str(),
                         std::strerror(errno));
            std::exit(1);
        }
        // Prices are emitted as integers in 1/10000 so nothing is lost to a
        // decimal representation; the analysis layer divides once at the end.
        std::fprintf(f_,
            "ts_ns,symbol,bid,bid_shares,ask,ask_shares,spread,"
            "bid_depth,ask_depth,bid_levels,ask_levels,"
            "trades,trade_shares,buy_shares,sell_shares,unclassified_shares,"
            "notional_cents,last_price,odd_lot_trades,sweep_trades,trading_status\n");
    }

    ~CsvSampler() { if (f_ != nullptr && f_ != stdout) std::fclose(f_); }
    CsvSampler(const CsvSampler&) = delete;
    CsvSampler& operator=(const CsvSampler&) = delete;

    void on_sample(std::uint32_t i, DeepBook& book, std::int64_t ts) {
        ++rows_;
        IntervalTape& tp = book.tape();
        if (f_ != nullptr) {
            const StableTop& t = book.stable_top();
            std::int64_t spread = -1;
            (void)t.spread(spread);

            std::fprintf(f_,
                "%" PRId64 ",%s,%u,%" PRId64 ",%u,%" PRId64 ",%" PRId64
                ",%" PRId64 ",%" PRId64 ",%zu,%zu,"
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
                "%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%c\n",
                ts, symbols_[i].c_str(),
                t.bid_valid ? t.bid : 0, t.bid_valid ? t.bid_shares : 0,
                t.ask_valid ? t.ask : 0, t.ask_valid ? t.ask_shares : 0,
                spread,
                book.bids().shares_within(kDepthTicks), book.asks().shares_within(kDepthTicks),
                book.bids().level_count(), book.asks().level_count(),
                tp.trades, tp.shares, tp.buy_shares, tp.sell_shares, tp.unclassified_shares,
                tp.notional_cents, tp.last_price, tp.odd_lot_trades, tp.sweep_trades,
                book.trading_status());
        }
        // Interval aggregates are per-row, so they reset after every sample. The
        // book state itself of course persists.
        tp.reset();
    }

    [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }

  private:
    std::FILE* f_ = nullptr;
    const std::vector<std::string>& symbols_;
    std::uint64_t rows_ = 0;
};

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t comma = s.find(',', start);
        const std::string tok = s.substr(start, comma == std::string::npos ? std::string::npos
                                                                          : comma - start);
        if (!tok.empty()) out.push_back(tok);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

void usage() {
    std::fprintf(stderr,
        "iex_replay — reconstruct order books from an IEX DEEP pcap\n\n"
        "usage: iex_replay --pcap FILE|- --symbols A,B,C [options]\n"
        "  --pcap FILE     DEEP pcap; '-' reads stdin (use with curl | gunzip)\n"
        "  --symbols LIST  comma-separated tickers to reconstruct        [required]\n"
        "  --out FILE      feature CSV ('-' for stdout)                  [none]\n"
        "  --sample-ms N   feature grid in milliseconds, 0 = every event [1000]\n"
        "  --tick N        price tick in 1/10000 units                   [100 = 1c]\n"
        "  --max-packets N stop after N packets (for quick smoke tests)  [0 = all]\n"
        "  --progress      print progress to stderr while streaming\n"
        "  --quiet         suppress the end-of-run report\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string pcap_path, symbols_arg, out_path;
    std::int64_t sample_ms = 1000;
    Price4 tick = 100;
    std::uint64_t max_packets = 0;
    bool progress = false, quiet = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); std::exit(1); }
            return argv[++i];
        };
        if (!std::strcmp(a, "--pcap")) pcap_path = next();
        else if (!std::strcmp(a, "--symbols")) symbols_arg = next();
        else if (!std::strcmp(a, "--out")) out_path = next();
        else if (!std::strcmp(a, "--sample-ms")) sample_ms = std::strtoll(next(), nullptr, 10);
        else if (!std::strcmp(a, "--tick")) tick = static_cast<Price4>(std::strtoul(next(), nullptr, 10));
        else if (!std::strcmp(a, "--max-packets")) max_packets = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(a, "--progress")) progress = true;
        else if (!std::strcmp(a, "--quiet")) quiet = true;
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) { usage(); return 0; }
        else { std::fprintf(stderr, "iex_replay: unknown argument %s\n", a); usage(); return 1; }
    }
    if (pcap_path.empty() || symbols_arg.empty()) { usage(); return 1; }

    const std::vector<std::string> symbols = split_csv(symbols_arg);
    if (symbols.empty()) { std::fprintf(stderr, "iex_replay: no symbols given\n"); return 1; }
    for (const std::string& s : symbols) {
        if (s.size() > 8) {
            std::fprintf(stderr, "iex_replay: symbol '%s' exceeds 8 characters\n", s.c_str());
            return 1;
        }
    }

    std::FILE* in_file = (pcap_path == "-") ? stdin : std::fopen(pcap_path.c_str(), "rb");
    if (in_file == nullptr) {
        std::fprintf(stderr, "iex_replay: cannot open %s: %s\n", pcap_path.c_str(),
                     std::strerror(errno));
        return 1;
    }

    StreamBuffer in(in_file);
    CsvSampler sampler(out_path, symbols);
    DeepBookRouter<CsvSampler> router(symbols, sampler, sample_ms * 1'000'000, tick);

    // stream_deep_pcap owns the loop, so progress and packet limits are applied
    // through a thin wrapper handler rather than by duplicating the loop here.
    struct Wrapper {
        DeepBookRouter<CsvSampler>& r;
        bool show;
        std::uint64_t segments = 0;

        void on_segment(const SegmentHeader& h) {
            ++segments;
            if (show && (segments & 0xFFFFF) == 0) {
                std::fprintf(stderr, "\r  %" PRIu64 "M segments, session %u ...",
                             segments / 1000000, h.session_id);
                std::fflush(stderr);
            }
            r.on_segment(h);
        }
        void on_message(std::uint8_t t, const std::byte* b, std::size_t n) { r.on_message(t, b, n); }
        void on_system_event(const SystemEvent& m) { r.on_system_event(m); }
        void on_security_directory(const SecurityDirectory& m) { r.on_security_directory(m); }
        void on_trading_status(const TradingStatus& m) { r.on_trading_status(m); }
        void on_operational_halt(const OperationalHaltStatus& m) { r.on_operational_halt(m); }
        void on_short_sale_price_test(const ShortSalePriceTestStatus& m) { r.on_short_sale_price_test(m); }
        void on_retail_liquidity(const RetailLiquidityIndicator& m) { r.on_retail_liquidity(m); }
        void on_security_event(const SecurityEvent& m) { r.on_security_event(m); }
        void on_price_level_update(const PriceLevelUpdate& m) { r.on_price_level_update(m); }
        void on_trade_report(const TradeReport& m) { r.on_trade_report(m); }
        void on_trade_break(const TradeBreak& m) { r.on_trade_break(m); }
        void on_official_price(const OfficialPrice& m) { r.on_official_price(m); }
        void on_auction_information(const AuctionInformation& m) { r.on_auction_information(m); }
    };

    std::string error;
    const std::uint64_t t0 = now_ns();
    TransportStats st;
    {
        Wrapper w{router, progress};
        st = stream_deep_pcap(in, w, error, kProtocolDeep1_0, max_packets);
    }
    const std::uint64_t elapsed = now_ns() - t0;
    router.flush();
    if (progress) std::fprintf(stderr, "\r%60s\r", "");

    if (in_file != stdin) std::fclose(in_file);

    if (!error.empty()) {
        std::fprintf(stderr, "iex_replay: %s\n", error.c_str());
        return 2;
    }
    if (quiet) return st.defects() == 0 ? 0 : 1;

    const double secs = static_cast<double>(elapsed) / 1e9;
    const double mb = static_cast<double>(st.capture.bytes) / (1024.0 * 1024.0);

    std::fprintf(stderr, "\ncapture\n");
    std::fprintf(stderr, "  packets             %" PRIu64 "\n", st.capture.packets);
    std::fprintf(stderr, "  bytes               %.1f MB\n", mb);
    std::fprintf(stderr, "  non-IP / non-UDP    %" PRIu64 " / %" PRIu64 "\n",
                 st.capture.non_ip_packets, st.capture.non_udp_packets);
    std::fprintf(stderr, "  truncated packets   %" PRIu64 "\n", st.capture.truncated_packets);

    std::fprintf(stderr, "\nIEX-TP\n");
    std::fprintf(stderr, "  segments            %" PRIu64 "\n", st.segments);
    std::fprintf(stderr, "  heartbeats          %" PRIu64 "\n", st.heartbeats);
    std::fprintf(stderr, "  messages            %" PRIu64 "\n", st.messages);
    std::fprintf(stderr, "  wrong protocol id   %" PRIu64 "\n", st.wrong_protocol);
    std::fprintf(stderr, "  session changes     %" PRIu64 "\n", st.session_changes);
    std::fprintf(stderr, "  sequence gaps       %" PRIu64 " (%" PRIu64 " messages missing)\n",
                 st.sequence_gaps, st.sequence_gap_messages);
    std::fprintf(stderr, "  sequence regressions %" PRIu64 "\n", st.sequence_regressions);
    std::fprintf(stderr, "  unknown msg types   %" PRIu64 "\n", st.unknown_message_type);
    std::fprintf(stderr, "  length mismatches   %" PRIu64 "\n", st.length_mismatch);
    std::fprintf(stderr, "  payload mismatches  %" PRIu64 "\n", st.payload_length_mismatch);

    std::fprintf(stderr, "\nthroughput\n");
    std::fprintf(stderr, "  wall time           %.2f s\n", secs);
    std::fprintf(stderr, "  messages/sec        %.2f M\n",
                 secs > 0 ? static_cast<double>(st.messages) / secs / 1e6 : 0.0);
    std::fprintf(stderr, "  decode rate         %.0f MB/s\n", secs > 0 ? mb / secs : 0.0);

    std::fprintf(stderr, "\nbooks  (system event '%c')\n",
                 router.system_event() ? static_cast<char>(router.system_event()) : '?');
    std::fprintf(stderr, "  matched messages    %" PRIu64 "\n", router.matched_messages());
    std::fprintf(stderr, "  feature rows        %" PRIu64 "\n", sampler.rows());
    std::fprintf(stderr, "  %-8s %10s %10s %12s %8s %8s %9s\n",
                 "symbol", "updates", "txns", "in-transit", "trades", "crossed", "bad px");
    for (std::uint32_t i = 0; i < router.symbol_count(); ++i) {
        DeepBook& b = router.book(i);
        std::fprintf(stderr, "  %-8s %10" PRIu64 " %10" PRIu64 " %12" PRIu64
                             " %8" PRIu64 " %8" PRIu64 " %9" PRIu64 "\n",
                     symbols[i].c_str(), b.updates(), b.transactions(),
                     b.in_transition_updates(), b.total_trades(),
                     b.crossed_transactions(), b.bad_prices());
    }

    std::fprintf(stderr, "\n  transport defects   %" PRIu64 "%s\n", st.defects(),
                 st.defects() == 0 ? "  (clean)" : "  <-- results from this run are suspect");
    return st.defects() == 0 ? 0 : 1;
}
