// nanobook — iex_book.hpp
//
// Multi-symbol book reconstruction from IEX DEEP, plus the microstructure
// aggregates a research pipeline needs.
//
// THE ATOMICITY RULE, which is the whole reason this file is not trivial:
//
// A single order book event may change several price levels at once — one
// aggressive order sweeping three levels, say. DEEP describes that as a run of
// Price Level Updates with the event flag OFF, terminated by one with the flag ON.
// The specification is explicit that the book keeps its previous BBO for the whole
// run, and that any intermediate BBO "never truly existed".
//
// So there are two different notions of "the book" here:
//
//   * the ladders, which are updated on every message as they arrive; and
//   * `stable_top()`, the BBO as of the last COMPLETED transaction, which is the
//     only one a feature or a signal may look at.
//
// Sampling the touch mid-transition invents quotes that were never on the market.
// They look like real, tradeable mispricings — a spread that momentarily gapped,
// a mid that jumped and came back — and a backtest will happily "trade" them. It
// is the most dangerous kind of bug in this codebase, because it makes results
// better rather than worse.
//
// Trade classification: DEEP does not say which side initiated a trade, so the
// aggressor is inferred with the Lee-Ready quote rule (compare the print to the
// prevailing midpoint, using the stable BBO from before the trade). A print above
// the mid is buy-initiated, below is sell-initiated, exactly at the mid is
// unclassified rather than guessed.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nanobook/iex_spec.hpp"
#include "nanobook/iex_transport.hpp"
#include "nanobook/order_map.hpp"   // for mix64
#include "nanobook/price_ladder.hpp"

namespace nanobook::iex {

// ---------------------------------------------------------------------------
// Watchlist lookup: 8-byte symbol word -> book slot, or absent.
//
// DEEP carries no locate, so every one of the day's ~2 billion messages needs a
// symbol test. At IEX volumes a linear scan over even 20 symbols is the dominant
// cost of the whole decode, so this is an open-addressing table keyed by the
// packed symbol word: one multiply-shift hash and usually one probe.
// ---------------------------------------------------------------------------
class SymbolTable {
  public:
    static constexpr std::uint32_t kAbsent = 0xFFFFFFFFu;

    void build(const std::vector<std::string>& symbols) {
        std::size_t cap = 16;
        while (cap < symbols.size() * 4) cap <<= 1;
        keys_.assign(cap, 0);
        slots_.assign(cap, kAbsent);
        mask_ = cap - 1;
        for (std::uint32_t i = 0; i < symbols.size(); ++i) {
            const Symbol8 s(symbols[i]);
            std::size_t j = mix64(s.word()) & mask_;
            while (slots_[j] != kAbsent) j = (j + 1) & mask_;
            keys_[j] = s.word();
            slots_[j] = i;
        }
    }

    [[nodiscard]] std::uint32_t find(const Symbol8& s) const noexcept {
        std::size_t j = mix64(s.word()) & mask_;
        for (;;) {
            if (slots_[j] == kAbsent) return kAbsent;
            if (keys_[j] == s.word()) return slots_[j];
            j = (j + 1) & mask_;
        }
    }

  private:
    std::vector<std::uint64_t> keys_;
    std::vector<std::uint32_t> slots_;
    std::size_t mask_ = 0;
};

// ---------------------------------------------------------------------------
// Top of book as of a completed transaction.
// ---------------------------------------------------------------------------
struct StableTop {
    Price4 bid = 0, ask = 0;
    std::int64_t bid_shares = 0, ask_shares = 0;
    bool bid_valid = false, ask_valid = false;

    // Twice the midpoint, kept integral so a half-cent mid is not rounded away.
    [[nodiscard]] bool mid_x2(std::int64_t& out) const noexcept {
        if (!bid_valid || !ask_valid) return false;
        out = static_cast<std::int64_t>(bid) + static_cast<std::int64_t>(ask);
        return true;
    }
    [[nodiscard]] bool spread(std::int64_t& out) const noexcept {
        if (!bid_valid || !ask_valid) return false;
        out = static_cast<std::int64_t>(ask) - static_cast<std::int64_t>(bid);
        return true;
    }
};

// Per-symbol aggregates accumulated between samples, then reset.
struct IntervalTape {
    std::uint64_t trades = 0;
    std::uint64_t shares = 0;
    std::uint64_t buy_shares = 0;         // Lee-Ready: print above the mid
    std::uint64_t sell_shares = 0;        // print below the mid
    std::uint64_t unclassified_shares = 0;
    std::uint64_t notional_cents = 0;
    Price4 last_price = 0;
    std::uint64_t odd_lot_trades = 0;
    std::uint64_t sweep_trades = 0;

    void reset() { *this = IntervalTape{}; }
};

// ---------------------------------------------------------------------------
// One symbol's book.
// ---------------------------------------------------------------------------
class DeepBook {
  public:
    explicit DeepBook(Price4 tick = 100)
        : bids_(Side::Buy, tick), asks_(Side::Sell, tick) {}

