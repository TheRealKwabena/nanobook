// Parser and wire format.
//
// The round-trip tests encode with itch_writer.hpp and decode with
// itch_spec.hpp. Those are two independent transcriptions of the NASDAQ spec, so
// a wrong field offset in either one fails here rather than silently shifting
// every price in a production run by four bytes.
#include <vector>

#include "framework.hpp"
#include "nanobook/itch_parser.hpp"
#include "nanobook/itch_writer.hpp"

using namespace nanobook;
using itch::Side;

namespace {

// Collects every decoded message so a test can assert on the fields.
struct Recorder : HandlerBase {
    std::vector<char> types;
    std::vector<std::uint64_t> timestamps;
    std::vector<itch::Locate> locates;

    // captured fields, by message
    itch::OrderRef add_ref = 0, exec_ref = 0, cancel_ref = 0, del_ref = 0;
    itch::OrderRef repl_old = 0, repl_new = 0;
    Side add_side = Side::Buy;
    itch::Shares add_shares = 0, exec_shares = 0, cancel_shares = 0, repl_shares = 0;
    itch::Price4 add_price = 0, exec_price = 0, repl_price = 0, trade_price = 0;
    std::string add_stock, dir_stock, mpid, halt_reason;
    std::uint64_t match = 0;
    bool printable = false;
    char event_code = 0, trading_state = 0, cross_type = 0;
    std::uint32_t round_lot = 0;
    std::uint64_t cross_shares = 0, noii_paired = 0, noii_imbalance = 0;

    void on_message(const itch::MsgView& m) {
        types.push_back(m.type());
        timestamps.push_back(m.timestamp());
        locates.push_back(m.locate());
    }
    void on_system_event(const itch::SystemEvent& m) { event_code = m.event_code(); }
    void on_stock_directory(const itch::StockDirectory& m) {
        dir_stock = std::string(m.stock());
        round_lot = m.round_lot_size();
    }
    void on_trading_action(const itch::StockTradingAction& m) {
        trading_state = m.trading_state();
        halt_reason = std::string(m.reason());
    }
    void on_add_order(const itch::AddOrder& m) {
        add_ref = m.order_ref(); add_side = m.side(); add_shares = m.shares();
        add_stock = std::string(m.stock()); add_price = m.price();
    }
    void on_add_order_mpid(const itch::AddOrderMpid& m) {
        add_ref = m.order_ref(); add_side = m.side(); add_shares = m.shares();
        add_stock = std::string(m.stock()); add_price = m.price();
        mpid = std::string(m.attribution());
    }
    void on_order_executed(const itch::OrderExecuted& m) {
        exec_ref = m.order_ref(); exec_shares = m.executed_shares(); match = m.match_number();
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        exec_ref = m.order_ref(); exec_shares = m.executed_shares(); match = m.match_number();
        printable = m.printable(); exec_price = m.execution_price();
    }
    void on_order_cancel(const itch::OrderCancel& m) {
        cancel_ref = m.order_ref(); cancel_shares = m.cancelled_shares();
    }
    void on_order_delete(const itch::OrderDelete& m) { del_ref = m.order_ref(); }
    void on_order_replace(const itch::OrderReplace& m) {
        repl_old = m.original_order_ref(); repl_new = m.new_order_ref();
        repl_shares = m.shares(); repl_price = m.price();
    }
    void on_trade(const itch::TradeNonCross& m) {
        trade_price = m.price(); match = m.match_number();
    }
    void on_cross_trade(const itch::CrossTrade& m) {
        cross_shares = m.shares(); cross_type = m.cross_type();
    }
    void on_noii(const itch::Noii& m) {
        noii_paired = m.paired_shares(); noii_imbalance = m.imbalance_shares();
    }
};

}  // namespace

NB_TEST(itch_parser, every_spec_length_is_consistent) {
    // The header is 11 bytes, so no message can be shorter, and the longest
    // defined message is 'I' at 50.
    for (char t : {'S','R','H','Y','L','V','W','K','J','h','A','F','E','C','X','D','U','P','Q','B','I','N'}) {
        const std::size_t n = itch::message_length(t);
        CHECK_MSG(n >= itch::kHeaderLen, std::string("message '") + t + "' shorter than header");
        CHECK_MSG(n <= 50, std::string("message '") + t + "' longer than 50 bytes");
    }
    CHECK_EQ(itch::message_length('z'), 0u);  // undefined type
    CHECK_EQ(itch::message_length('A'), 36u);
    CHECK_EQ(itch::message_length('U'), 35u);
}

