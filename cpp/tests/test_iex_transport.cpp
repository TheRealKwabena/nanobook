// IEX capture and transport layers: pcap / pcap-ng containers, IEX-TP framing,
// and the DEEP book's atomic-transaction rule.
//
// The container tests build synthetic captures in both formats. This matters
// because the real HIST files turned out to be pcap-ng despite being named
// `.pcap.gz` — a decoder tested only against classic pcap rejected every real
// file, and a decoder tested only against pcap-ng would break on the older
// archive, which is classic.
#include <cstdio>
#include <string>
#include <vector>

#include "framework.hpp"
#include "nanobook/iex_book.hpp"
#include "nanobook/iex_transport.hpp"

using namespace nanobook;
using namespace nanobook::iex;

namespace {

// ---- little-endian append helpers ----
void put8(std::vector<std::byte>& v, std::uint8_t x) { v.push_back(static_cast<std::byte>(x)); }
void put16(std::vector<std::byte>& v, std::uint16_t x) { v.insert(v.end(), 2, std::byte{}); store_le16(&v[v.size()-2], x); }
void put32(std::vector<std::byte>& v, std::uint32_t x) { v.insert(v.end(), 4, std::byte{}); store_le32(&v[v.size()-4], x); }
void put64(std::vector<std::byte>& v, std::uint64_t x) { v.insert(v.end(), 8, std::byte{}); store_le64(&v[v.size()-8], x); }
void put64s(std::vector<std::byte>& v, std::int64_t x) { v.insert(v.end(), 8, std::byte{}); store_le64s(&v[v.size()-8], x); }
void put16be(std::vector<std::byte>& v, std::uint16_t x) { v.insert(v.end(), 2, std::byte{}); store_be16(&v[v.size()-2], x); }

void put_symbol(std::vector<std::byte>& v, const std::string& s) {
    for (std::size_t i = 0; i < 8; ++i) put8(v, i < s.size() ? static_cast<std::uint8_t>(s[i]) : ' ');
}

// ---- DEEP message encoders ----
std::vector<std::byte> plu(Side side, std::uint8_t flags, std::int64_t ts, const std::string& sym,
                           Shares size, Price8 price) {
    std::vector<std::byte> m;
    put8(m, side == Side::Buy ? 0x38 : 0x35);
    put8(m, flags);
    put64s(m, ts);
    put_symbol(m, sym);
    put32(m, size);
    put64s(m, price);
    return m;
}

std::vector<std::byte> trade(std::uint8_t flags, std::int64_t ts, const std::string& sym,
                             Shares size, Price8 price, std::int64_t id) {
    std::vector<std::byte> m;
    put8(m, 0x54);
    put8(m, flags);
    put64s(m, ts);
    put_symbol(m, sym);
    put32(m, size);
    put64s(m, price);
    put64s(m, id);
    return m;
}

std::vector<std::byte> trading_status(std::int64_t ts, const std::string& sym, char status) {
    std::vector<std::byte> m;
    put8(m, 0x48);
    put8(m, static_cast<std::uint8_t>(status));
    put64s(m, ts);
    put_symbol(m, sym);
    for (int i = 0; i < 4; ++i) put8(m, ' ');   // reason
    return m;
}

// ---- IEX-TP segment ----
std::vector<std::byte> segment(std::uint16_t protocol, std::uint32_t session, std::int64_t first_seq,
                               const std::vector<std::vector<std::byte>>& msgs) {
    std::vector<std::byte> payload;
    for (const auto& m : msgs) {
        put16(payload, static_cast<std::uint16_t>(m.size()));
        payload.insert(payload.end(), m.begin(), m.end());
    }
    std::vector<std::byte> seg;
    put8(seg, 1);                    // version
    put8(seg, 0);                    // reserved
    put16(seg, protocol);
    put32(seg, 1);                   // channel id
    put32(seg, session);
    put16(seg, static_cast<std::uint16_t>(payload.size()));
    put16(seg, static_cast<std::uint16_t>(msgs.size()));
    put64s(seg, 0);                  // stream offset
    put64s(seg, first_seq);
    put64s(seg, 0);                  // send time
    seg.insert(seg.end(), payload.begin(), payload.end());
    return seg;
}

// ---- Ethernet / IPv4 / UDP wrapper ----
// `ip_options` exercises the IHL-is-not-always-20 path.
std::vector<std::byte> udp_frame(const std::vector<std::byte>& payload, std::size_t ip_options = 0,
                                 int vlan_tags = 0) {
    std::vector<std::byte> f;
    for (int i = 0; i < 12; ++i) put8(f, 0);            // MACs
    for (int i = 0; i < vlan_tags; ++i) { put16be(f, 0x8100); put16be(f, 0x0064); }
    put16be(f, 0x0800);                                 // IPv4
    const std::size_t ihl = 20 + ip_options;
    put8(f, static_cast<std::uint8_t>(0x40 | (ihl / 4)));
    put8(f, 0);
    put16be(f, static_cast<std::uint16_t>(ihl + 8 + payload.size()));
    // identification(2) + flags/fragment(2) + ttl(1) + protocol(1) + checksum(2)
    // == 8 bytes, which together with the 4 preceding and the two 4-byte
    // addresses below makes the header exactly 20 bytes.
    for (int i = 0; i < 4; ++i) put16be(f, 0);
    // Rewrite protocol byte at offset (ethernet + vlan) + 9.
    const std::size_t ip_off = 14 + static_cast<std::size_t>(vlan_tags) * 4;
    f[ip_off + 9] = std::byte{17};                      // UDP
    for (int i = 0; i < 4; ++i) put8(f, 0);             // src IP
    for (int i = 0; i < 4; ++i) put8(f, 0);             // dst IP
    for (std::size_t i = 0; i < ip_options; ++i) put8(f, 0);
    put16be(f, 1234);                                   // src port
    put16be(f, 10378);                                  // dst port
    put16be(f, static_cast<std::uint16_t>(8 + payload.size()));
    put16be(f, 0);                                      // checksum
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// ---- containers ----
std::vector<std::byte> classic_pcap(const std::vector<std::vector<std::byte>>& frames, bool nanos) {
    std::vector<std::byte> c;
    put32(c, nanos ? 0xa1b23c4d : 0xa1b2c3d4);
    put16(c, 2); put16(c, 4);
    put32(c, 0); put32(c, 0);
    put32(c, 65535);
    put32(c, kLinktypeEthernet);
    for (const auto& f : frames) {
        put32(c, 1700000000);
        put32(c, nanos ? 123456789u : 123456u);
        put32(c, static_cast<std::uint32_t>(f.size()));
        put32(c, static_cast<std::uint32_t>(f.size()));
        c.insert(c.end(), f.begin(), f.end());
    }
    return c;
}

// Includes a Name Resolution Block that must be skipped, and if_tsresol = 9
// (nanoseconds) rather than the microsecond default.
std::vector<std::byte> pcapng(const std::vector<std::vector<std::byte>>& frames) {
    std::vector<std::byte> c;
    // Section Header Block with an opt_comment.
    const std::string comment = "File created by merging: test";
    const std::size_t copt = 4 + ((comment.size() + 3) & ~std::size_t{3});
    const std::uint32_t shb_len = static_cast<std::uint32_t>(28 + copt + 4);
    put32(c, 0x0A0D0D0A); put32(c, shb_len);
    put32(c, 0x1A2B3C4D); put16(c, 1); put16(c, 0); put64s(c, -1);
    put16(c, 1); put16(c, static_cast<std::uint16_t>(comment.size()));
    for (char ch : comment) put8(c, static_cast<std::uint8_t>(ch));
    for (std::size_t i = comment.size(); i < ((comment.size() + 3) & ~std::size_t{3}); ++i) put8(c, 0);
    put32(c, 0); put32(c, shb_len);

    // Interface Description Block: link type Ethernet, if_tsresol = 9 (ns).
    put32(c, 0x00000001); put32(c, 20 + 8 + 4);
    put16(c, static_cast<std::uint16_t>(kLinktypeEthernet)); put16(c, 0); put32(c, 65535);
    put16(c, 9); put16(c, 1); put8(c, 9); put8(c, 0); put8(c, 0); put8(c, 0);
    put32(c, 0);
    put32(c, 20 + 8 + 4);

    // A block type we do not decode, to prove it is skipped rather than misread.
    put32(c, 0x00000004); put32(c, 16); put32(c, 0); put32(c, 16);

    for (const auto& f : frames) {
        const std::size_t padded = (f.size() + 3) & ~std::size_t{3};
        const auto len = static_cast<std::uint32_t>(32 + padded);
        put32(c, 0x00000006); put32(c, len);
        put32(c, 0);                                          // interface id
        const std::uint64_t ticks = 1700000000ull * 1000000000ull + 500;
        put32(c, static_cast<std::uint32_t>(ticks >> 32));
        put32(c, static_cast<std::uint32_t>(ticks & 0xFFFFFFFF));
        put32(c, static_cast<std::uint32_t>(f.size()));
        put32(c, static_cast<std::uint32_t>(f.size()));
        c.insert(c.end(), f.begin(), f.end());
        for (std::size_t i = f.size(); i < padded; ++i) put8(c, 0);
        put32(c, len);
    }
    return c;
}

// Run a synthetic capture through the full stack via a temp file.
template <class Handler>
TransportStats run(const std::vector<std::byte>& capture, Handler& h, std::string& error) {
    std::FILE* f = std::tmpfile();
    std::fwrite(capture.data(), 1, capture.size(), f);
    std::rewind(f);
    StreamBuffer in(f, 4096);          // small buffer, so refill boundaries are exercised
    TransportStats st = stream_deep_pcap(in, h, error);
    std::fclose(f);
    return st;
}

struct Counter : DeepHandlerBase {
    std::uint64_t plus = 0, trades = 0, segments = 0;
    std::vector<Price8> prices;
    void on_segment(const SegmentHeader&) { ++segments; }
    void on_price_level_update(const PriceLevelUpdate& m) { ++plus; prices.push_back(m.price()); }
    void on_trade_report(const TradeReport&) { ++trades; }
};

}  // namespace

// ---------------------------------------------------------------------------
// IEX-TP segment header
// ---------------------------------------------------------------------------
NB_TEST(iex_transport, segment_header_is_forty_bytes_and_parses) {
    const auto seg = segment(kProtocolDeep1_0, 0xABCD1234, 987654321,
                             {plu(Side::Buy, 0x01, 111, "AAPL", 100, 1000000)});
    CHECK(seg.size() >= kSegmentHeaderLen);
    CHECK_EQ(kSegmentHeaderLen, 40u);

    const SegmentHeader h = SegmentHeader::parse(seg.data());
    CHECK_EQ(h.version, 1u);
    CHECK_EQ(h.protocol_id, kProtocolDeep1_0);
    CHECK_EQ(h.channel_id, 1u);
    CHECK_EQ(h.session_id, 0xABCD1234u);
    CHECK_EQ(h.message_count, 1u);
    CHECK_EQ(h.first_sequence, 987654321);
    CHECK_EQ(h.payload_length, 32u);   // 2-byte length + 30-byte PLU
}

// ---------------------------------------------------------------------------
// Both container formats
// ---------------------------------------------------------------------------
NB_TEST(iex_transport, decodes_classic_pcap) {
    const auto seg = segment(kProtocolDeep1_0, 1, 1,
                             {plu(Side::Buy, 0x01, 111, "AAPL", 100, 1000000),
                              trade(0, 112, "AAPL", 50, 1000000, 7)});
    Counter c;
    std::string err;
    const auto st = run(classic_pcap({udp_frame(seg)}, /*nanos=*/false), c, err);
    CHECK_EQ(err, std::string(""));
    CHECK_EQ(st.capture.packets, 1u);
    CHECK_EQ(st.segments, 1u);
    CHECK_EQ(st.messages, 2u);
    CHECK_EQ(c.plus, 1u);
    CHECK_EQ(c.trades, 1u);
    CHECK_EQ(st.defects(), 0u);
}

NB_TEST(iex_transport, decodes_pcapng_which_is_what_iex_actually_ships) {
    // Sequence numbers must advance across segments, or the second is correctly
    // flagged as a replay (see duplicate_sequence_is_flagged below).
    const auto s1 = segment(kProtocolDeep1_0, 1, 1,
                            {plu(Side::Buy, 0x01, 111, "AAPL", 100, 1000000)});
    const auto s2 = segment(kProtocolDeep1_0, 1, 2,
                            {plu(Side::Buy, 0x01, 112, "AAPL", 200, 1000000)});
    Counter c;
    std::string err;
    const auto st = run(pcapng({udp_frame(s1), udp_frame(s2)}), c, err);
    CHECK_EQ(err, std::string(""));
    CHECK_EQ(st.capture.packets, 2u);
    CHECK_EQ(st.capture.blocks_skipped, 1u);   // the undecoded block was skipped
    CHECK_EQ(c.plus, 2u);
    CHECK_EQ(st.segments, 2u);
    CHECK_EQ(st.sequence_gaps, 0u);
    CHECK_EQ(st.defects(), 0u);
}

NB_TEST(iex_transport, duplicate_sequence_is_flagged_as_a_replay) {
    // IEX HIST is a merge of the A and B multicast feeds, so a replayed sequence
    // is a real possibility rather than a hypothetical. It must be visible:
    // applying the same price level update twice is harmless for an absolute
    // update but not for anything that accumulates, like the tape.
    const auto seg = segment(kProtocolDeep1_0, 1, 1,
                             {plu(Side::Buy, 0x01, 111, "AAPL", 100, 1000000)});
    Counter c;
    std::string err;
    const auto st = run(pcapng({udp_frame(seg), udp_frame(seg)}), c, err);
    CHECK_EQ(st.sequence_regressions, 1u);
    CHECK_EQ(st.sequence_gaps, 0u);
    CHECK(st.defects() > 0);
}

NB_TEST(iex_transport, handles_ip_options_and_vlan_tags) {
    // Assuming a 20-byte IPv4 header or a bare Ethernet frame shifts every
    // subsequent field, which decodes as plausible garbage rather than failing.
    const auto seg = segment(kProtocolDeep1_0, 1, 1,
                             {plu(Side::Sell, 0x01, 111, "MSFT", 300, 4000000)});
    for (std::size_t opts : {std::size_t{0}, std::size_t{4}, std::size_t{8}}) {
        for (int vlans : {0, 1, 2}) {
            Counter c;
            std::string err;
            const auto st = run(pcapng({udp_frame(seg, opts, vlans)}), c, err);
            CHECK_EQ(c.plus, 1u);
            CHECK_EQ(st.defects(), 0u);
            if (c.prices.size() == 1) CHECK_EQ(c.prices[0], 4000000);
        }
    }
}

NB_TEST(iex_transport, messages_spanning_buffer_refills_are_contiguous) {
    // The StreamBuffer above is 4 KiB; this capture is far larger, so segments
    // straddle refill boundaries. A reader that did not compact would corrupt them.
    std::vector<std::vector<std::byte>> frames;
    for (int i = 0; i < 400; ++i) {
        frames.push_back(udp_frame(segment(kProtocolDeep1_0, 1, 1 + i,
            {plu(Side::Buy, 0x01, 1000 + i, "AAPL", static_cast<Shares>(100 + i), 1000000)})));
    }
    Counter c;
    std::string err;
    const auto st = run(pcapng(frames), c, err);
    CHECK_EQ(st.capture.packets, 400u);
    CHECK_EQ(c.plus, 400u);
    CHECK_EQ(st.defects(), 0u);
}

NB_TEST(iex_transport, wrong_protocol_is_rejected_not_misparsed) {
    // A TOPS file handed to the DEEP decoder must be refused, not decoded into
    // convincing nonsense.
    const auto seg = segment(kProtocolTops1_5, 1, 1,
                             {plu(Side::Buy, 0x01, 111, "AAPL", 100, 1000000)});
    Counter c;
    std::string err;
    const auto st = run(pcapng({udp_frame(seg)}), c, err);
    CHECK_EQ(st.wrong_protocol, 1u);
    CHECK_EQ(st.segments, 0u);
    CHECK_EQ(c.plus, 0u);
}

NB_TEST(iex_transport, sequence_gaps_are_detected) {
    // Sequences 1..1, then a jump to 5: three messages are missing. A book
    // rebuilt across a gap is wrong with no other symptom.
    std::vector<std::vector<std::byte>> frames;
    frames.push_back(udp_frame(segment(kProtocolDeep1_0, 1, 1,
        {plu(Side::Buy, 0x01, 1, "AAPL", 100, 1000000)})));
    frames.push_back(udp_frame(segment(kProtocolDeep1_0, 1, 5,
        {plu(Side::Buy, 0x01, 2, "AAPL", 200, 1000000)})));
    Counter c;
    std::string err;
    const auto st = run(pcapng(frames), c, err);
    CHECK_EQ(st.sequence_gaps, 1u);
    CHECK_EQ(st.sequence_gap_messages, 3u);
    CHECK(st.defects() > 0);           // so a research run refuses to trust this
}

NB_TEST(iex_transport, session_change_resets_sequence_expectations) {
    std::vector<std::vector<std::byte>> frames;
    frames.push_back(udp_frame(segment(kProtocolDeep1_0, 1, 100,
        {plu(Side::Buy, 0x01, 1, "AAPL", 100, 1000000)})));
    frames.push_back(udp_frame(segment(kProtocolDeep1_0, 2, 1,
        {plu(Side::Buy, 0x01, 2, "AAPL", 200, 1000000)})));
    Counter c;
    std::string err;
    const auto st = run(pcapng(frames), c, err);
    CHECK_EQ(st.session_changes, 1u);
    CHECK_EQ(st.sequence_gaps, 0u);        // not a gap: numbering restarts
    CHECK_EQ(st.sequence_regressions, 0u);
}

NB_TEST(iex_transport, pcapng_is_not_mistaken_for_classic_pcap) {
    Counter c;
    std::string err;
    // Truncated pcap-ng: must fail with a clear message, not decode rubbish.
    std::vector<std::byte> bad;
    put32(bad, 0x0A0D0D0A);
    put32(bad, 1000);
    const auto st = run(bad, c, err);
    CHECK(!err.empty());
    CHECK_EQ(st.segments, 0u);
}

NB_TEST(iex_transport, unrecognised_magic_is_reported) {
    Counter c;
    std::string err;
    std::vector<std::byte> bad;
    put32(bad, 0xDEADBEEF);
    put32(bad, 0);
    run(bad, c, err);
    CHECK(err.find("magic") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DEEP book: the atomicity rule. This is the test that matters most in the file.
// ---------------------------------------------------------------------------
NB_TEST(iex_book, bbo_does_not_move_until_the_transaction_completes) {
    // Book: bid 100.00, asks at 100.01 / 100.02 / 100.03.
    // A sweep clears 100.01 and 100.02 atomically: two PLUs with the flag OFF and
    // a third with it ON. Throughout the run the spec says the book retains its
    // prior BBO, and an intermediate BBO "never truly existed".
    DeepBook b;
    auto apply = [&](Side s, std::uint8_t flags, Shares sz, Price8 px) {
        const auto m = plu(s, flags, 1000, "AAPL", sz, px);
        return b.apply(PriceLevelUpdate(m.data()));
    };

    apply(Side::Buy,  0x01, 100, 1000000);
    apply(Side::Sell, 0x01, 100, 1000100);
    apply(Side::Sell, 0x01, 200, 1000200);
    apply(Side::Sell, 0x01, 300, 1000300);
    CHECK_EQ(b.stable_top().ask, 1000100u);
    CHECK_EQ(b.stable_top().ask_shares, 100);

    // --- the sweep begins ---
    CHECK(!apply(Side::Sell, 0x00, 0, 1000100));   // flag OFF: in transition
    // The ladder has already changed, but the published BBO must NOT have.
    CHECK_EQ(b.stable_top().ask, 1000100u);
    CHECK_EQ(b.stable_top().ask_shares, 100);

    CHECK(!apply(Side::Sell, 0x00, 0, 1000200));   // still in transition
    CHECK_EQ(b.stable_top().ask, 1000100u);        // still the pre-sweep BBO

    CHECK(apply(Side::Sell, 0x01, 300, 1000300));  // flag ON: transaction complete
    // Only now does the BBO advance — straight to 100.03, never showing 100.02.
    CHECK_EQ(b.stable_top().ask, 1000300u);
    CHECK_EQ(b.stable_top().ask_shares, 300);

    CHECK_EQ(b.transactions(), 5u);
    CHECK_EQ(b.in_transition_updates(), 2u);
}

NB_TEST(iex_book, plu_sizes_are_absolute_not_deltas) {
    DeepBook b;
    auto apply = [&](Side s, Shares sz, Price8 px) {
        const auto m = plu(s, 0x01, 1000, "AAPL", sz, px);
        b.apply(PriceLevelUpdate(m.data()));
    };
    apply(Side::Buy, 500, 1000000);
    CHECK_EQ(b.stable_top().bid_shares, 500);
    apply(Side::Buy, 200, 1000000);            // absolute: now 200, not 700
    CHECK_EQ(b.stable_top().bid_shares, 200);
    apply(Side::Buy, 0, 1000000);              // zero removes the level
    CHECK(!b.stable_top().bid_valid);
}

NB_TEST(iex_book, lee_ready_classifies_against_the_pre_trade_mid) {
    DeepBook b;
    auto quote = [&](Side s, Shares sz, Price8 px) {
        const auto m = plu(s, 0x01, 1000, "AAPL", sz, px);
        b.apply(PriceLevelUpdate(m.data()));
    };
    auto print = [&](Shares sz, Price8 px) {
        const auto m = trade(0, 1001, "AAPL", sz, px, 1);
        b.apply(TradeReport(m.data()));
    };

    quote(Side::Buy, 100, 1000000);     // 100.00
    quote(Side::Sell, 100, 1000200);    // 100.02  => mid 100.01

    print(300, 1000200);                // at the ask: buy-initiated
    print(400, 1000000);                // at the bid: sell-initiated
    print(500, 1000100);                // exactly at the mid: unclassified
    CHECK_EQ(b.tape().buy_shares, 300u);
    CHECK_EQ(b.tape().sell_shares, 400u);
    CHECK_EQ(b.tape().unclassified_shares, 500u);
    CHECK_EQ(b.tape().trades, 3u);
    CHECK_EQ(b.tape().shares, 1200u);
}

NB_TEST(iex_book, trade_with_no_two_sided_quote_is_unclassified_not_guessed) {
    DeepBook b;
    const auto m = trade(0, 1001, "AAPL", 100, 1000000, 1);
    b.apply(TradeReport(m.data()));
    CHECK_EQ(b.tape().unclassified_shares, 100u);
    CHECK_EQ(b.tape().buy_shares, 0u);
    CHECK_EQ(b.tape().sell_shares, 0u);
}

NB_TEST(iex_book, rejects_non_representable_prices_and_counts_them) {
    DeepBook b;
    const auto m = plu(Side::Buy, 0x01, 1000, "AAPL", 100, -5);   // negative price
    b.apply(PriceLevelUpdate(m.data()));
    CHECK_EQ(b.bad_prices(), 1u);
    CHECK(!b.stable_top().bid_valid);
    // The transaction boundary is still honoured, or every later sample desyncs.
    CHECK_EQ(b.transactions(), 1u);
}

NB_TEST(iex_book, router_filters_to_the_watchlist) {
    struct Sampler {
        std::uint64_t n = 0;
        std::vector<std::uint32_t> seen;
        void on_sample(std::uint32_t i, DeepBook&, std::int64_t) { ++n; seen.push_back(i); }
    } s;

    const std::vector<std::string> want = {"AAPL", "MSFT"};
    DeepBookRouter<Sampler> r(want, s, /*sample_ns=*/0);

    for (const char* sym : {"AAPL", "MSFT", "NVDA", "ZZZZ", "AAPL"}) {
        const auto m = plu(Side::Buy, 0x01, 1000, sym, 100, 1000000);
        r.on_price_level_update(PriceLevelUpdate(m.data()));
    }
    CHECK_EQ(r.matched_messages(), 3u);   // AAPL, MSFT, AAPL only
    CHECK_EQ(s.n, 3u);
    CHECK_EQ(r.book(0).stable_top().bid, 1000000u);
    CHECK_EQ(r.book(1).stable_top().bid, 1000000u);
}

NB_TEST(iex_book, trading_status_is_tracked_per_symbol) {
    struct Sampler { void on_sample(std::uint32_t, DeepBook&, std::int64_t) {} } s;
    const std::vector<std::string> want = {"AAPL"};
    DeepBookRouter<Sampler> r(want, s, 0);
    const auto m = trading_status(1000, "AAPL", 'H');
    r.on_trading_status(TradingStatus(m.data()));
    CHECK_EQ(r.book(0).trading_status(), 'H');
}