    // Apply a price level update. Returns true if this message completed a
    // transaction, i.e. the BBO just became meaningful again.
    bool apply(const PriceLevelUpdate& m) {
        Price4 px = 0;
        if (!price_to_p4(m.price(), px)) {
            ++bad_price_;
            // Still honour the transaction boundary: dropping the flag would
            // desynchronise every later sample.
            return finish_if_complete(m);
        }

        PriceLadder& l = (m.side() == Side::Buy) ? bids_ : asks_;
        if (!l.set_level(px, m.size())) ++unrepresentable_;
        ++updates_;
        if (!m.event_complete()) ++in_transition_updates_;
        ts_ = m.timestamp();
        return finish_if_complete(m);
    }

    void apply(const TradeReport& t) {
        Price4 px = 0;
        if (!price_to_p4(t.price(), px)) { ++bad_price_; return; }
        ts_ = t.timestamp();

        ++tape_.trades;
        tape_.shares += t.size();
        tape_.last_price = px;
        // Price is in 1/10000; cents = price4 * shares / 100.
        tape_.notional_cents += static_cast<std::uint64_t>(px) * t.size() / 100;
        if (t.odd_lot()) ++tape_.odd_lot_trades;
        if (t.intermarket_sweep()) ++tape_.sweep_trades;

        // Lee-Ready quote rule, against the BBO from BEFORE this trade. Using the
        // post-trade book would classify by the consequence of the trade rather
        // than the state the aggressor actually saw.
        std::int64_t m2 = 0;
        if (stable_.mid_x2(m2)) {
            const std::int64_t px2 = 2 * static_cast<std::int64_t>(px);
            if (px2 > m2) tape_.buy_shares += t.size();
            else if (px2 < m2) tape_.sell_shares += t.size();
            else tape_.unclassified_shares += t.size();
        } else {
            tape_.unclassified_shares += t.size();
        }
        ++total_trades_;
    }

    // The only BBO a feature may read: as of the last completed transaction.
    [[nodiscard]] const StableTop& stable_top() const noexcept { return stable_; }
    [[nodiscard]] const PriceLadder& bids() const noexcept { return bids_; }
    [[nodiscard]] const PriceLadder& asks() const noexcept { return asks_; }
    [[nodiscard]] IntervalTape& tape() noexcept { return tape_; }
    [[nodiscard]] const IntervalTape& tape() const noexcept { return tape_; }
    [[nodiscard]] std::int64_t timestamp() const noexcept { return ts_; }
    [[nodiscard]] std::uint8_t trading_status() const noexcept { return trading_status_; }
    void set_trading_status(std::uint8_t s) noexcept { trading_status_ = s; }

    [[nodiscard]] std::uint64_t updates() const noexcept { return updates_; }
    [[nodiscard]] std::uint64_t transactions() const noexcept { return transactions_; }
    [[nodiscard]] std::uint64_t in_transition_updates() const noexcept { return in_transition_updates_; }
    [[nodiscard]] std::uint64_t bad_prices() const noexcept { return bad_price_; }
    [[nodiscard]] std::uint64_t unrepresentable() const noexcept { return unrepresentable_; }
    [[nodiscard]] std::uint64_t total_trades() const noexcept { return total_trades_; }
    [[nodiscard]] std::uint64_t crossed_transactions() const noexcept { return crossed_; }

  private:
    bool finish_if_complete(const PriceLevelUpdate& m) {
        if (!m.event_complete()) return false;
        ++transactions_;
        refresh_stable();
        return true;
    }

    void refresh_stable() {
        LevelView lv{};
        stable_ = StableTop{};
        if (bids_.best(lv)) {
            stable_.bid_valid = true;
            stable_.bid = lv.price;
            stable_.bid_shares = lv.shares;
        }
        if (asks_.best(lv)) {
            stable_.ask_valid = true;
            stable_.ask = lv.price;
            stable_.ask_shares = lv.shares;
        }
        // A crossed book after a completed transaction should not happen on a
        // single venue. Locked (bid == ask) can occur transiently; crossed means
        // the reconstruction or the capture is wrong.
        if (stable_.bid_valid && stable_.ask_valid && stable_.bid > stable_.ask) ++crossed_;
    }

