// nanobook — iex_transport.hpp
//
// The layers between an IEX HIST file and a DEEP message.
//
// A HIST download is a raw network capture, so the decoder has to peel four
// wrappers before it reaches anything tradeable:
//
//     pcap file
//       └─ pcap packet record (16-byte header)
//            └─ Ethernet frame (+ optional VLAN tags)
//                 └─ IPv4 + UDP
//                      └─ IEX-TP segment (40-byte header)
//                           └─ [2-byte length][DEEP message]  x N
//
// Each layer is a place to get it wrong quietly, so each one is validated rather
// than assumed: the pcap magic fixes the byte order and timestamp precision, the
// IPv4 header length is read rather than assumed to be 20, non-UDP packets are
// counted, and the IEX-TP payload length is cross-checked against the sum of the
// message blocks inside it.
//
// IEX-TP also carries sequence numbers, which makes gap detection possible: a
// multicast capture can genuinely be missing packets, and a book reconstructed
// across a gap is wrong in a way nothing else will reveal. Gaps are counted and
// reported rather than silently bridged.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "nanobook/byte_order.hpp"
#include "nanobook/iex_spec.hpp"
#include "nanobook/pcap.hpp"
#include "nanobook/stream_buffer.hpp"

namespace nanobook::iex {

// ---------------------------------------------------------------------------
// IEX-TP segment header — 40 bytes, little-endian.
//
//   off  len  field
//     0    1  Version                       (0x01 for IEX-TP v1)
//     1    1  Reserved
//     2    2  Message Protocol ID           (0x8004 == DEEP v1.0)
//     4    4  Channel ID
//     8    4  Session ID                    (identifies the trading session)
//    12    2  Payload Length                (bytes after this header)
//    14    2  Message Count                 (0 for a heartbeat)
//    16    8  Stream Offset                 (signed; byte offset in the stream)
//    24    8  First Message Sequence Number (signed)
//    32    8  Send Time                     (ns since epoch)
// ---------------------------------------------------------------------------
inline constexpr std::size_t kSegmentHeaderLen = 40;

// Message Protocol IDs seen in HIST files.
inline constexpr std::uint16_t kProtocolDeep1_0 = 0x8004;
inline constexpr std::uint16_t kProtocolTops1_5 = 0x8003;
inline constexpr std::uint16_t kProtocolTops1_6 = 0x8003;

struct SegmentHeader {
    std::uint8_t  version;
    std::uint16_t protocol_id;
    std::uint32_t channel_id;
    std::uint32_t session_id;
    std::uint16_t payload_length;
    std::uint16_t message_count;
    std::int64_t  stream_offset;
    std::int64_t  first_sequence;
    std::int64_t  send_time;

    [[nodiscard]] static SegmentHeader parse(const std::byte* p) noexcept {
        SegmentHeader h;
        h.version        = load_le8(p);
        // p[1] is reserved.
        h.protocol_id    = load_le16(p + 2);
        h.channel_id     = load_le32(p + 4);
        h.session_id     = load_le32(p + 8);
        h.payload_length = load_le16(p + 12);
        h.message_count  = load_le16(p + 14);
        h.stream_offset  = load_le64s(p + 16);
        h.first_sequence = load_le64s(p + 24);
        h.send_time      = load_le64s(p + 32);
        return h;
    }
};

// ---------------------------------------------------------------------------
// Statistics. Every counter here is a way the capture can be imperfect, and a
// research run should refuse to publish a number while any of them is unexpected.
// ---------------------------------------------------------------------------
struct TransportStats {
    CaptureStats  capture;                 // container-level counters (pcap.hpp)
    std::uint64_t segments = 0;
    std::uint64_t heartbeats = 0;          // message_count == 0
    std::uint64_t messages = 0;
    std::uint64_t wrong_protocol = 0;      // e.g. a TOPS file fed to the DEEP path
    std::uint64_t bad_version = 0;
    std::uint64_t payload_length_mismatch = 0;
    std::uint64_t unknown_message_type = 0;
    std::uint64_t length_mismatch = 0;     // framed length != spec length
    std::uint64_t sequence_gaps = 0;
    std::uint64_t sequence_gap_messages = 0;  // total messages missing
    std::uint64_t sequence_regressions = 0;   // duplicate / replayed segment
    std::uint64_t session_changes = 0;