NB_TEST(itch_parser, add_order_round_trips) {
    itch::MsgBuf m;
    const std::size_t n = itch::write_add_order(m, 1234, 34200000000001ull, 0xDEADBEEFCAFEull,
                                                Side::Sell, 1500, "AAPL", 1904200);
    CHECK_EQ(n, 36u);
    std::vector<std::byte> feed;
    itch::append_framed(feed, m, n);

    Recorder r;
    const ParseStats st = parse(feed.data(), feed.size(), r);
    CHECK_EQ(st.messages, 1u);
    CHECK_EQ(st.framing_errors, 0u);
    CHECK_EQ(r.locates[0], 1234u);
    CHECK_EQ(r.timestamps[0], 34200000000001ull);
    CHECK_EQ(r.add_ref, 0xDEADBEEFCAFEull);
    CHECK(r.add_side == Side::Sell);
    CHECK_EQ(r.add_shares, 1500u);
    CHECK_EQ(r.add_stock, std::string("AAPL"));   // trailing spaces trimmed
    CHECK_EQ(r.add_price, 1904200u);              // $190.42
}

NB_TEST(itch_parser, all_decoded_messages_round_trip) {
    std::vector<std::byte> feed;
    itch::MsgBuf m;
    itch::append_framed(feed, m, itch::write_system_event(m, 100, 'Q'));
    itch::append_framed(feed, m, itch::write_stock_directory(m, 7, 200, "MSFT", 100));
    itch::append_framed(feed, m, itch::write_trading_action(m, 7, 300, "MSFT", 'H', "LUDP"));
    itch::append_framed(feed, m, itch::write_add_order_mpid(m, 7, 400, 11, Side::Buy, 700, "MSFT", 4001000, "NSDQ"));
    itch::append_framed(feed, m, itch::write_order_executed(m, 7, 500, 11, 300, 9999));
    itch::append_framed(feed, m, itch::write_order_cancel(m, 7, 600, 11, 100));
    itch::append_framed(feed, m, itch::write_order_replace(m, 7, 700, 11, 12, 900, 4002000));
    itch::append_framed(feed, m, itch::write_order_delete(m, 7, 800, 12));
    itch::append_framed(feed, m, itch::write_trade(m, 7, 900, Side::Sell, 250, "MSFT", 4001500, 8888));
    itch::append_framed(feed, m, itch::write_cross_trade(m, 7, 1000, 1234567, "MSFT", 4005000, 7777, 'O'));
    itch::append_framed(feed, m, itch::write_noii(m, 7, 1100, 555000, 66000, 'B', "MSFT", 4000000, 4001000, 4000500, 'O', ' '));
    itch::append_framed(feed, m, itch::write_order_executed_with_price(m, 7, 1200, 12, 50, 6666, true, 3999000));
    itch::append_framed(feed, m, itch::write_broken_trade(m, 7, 1300, 8888));

    Recorder r;
    const ParseStats st = parse(feed.data(), feed.size(), r);

    CHECK_EQ(st.messages, 13u);
    CHECK_EQ(st.framing_errors, 0u);
    CHECK_EQ(st.unknown_type, 0u);
    CHECK_EQ(st.length_mismatch, 0u);
    CHECK_EQ(st.truncated_tail, 0u);

    CHECK_EQ(r.event_code, 'Q');
    CHECK_EQ(r.dir_stock, std::string("MSFT"));
    CHECK_EQ(r.round_lot, 100u);
    CHECK_EQ(r.trading_state, 'H');
    CHECK_EQ(r.halt_reason, std::string("LUDP"));
    CHECK_EQ(r.mpid, std::string("NSDQ"));
    CHECK_EQ(r.add_shares, 700u);
    CHECK_EQ(r.add_price, 4001000u);
    CHECK_EQ(r.cancel_shares, 100u);
    CHECK_EQ(r.repl_old, 11u);
    CHECK_EQ(r.repl_new, 12u);
    CHECK_EQ(r.repl_shares, 900u);
    CHECK_EQ(r.repl_price, 4002000u);
    CHECK_EQ(r.del_ref, 12u);
    CHECK_EQ(r.trade_price, 4001500u);
    CHECK_EQ(r.cross_shares, 1234567u);
    CHECK_EQ(r.cross_type, 'O');
    CHECK_EQ(r.noii_paired, 555000u);
    CHECK_EQ(r.noii_imbalance, 66000u);
    CHECK(r.printable);
    CHECK_EQ(r.exec_price, 3999000u);

    // Every framed message must have been visited exactly once, in order.
    const std::vector<char> want = {'S','R','H','F','E','X','U','D','P','Q','I','C','B'};
    CHECK(r.types == want);
}