    PriceLadder bids_, asks_;
    StableTop   stable_;
    IntervalTape tape_;
    std::int64_t ts_ = 0;
    std::uint8_t trading_status_ = '?';
    std::uint64_t updates_ = 0, transactions_ = 0, in_transition_updates_ = 0;
    std::uint64_t bad_price_ = 0, unrepresentable_ = 0, total_trades_ = 0, crossed_ = 0;
};

// ---------------------------------------------------------------------------
// Handler: routes DEEP messages to the watched symbols' books and samples
// features on a fixed time grid.
//
// Observers receive `on_sample(symbol_index, book, sample_ts)` and are called only
// at transaction boundaries, so the book they see is always self-consistent.
// ---------------------------------------------------------------------------
struct NullSampler {
    void on_sample(std::uint32_t, DeepBook&, std::int64_t) {}
};

template <class Sampler>
class DeepBookRouter : public DeepHandlerBase {
  public:
    // `sample_ns` == 0 samples at every completed transaction; otherwise features
    // are emitted on a fixed grid, which is what keeps a day of output to
    // megabytes instead of gigabytes.
    DeepBookRouter(const std::vector<std::string>& symbols, Sampler& s,
                   std::int64_t sample_ns = 1'000'000'000, Price4 tick = 100)
        : sampler_(&s), sample_ns_(sample_ns) {
        table_.build(symbols);
        books_.reserve(symbols.size());
        for (std::size_t i = 0; i < symbols.size(); ++i) books_.emplace_back(tick);
        next_sample_.assign(symbols.size(), 0);
        n_symbols_ = symbols.size();
    }

    void on_price_level_update(const PriceLevelUpdate& m) {
        const std::uint32_t i = table_.find(m.symbol());
        if (i == SymbolTable::kAbsent) return;
        ++matched_;
        if (books_[i].apply(m)) maybe_sample(i, m.timestamp());
    }

    void on_trade_report(const TradeReport& t) {
        const std::uint32_t i = table_.find(t.symbol());
        if (i == SymbolTable::kAbsent) return;
        ++matched_;
        books_[i].apply(t);
    }

    void on_trading_status(const TradingStatus& s) {
        const std::uint32_t i = table_.find(s.symbol());
        if (i == SymbolTable::kAbsent) return;
        books_[i].set_trading_status(s.status());
        ++status_changes_;
    }

    void on_system_event(const SystemEvent& e) { system_event_ = e.event(); }

    void on_security_event(const SecurityEvent& e) {
        const std::uint32_t i = table_.find(e.symbol());
        if (i == SymbolTable::kAbsent) return;
        // Receipt implies every preceding PLU for this symbol has been sent, so
        // the open/close book state is final. Force a sample to capture it.
        if (e.event() == kOpeningProcessComplete || e.event() == kClosingProcessComplete) {
            sampler_->on_sample(i, books_[i], e.timestamp());
            ++forced_samples_;
        }
    }

    void on_official_price(const OfficialPrice& p) {
        const std::uint32_t i = table_.find(p.symbol());
        if (i == SymbolTable::kAbsent) return;
        Price4 px = 0;
        if (price_to_p4(p.price(), px)) {
            if (p.price_type() == 'Q') official_open_[i] = px;
            else if (p.price_type() == 'M') official_close_[i] = px;
        }
    }

    // Flush a final sample per symbol at end of stream, so the closing state is
    // not lost just because the grid boundary never arrived.
    void flush() {
        for (std::uint32_t i = 0; i < n_symbols_; ++i) {
            if (books_[i].transactions() > 0) sampler_->on_sample(i, books_[i], books_[i].timestamp());
        }
    }

    [[nodiscard]] DeepBook& book(std::uint32_t i) noexcept { return books_[i]; }
    [[nodiscard]] std::size_t symbol_count() const noexcept { return n_symbols_; }
    [[nodiscard]] std::uint64_t matched_messages() const noexcept { return matched_; }
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    [[nodiscard]] std::uint64_t forced_samples() const noexcept { return forced_samples_; }
    [[nodiscard]] std::uint8_t system_event() const noexcept { return system_event_; }
    [[nodiscard]] Price4 official_open(std::uint32_t i) const {
        auto it = official_open_.find(i);
        return it == official_open_.end() ? 0 : it->second;
    }
    [[nodiscard]] Price4 official_close(std::uint32_t i) const {
        auto it = official_close_.find(i);
        return it == official_close_.end() ? 0 : it->second;
    }

  private:
    // The grid is a DOWNSAMPLER, not a clock. Every emitted row is stamped with
    // the book's actual event timestamp.
    //
    // Stamping rows with the grid boundary instead is a look-ahead leak, and a
    // severe one. On a thinly quoted symbol an event can arrive 100 seconds after
    // the boundary it triggers; the row would then claim time T while carrying
    // book state from T+100, so its features are contemporaneous with — or later
    // than — its own label. Any forward return computed from that label is partly
    // measuring the past, which shows up as a spectacular and entirely false
    // information coefficient.
    void maybe_sample(std::uint32_t i, std::int64_t ts) {
        if (sample_ns_ == 0) {
            sampler_->on_sample(i, books_[i], ts);
            ++samples_;
            return;
        }
        if (next_sample_[i] == 0) {
            next_sample_[i] = (ts / sample_ns_) * sample_ns_ + sample_ns_;
            return;
        }
        if (ts < next_sample_[i]) return;
        sampler_->on_sample(i, books_[i], ts);   // true event time, never the grid
        ++samples_;
        // Skip forward over quiet intervals rather than emitting a row per grid
        // step through a stretch where nothing happened.
        next_sample_[i] = (ts / sample_ns_) * sample_ns_ + sample_ns_;
    }

    Sampler* sampler_;
    SymbolTable table_;
    std::vector<DeepBook> books_;
    std::vector<std::int64_t> next_sample_;
    std::map<std::uint32_t, Price4> official_open_, official_close_;
    std::int64_t sample_ns_;
    std::size_t n_symbols_ = 0;
    std::uint64_t matched_ = 0, samples_ = 0, forced_samples_ = 0, status_changes_ = 0;
    std::uint8_t system_event_ = '?';
};

}  // namespace nanobook::iex