    [[nodiscard]] std::uint64_t defects() const noexcept {
        return bad_version + payload_length_mismatch + unknown_message_type +
               length_mismatch + sequence_gaps + sequence_regressions +
               capture.truncated_packets + capture.bad_blocks;
    }
};

// ---------------------------------------------------------------------------
// Handler: override what you consume. All no-ops by default, statically
// dispatched exactly as in the ITCH parser, so unused hooks cost nothing.
// ---------------------------------------------------------------------------
struct DeepHandlerBase {
    void on_segment(const SegmentHeader&) {}
    void on_message(std::uint8_t /*type*/, const std::byte* /*body*/, std::size_t /*len*/) {}
    void on_system_event(const SystemEvent&) {}
    void on_security_directory(const SecurityDirectory&) {}
    void on_trading_status(const TradingStatus&) {}
    void on_operational_halt(const OperationalHaltStatus&) {}
    void on_short_sale_price_test(const ShortSalePriceTestStatus&) {}
    void on_retail_liquidity(const RetailLiquidityIndicator&) {}
    void on_security_event(const SecurityEvent&) {}
    void on_price_level_update(const PriceLevelUpdate&) {}
    void on_trade_report(const TradeReport&) {}
    void on_trade_break(const TradeBreak&) {}
    void on_official_price(const OfficialPrice&) {}
    void on_auction_information(const AuctionInformation&) {}
};

// ---------------------------------------------------------------------------
// Decode one IEX-TP segment payload: a run of [2-byte length][message] blocks.
// `payload` must hold `payload_len` readable bytes.
// ---------------------------------------------------------------------------
template <class Handler>
void decode_segment_payload(const std::byte* payload, std::size_t payload_len,
                            std::uint16_t message_count, Handler& h, TransportStats& st) {
    std::size_t off = 0;
    std::uint16_t seen = 0;

    while (off + 2 <= payload_len && seen < message_count) {
        const std::size_t len = load_le16(payload + off);
        off += 2;
        if (len == 0 || off + len > payload_len) { ++st.payload_length_mismatch; return; }

        const std::byte* body = payload + off;
        const auto type = static_cast<std::uint8_t>(body[0]);

        // Cross-check the framed length against the spec table. A mismatch means
        // the file is a different DEEP version than this decoder implements — the
        // sort of thing that otherwise surfaces as inexplicably wrong prices.
        const std::size_t spec_len = message_length(type);
        if (spec_len == 0) ++st.unknown_message_type;
        else if (spec_len != len) ++st.length_mismatch;

        ++st.messages;
        ++seen;
        h.on_message(type, body, len);

        // Ordered by frequency: price level updates are the overwhelming majority
        // of a DEEP session, trades next.
        if (spec_len == len) {
            switch (type) {
                case 0x38:
                case 0x35: { PriceLevelUpdate m(body); h.on_price_level_update(m); break; }
                case 0x54: { TradeReport m(body);      h.on_trade_report(m); break; }
                case 0x44: { SecurityDirectory m(body); h.on_security_directory(m); break; }
                case 0x48: { TradingStatus m(body);    h.on_trading_status(m); break; }
                case 0x49: { RetailLiquidityIndicator m(body); h.on_retail_liquidity(m); break; }
                case 0x4f: { OperationalHaltStatus m(body);    h.on_operational_halt(m); break; }
                case 0x50: { ShortSalePriceTestStatus m(body); h.on_short_sale_price_test(m); break; }
                case 0x45: { SecurityEvent m(body);    h.on_security_event(m); break; }
                case 0x53: { SystemEvent m(body);      h.on_system_event(m); break; }
                case 0x58: { OfficialPrice m(body);    h.on_official_price(m); break; }
                case 0x42: { TradeBreak m(body);       h.on_trade_break(m); break; }
                case 0x41: { AuctionInformation m(body); h.on_auction_information(m); break; }
                default: break;
            }
        }
        off += len;
    }

    if (seen != message_count) ++st.payload_length_mismatch;
}

// ---------------------------------------------------------------------------
// Top-level: stream a pcap of IEX-TP/DEEP and dispatch every message.
// ---------------------------------------------------------------------------
// `max_packets` of 0 means the whole stream. A limit is essential in practice:
// a real HIST day is 11-12 GB, and verifying a decoder against the first few
// hundred thousand packets takes seconds instead of an hour.
// `max_packets` of 0 means the whole stream. A limit is essential in practice: a
// real HIST day is 11-12 GB, and verifying a decoder against the first few hundred
// thousand packets takes seconds instead of an hour.
template <class Handler>
TransportStats stream_deep_pcap(StreamBuffer& in, Handler& h, std::string& error,
                                std::uint16_t want_protocol = kProtocolDeep1_0,
                                std::uint64_t max_packets = 0) {
    TransportStats st;
    CaptureReader cap;
    if (!cap.open(in, error)) return st;

    std::int64_t expect_seq = -1;
    std::uint32_t session = 0;
    bool have_session = false;
    CapturedPacket pkt;

    while (cap.next(in, pkt, st.capture)) {
        if (max_packets != 0 && st.capture.packets > max_packets) { cap.finish(in); break; }

        if (pkt.payload != nullptr && pkt.payload_len >= kSegmentHeaderLen) {
            const SegmentHeader hdr = SegmentHeader::parse(pkt.payload);

            if (hdr.version != 1) {
                ++st.bad_version;
            } else if (hdr.protocol_id != want_protocol) {
                // A TOPS file handed to the DEEP decoder lands here rather than
                // producing plausible nonsense.
                ++st.wrong_protocol;
            } else {
                ++st.segments;

                if (!have_session || hdr.session_id != session) {
                    if (have_session) ++st.session_changes;
                    session = hdr.session_id;
                    have_session = true;
                    expect_seq = -1;   // sequence numbering restarts per session
                }

                // Sequence continuity. IEX HIST is a merge of the A and B
                // multicast feeds, so the same sequence can legitimately appear
                // twice; those show up as regressions rather than gaps. A real gap
                // means messages are missing, and a book rebuilt across one is
                // wrong with no other symptom.
                if (hdr.message_count > 0) {
                    if (expect_seq >= 0) {
                        if (hdr.first_sequence > expect_seq) {
                            ++st.sequence_gaps;
                            st.sequence_gap_messages +=
                                static_cast<std::uint64_t>(hdr.first_sequence - expect_seq);
                        } else if (hdr.first_sequence < expect_seq) {
                            ++st.sequence_regressions;
                        }
                    }
                    expect_seq = std::max<std::int64_t>(
                        expect_seq, hdr.first_sequence + hdr.message_count);
                } else {
                    ++st.heartbeats;
                }

                h.on_segment(hdr);

                const std::size_t body_len = std::min<std::size_t>(
                    hdr.payload_length, pkt.payload_len - kSegmentHeaderLen);
                if (body_len != hdr.payload_length) ++st.payload_length_mismatch;
                if (hdr.message_count > 0) {
                    decode_segment_payload(pkt.payload + kSegmentHeaderLen, body_len,
                                           hdr.message_count, h, st);
                }
            }
        }
        cap.finish(in);
    }
    return st;
}

}  // namespace nanobook::iex
