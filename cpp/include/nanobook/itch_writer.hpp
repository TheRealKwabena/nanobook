// nanobook — itch_writer.hpp
//
// Encoders for ITCH 5.0 messages: the inverse of itch_spec.hpp's decoders.
//
// These exist so the generator and the test suite share one transcription of the
// wire format, and so that transcription is *independent of the decoder's*. The
// round-trip test encodes a message here and decodes it with itch_spec.hpp; if
// the two disagree about a field offset, the test fails. A single shared offset
// constant would have made both wrong together and the test vacuous.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "nanobook/byte_order.hpp"
#include "nanobook/itch_spec.hpp"

namespace nanobook::itch {

// Scratch space for one message. The largest ITCH 5.0 message ('I', NOII) is
// 50 bytes; 64 keeps it on one cache line with room for future revisions.
struct MsgBuf {
    std::byte b[64];
};

inline void put_header(std::byte* p, char type, std::uint16_t locate, std::uint64_t ts) {
    p[0] = static_cast<std::byte>(type);
    store_be16(p + 1, locate);
    store_be16(p + 3, 0);  // tracking number: NASDAQ internal, not meaningful
    store_be48(p + 5, ts);
}

// Fixed-width alphanumeric fields are left-justified and space padded.
inline void put_stock(std::byte* p, const std::string& sym) {
    std::memset(p, ' ', 8);
    std::memcpy(p, sym.data(), std::min<std::size_t>(8, sym.size()));
}

inline std::size_t write_system_event(MsgBuf& m, std::uint64_t ts, char code) {
    put_header(m.b, 'S', 0, ts);
    m.b[11] = static_cast<std::byte>(code);
    return 12;
}

inline std::size_t write_stock_directory(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                         const std::string& sym, std::uint32_t round_lot = 100) {
    put_header(m.b, 'R', locate, ts);
    put_stock(m.b + 11, sym);
    m.b[19] = static_cast<std::byte>('Q');  // market category: NASDAQ Global Select
    m.b[20] = static_cast<std::byte>(' ');  // financial status: normal
    store_be32(m.b + 21, round_lot);
    m.b[25] = static_cast<std::byte>('N');  // round lots only
    m.b[26] = static_cast<std::byte>('C');  // issue classification: common stock
    m.b[27] = static_cast<std::byte>(' ');
    m.b[28] = static_cast<std::byte>(' ');
    m.b[29] = static_cast<std::byte>('P');  // authenticity: production
    m.b[30] = static_cast<std::byte>('N');  // short sale threshold
    m.b[31] = static_cast<std::byte>('N');  // IPO flag
    m.b[32] = static_cast<std::byte>('1');  // LULD tier
    m.b[33] = static_cast<std::byte>('N');  // ETP flag
    store_be32(m.b + 34, 0);                // ETP leverage
    m.b[38] = static_cast<std::byte>('N');  // inverse indicator
    return 39;
}

inline std::size_t write_trading_action(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                        const std::string& sym, char state,
                                        const std::string& reason = "") {
    put_header(m.b, 'H', locate, ts);
    put_stock(m.b + 11, sym);
    m.b[19] = static_cast<std::byte>(state);
    m.b[20] = static_cast<std::byte>(' ');  // reserved
    std::memset(m.b + 21, ' ', 4);
    std::memcpy(m.b + 21, reason.data(), std::min<std::size_t>(4, reason.size()));
    return 25;
}

inline std::size_t write_add_order(MsgBuf& m, std::uint16_t locate, std::uint64_t ts, OrderRef ref,
                                   Side side, Shares shares, const std::string& sym, Price4 px) {
    put_header(m.b, 'A', locate, ts);
    store_be64(m.b + 11, ref);
    m.b[19] = static_cast<std::byte>(side_to_char(side));
    store_be32(m.b + 20, shares);
    put_stock(m.b + 24, sym);
    store_be32(m.b + 32, px);
    return 36;
}

inline std::size_t write_add_order_mpid(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                       OrderRef ref, Side side, Shares shares,
                                       const std::string& sym, Price4 px,
                                       const std::string& mpid = "NSDQ") {
    write_add_order(m, locate, ts, ref, side, shares, sym, px);
    m.b[0] = static_cast<std::byte>('F');
    std::memset(m.b + 36, ' ', 4);
    std::memcpy(m.b + 36, mpid.data(), std::min<std::size_t>(4, mpid.size()));
    return 40;
}

inline std::size_t write_order_executed(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                        OrderRef ref, Shares shares, std::uint64_t match) {
    put_header(m.b, 'E', locate, ts);
    store_be64(m.b + 11, ref);
    store_be32(m.b + 19, shares);
    store_be64(m.b + 23, match);
    return 31;
}

inline std::size_t write_order_executed_with_price(MsgBuf& m, std::uint16_t locate,
                                                   std::uint64_t ts, OrderRef ref, Shares shares,
                                                   std::uint64_t match, bool printable, Price4 px) {
    write_order_executed(m, locate, ts, ref, shares, match);
    m.b[0] = static_cast<std::byte>('C');
    m.b[31] = static_cast<std::byte>(printable ? 'Y' : 'N');
    store_be32(m.b + 32, px);
    return 36;
}

inline std::size_t write_order_cancel(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                      OrderRef ref, Shares cancelled) {
    put_header(m.b, 'X', locate, ts);
    store_be64(m.b + 11, ref);
    store_be32(m.b + 19, cancelled);
    return 23;
}

inline std::size_t write_order_delete(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                      OrderRef ref) {
    put_header(m.b, 'D', locate, ts);
    store_be64(m.b + 11, ref);
    return 19;
}

inline std::size_t write_order_replace(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                       OrderRef old_ref, OrderRef new_ref, Shares shares,
                                       Price4 px) {
    put_header(m.b, 'U', locate, ts);
    store_be64(m.b + 11, old_ref);
    store_be64(m.b + 19, new_ref);
    store_be32(m.b + 27, shares);
    store_be32(m.b + 31, px);
    return 35;
}

inline std::size_t write_trade(MsgBuf& m, std::uint16_t locate, std::uint64_t ts, Side side,
                               Shares shares, const std::string& sym, Price4 px,
                               std::uint64_t match, OrderRef ref = 0) {
    put_header(m.b, 'P', locate, ts);
    store_be64(m.b + 11, ref);
    m.b[19] = static_cast<std::byte>(side_to_char(side));
    store_be32(m.b + 20, shares);
    put_stock(m.b + 24, sym);
    store_be32(m.b + 32, px);
    store_be64(m.b + 36, match);
    return 44;
}

inline std::size_t write_cross_trade(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                     std::uint64_t shares, const std::string& sym, Price4 px,
                                     std::uint64_t match, char cross_type) {
    put_header(m.b, 'Q', locate, ts);
    store_be64(m.b + 11, shares);
    put_stock(m.b + 19, sym);
    store_be32(m.b + 27, px);
    store_be64(m.b + 31, match);
    m.b[39] = static_cast<std::byte>(cross_type);
    return 40;
}

inline std::size_t write_broken_trade(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                                      std::uint64_t match) {
    put_header(m.b, 'B', locate, ts);
    store_be64(m.b + 11, match);
    return 19;
}

inline std::size_t write_noii(MsgBuf& m, std::uint16_t locate, std::uint64_t ts,
                              std::uint64_t paired, std::uint64_t imbalance, char direction,
                              const std::string& sym, Price4 far_px, Price4 near_px, Price4 ref_px,
                              char cross_type, char variation) {
    put_header(m.b, 'I', locate, ts);
    store_be64(m.b + 11, paired);
    store_be64(m.b + 19, imbalance);
    m.b[27] = static_cast<std::byte>(direction);
    put_stock(m.b + 28, sym);
    store_be32(m.b + 36, far_px);
    store_be32(m.b + 40, near_px);
    store_be32(m.b + 44, ref_px);
    m.b[48] = static_cast<std::byte>(cross_type);
    m.b[49] = static_cast<std::byte>(variation);
    return 50;
}

// BinaryFILE framing helper: 2-byte big-endian length, then the body.
inline void append_framed(std::vector<std::byte>& out, const MsgBuf& m, std::size_t len) {
    std::byte hdr[2];
    store_be16(hdr, static_cast<std::uint16_t>(len));
    out.insert(out.end(), hdr, hdr + 2);
    out.insert(out.end(), m.b, m.b + len);
}

}  // namespace nanobook::itch
