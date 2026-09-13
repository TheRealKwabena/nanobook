// IEX DEEP wire format.
//
// The decisive tests here decode the worked examples printed in the IEX DEEP
// Specification v1.08 itself, byte for byte. That is stronger than a round-trip
// against our own encoder: these bytes came from the exchange's own document, so
// agreeing with them means agreeing with IEX, not merely with ourselves.
#include <vector>

#include "framework.hpp"
#include "nanobook/iex_spec.hpp"

using namespace nanobook;
using namespace nanobook::iex;

namespace {

// Build a message from a spec hex dump, so the test reads like the document.
std::vector<std::byte> hex(std::string_view s) {
    std::vector<std::byte> out;
    unsigned cur = 0;
    int nibbles = 0;
    for (char c : s) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        cur = (cur << 4) | static_cast<unsigned>(v);
        if (++nibbles == 2) {
            out.push_back(static_cast<std::byte>(cur));
            cur = 0;
            nibbles = 0;
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Price Level Update — DEEP Specification v1.08, page 18 example.
//
//   Message Type   38                       // 8 = PLU on the Buy Side
//   Event Flags    01                       // Event processing complete
//   Timestamp      ac 63 c0 20 96 86 6d 14  // 2016-08-23 15:30:32.572715948
//   Symbol         5a 49 45 58 54 20 20 20  // ZIEXT
//   Size           e4 25 00 00              // 9,700 shares
//   Price          24 1d 0f 00 00 00 00 00  // $99.05
// ---------------------------------------------------------------------------
NB_TEST(iex_spec, price_level_update_matches_spec_example) {
    const auto m = hex("38 01 ac 63 c0 20 96 86 6d 14 "
                       "5a 49 45 58 54 20 20 20 "
                       "e4 25 00 00 "
                       "24 1d 0f 00 00 00 00 00");
    CHECK_EQ(m.size(), 30u);                       // spec: "Total Message Data length is 30 bytes"
    CHECK_EQ(message_length(0x38), 30u);

    PriceLevelUpdate plu(m.data());
    CHECK_EQ(plu.type(), 0x38u);
    CHECK(plu.side() == Side::Buy);
    CHECK(plu.event_complete());                   // flags 0x01 => BBO is meaningful
    CHECK_EQ(plu.size(), 9700u);
    CHECK_EQ(plu.price(), 990500);                 // $99.0500 at 4 implied decimals

    // The spec annotates its examples in Eastern wall-clock time, but the field
    // is nanoseconds since the POSIX epoch UTC. 19:30:32 UTC == 15:30:32 EDT.
    CHECK_EQ(plu.timestamp(), 1471980632572715948LL);

    char buf[9];
    CHECK_EQ(plu.symbol().str(buf), std::string("ZIEXT"));   // padding trimmed
}

NB_TEST(iex_spec, sell_side_plu_is_the_same_layout) {
    auto m = hex("35 00 ac 63 c0 20 96 86 6d 14 "
                 "5a 49 45 58 54 20 20 20 e4 25 00 00 24 1d 0f 00 00 00 00 00");
    PriceLevelUpdate plu(m.data());
    CHECK_EQ(plu.type(), 0x35u);
    CHECK(plu.side() == Side::Sell);
    CHECK(!plu.event_complete());                  // flags 0x00 => book in transition
    CHECK_EQ(plu.size(), 9700u);
    CHECK_EQ(message_length(0x35), 30u);
}

// ---------------------------------------------------------------------------
// Trade Report — DEEP Specification v1.08, page 21 example.
// ---------------------------------------------------------------------------
NB_TEST(iex_spec, trade_report_matches_spec_example) {
    const auto m = hex("54 00 c3 df f7 05 a2 86 6d 14 "
                       "5a 49 45 58 54 20 20 20 "
                       "64 00 00 00 "
                       "24 1d 0f 00 00 00 00 00 "
                       "96 8f 06 00 00 00 00 00");
    CHECK_EQ(m.size(), 38u);                       // spec: 38 bytes
    CHECK_EQ(message_length(0x54), 38u);

    TradeReport t(m.data());
    CHECK_EQ(t.type(), 0x54u);
    CHECK_EQ(t.size(), 100u);
    CHECK_EQ(t.price(), 990500);
    CHECK_EQ(t.trade_id(), 429974);
    CHECK_EQ(t.timestamp(), 1471980683662974915LL);

    // Sale condition 0x00: non-ISO, regular session, subject to Rule 611,
    // continuous trading — i.e. an ordinary printable trade.
    CHECK_EQ(t.sale_condition_flags(), 0x00u);
    CHECK(!t.intermarket_sweep());
    CHECK(!t.extended_hours());
    CHECK(!t.odd_lot());
    CHECK(!t.single_price_cross());

    char buf[9];
    CHECK_EQ(t.symbol().str(buf), std::string("ZIEXT"));
}

// ---------------------------------------------------------------------------
// Every length in the table must agree with the spec's stated totals.
// ---------------------------------------------------------------------------
NB_TEST(iex_spec, message_lengths_match_the_specification) {
    CHECK_EQ(message_length(0x53), 10u);  // System Event
    CHECK_EQ(message_length(0x44), 31u);  // Security Directory
    CHECK_EQ(message_length(0x48), 22u);  // Trading Status
    CHECK_EQ(message_length(0x49), 18u);  // Retail Liquidity Indicator
    CHECK_EQ(message_length(0x4f), 18u);  // Operational Halt Status
    CHECK_EQ(message_length(0x50), 19u);  // Short Sale Price Test Status
    CHECK_EQ(message_length(0x45), 18u);  // Security Event
    CHECK_EQ(message_length(0x38), 30u);  // Price Level Update, buy
    CHECK_EQ(message_length(0x35), 30u);  // Price Level Update, sell
    CHECK_EQ(message_length(0x54), 38u);  // Trade Report
    CHECK_EQ(message_length(0x58), 26u);  // Official Price
    CHECK_EQ(message_length(0x42), 38u);  // Trade Break
    CHECK_EQ(message_length(0x41), 80u);  // Auction Information
    CHECK_EQ(message_length(0x99), 0u);   // unknown
}

// ---------------------------------------------------------------------------
// Symbol matching. DEEP has no locate, so filtering is an 8-byte word compare.
// ---------------------------------------------------------------------------
NB_TEST(iex_spec, symbol8_compares_as_one_word) {
    const auto m = hex("38 01 00 00 00 00 00 00 00 00 "
                       "41 41 50 4c 20 20 20 20 "          // "AAPL    "
                       "00 00 00 00 00 00 00 00 00 00 00 00");
    PriceLevelUpdate plu(m.data());
    CHECK(plu.symbol() == Symbol8("AAPL"));
    CHECK(plu.symbol() != Symbol8("AAPLX"));
    CHECK(plu.symbol() != Symbol8("AAP"));
    CHECK(plu.symbol() != Symbol8("MSFT"));

    // A prefix must not match: "AAPL" padded differs from "AAPLW" padded.
    CHECK(Symbol8("AAPL") != Symbol8("AAPLW"));
    // Same ticker built two ways must be identical.
    CHECK(Symbol8("ZIEXT") == Symbol8(std::string_view("ZIEXT")));

    char buf[9];
    CHECK_EQ(Symbol8("BRK B").str(buf), std::string("BRK B"));  // inner space kept
}

// ---------------------------------------------------------------------------
// Signed 64-bit price narrowing. DEEP prices are int64; the ladder needs uint32.
// ---------------------------------------------------------------------------
NB_TEST(iex_spec, price_narrowing_rejects_what_it_cannot_represent) {
    Price4 out = 0;

    CHECK(price_to_p4(990500, out));
    CHECK_EQ(out, 990500u);

    // Zero is DEEP's "no price" for several fields, not a real price of $0.
    CHECK(!price_to_p4(0, out));
    // Negative cannot be a quote; it means a decode has gone wrong.
    CHECK(!price_to_p4(-1, out));
    CHECK(!price_to_p4(-990500, out));
    // Above 2^31 would collide with the side bit packed into OrderEntry.
    CHECK(!price_to_p4(0x80000000LL, out));
    CHECK(!price_to_p4(1LL << 40, out));
    // Just inside the limit is fine: $214,748.3647
    CHECK(price_to_p4(0x7FFFFFFF, out));
    CHECK_EQ(out, 0x7FFFFFFFu);
}

// ---------------------------------------------------------------------------
// Little-endian, not big-endian. This is the single most likely porting bug when
// the same codebase already speaks a big-endian protocol.
// ---------------------------------------------------------------------------
NB_TEST(iex_spec, deep_is_little_endian_unlike_itch) {
    // Size 9,700 == 0x25e4. On the wire little-endian that is e4 25 00 00.
    // Read big-endian it would be 0xe4250000 == 3,827,236,864.
    const auto m = hex("38 01 00 00 00 00 00 00 00 00 "
                       "5a 49 45 58 54 20 20 20 e4 25 00 00 24 1d 0f 00 00 00 00 00");
    PriceLevelUpdate plu(m.data());
    CHECK_EQ(plu.size(), 9700u);
    CHECK(plu.size() != 3827236864u);
    CHECK_EQ(plu.price(), 990500);
}

NB_TEST(iex_spec, system_and_security_event_codes) {
    const auto m = hex("53 52 ac 63 c0 20 96 86 6d 14");   // 'S', event 'R'
    CHECK_EQ(m.size(), 10u);
    SystemEvent se(m.data());
    CHECK_EQ(se.type(), 0x53u);
    CHECK_EQ(se.event(), kStartOfRegularHours);
    CHECK_EQ(se.timestamp(), 1471980632572715948LL);
}

NB_TEST(iex_spec, trade_sale_condition_flags_decode) {
    // 0xa0 == intermarket sweep (0x80) + odd lot (0x20).
    auto m = hex("54 a0 00 00 00 00 00 00 00 00 "
                 "5a 49 45 58 54 20 20 20 01 00 00 00 "
                 "24 1d 0f 00 00 00 00 00 01 00 00 00 00 00 00 00");
    TradeReport t(m.data());
    CHECK(t.intermarket_sweep());
    CHECK(t.odd_lot());
    CHECK(!t.extended_hours());
    CHECK(!t.single_price_cross());
    CHECK_EQ(t.size(), 1u);
}