NB_TEST(itch_parser, per_type_counters_are_right) {
    std::vector<std::byte> feed;
    itch::MsgBuf m;
    for (int i = 0; i < 5; ++i)
        itch::append_framed(feed, m, itch::write_add_order(m, 1, 10, static_cast<itch::OrderRef>(i + 1), Side::Buy, 100, "X", 1000000));
    for (int i = 0; i < 3; ++i)
        itch::append_framed(feed, m, itch::write_order_delete(m, 1, 20, static_cast<itch::OrderRef>(i + 1)));

    Recorder r;
    const ParseStats st = parse(feed.data(), feed.size(), r);
    CHECK_EQ(st.messages, 8u);
    CHECK_EQ(st.count('A'), 5u);
    CHECK_EQ(st.count('D'), 3u);
    CHECK_EQ(st.count('E'), 0u);
}

NB_TEST(itch_parser, raw_framing_uses_the_spec_length_table) {
    // MoldUDP64 payloads carry no length prefix, so framing depends entirely on
    // the spec table being complete and correct.
    std::vector<std::byte> feed;
    itch::MsgBuf m;
    std::size_t n = itch::write_add_order(m, 3, 11, 1, Side::Buy, 100, "ZZ", 500000);
    feed.insert(feed.end(), m.b, m.b + n);
    n = itch::write_order_delete(m, 3, 12, 1);
    feed.insert(feed.end(), m.b, m.b + n);
    n = itch::write_order_executed(m, 3, 13, 2, 50, 1);
    feed.insert(feed.end(), m.b, m.b + n);

    Recorder r;
    const ParseStats st = parse(feed.data(), feed.size(), r, Framing::Raw);
    CHECK_EQ(st.messages, 3u);
    CHECK_EQ(st.framing_errors, 0u);
    const std::vector<char> want = {'A', 'D', 'E'};
    CHECK(r.types == want);
}

NB_TEST(itch_parser, truncated_tail_is_reported_not_read_past) {
    std::vector<std::byte> feed;
    itch::MsgBuf m;
    itch::append_framed(feed, m, itch::write_add_order(m, 1, 10, 1, Side::Buy, 100, "X", 1000000));
    itch::append_framed(feed, m, itch::write_add_order(m, 1, 11, 2, Side::Buy, 100, "X", 1000000));
    feed.resize(feed.size() - 9);  // chop the last message mid-body

    Recorder r;
    const ParseStats st = parse(feed.data(), feed.size(), r);
    CHECK_EQ(st.messages, 1u);          // only the intact one
    CHECK(st.truncated_tail > 0);
}

NB_TEST(itch_parser, zero_length_frame_stops_cleanly) {
    // A zero length means the stream has desynced. Guessing a length from that
    // point would emit garbage messages for the rest of the file.
    std::vector<std::byte> feed;
    itch::MsgBuf m;
    itch::append_framed(feed, m, itch::write_add_order(m, 1, 10, 1, Side::Buy, 100, "X", 1000000));
    feed.push_back(std::byte{0});
    feed.push_back(std::byte{0});
    itch::append_framed(feed, m, itch::write_add_order(m, 1, 11, 2, Side::Buy, 100, "X", 1000000));

    Recorder r;
    const ParseStats st = parse(feed.data(), feed.size(), r);
    CHECK_EQ(st.messages, 1u);
    CHECK_EQ(st.framing_errors, 1u);
}

NB_TEST(itch_parser, unknown_type_is_counted_and_skipped_by_frame_length) {
    // BinaryFILE framing lets us step over a message type we do not know,
    // because the length came from the frame rather than from the table.
    std::vector<std::byte> feed;
    itch::MsgBuf m;
    itch::append_framed(feed, m, itch::write_add_order(m, 1, 10, 1, Side::Buy, 100, "X", 1000000));
    itch::put_header(m.b, 'z', 1, 11);           // fabricated unknown type
    itch::append_framed(feed, m, 20);
    itch::append_framed(feed, m, itch::write_order_delete(m, 1, 12, 1));

    Recorder r;
    const ParseStats st = parse(feed.data(), feed.size(), r);
    CHECK_EQ(st.messages, 3u);          // stayed in sync across the unknown one
    CHECK_EQ(st.unknown_type, 1u);
    CHECK_EQ(st.count('D'), 1u);
}

NB_TEST(itch_parser, empty_input_is_not_an_error) {
    Recorder r;
    const ParseStats st = parse(nullptr, 0, r);
    CHECK_EQ(st.messages, 0u);
    CHECK_EQ(st.framing_errors, 0u);
}

NB_TEST(itch_parser, ascii_fields_trim_padding_but_keep_inner_spaces) {
    itch::MsgBuf m;
    const std::size_t n = itch::write_add_order(m, 1, 10, 1, Side::Buy, 100, "BRK B", 1000000);
    std::vector<std::byte> feed;
    itch::append_framed(feed, m, n);
    Recorder r;
    parse(feed.data(), feed.size(), r);
    CHECK_EQ(r.add_stock, std::string("BRK B"));
}
