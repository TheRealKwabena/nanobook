// nanobook — pcap.hpp
//
// Streaming reader for network captures, in both formats that matter: classic
// libpcap and pcap-ng.
//
// pcap-ng is not optional. IEX HIST files are named `*.pcap.gz`, and the DEEP
// specification says "PCAP or PCAP-NG", which reads like classic pcap is the
// common case. It is not: the actual downloads begin with the pcap-ng section
// header magic 0x0A0D0D0A and an option reading "File created by merging:",
// because IEX merges the A and B multicast captures. A decoder that only handled
// classic pcap would reject every real file — which is what this one did until it
// met one.
//
// Both readers expose the same interface, so the layers above (IEX-TP, DEEP) do
// not know or care which container they came from.
//
//   classic pcap                      pcap-ng
//   ------------                      -------
//   24-byte global header             Section Header Block  (endianness, version)
//   per packet:                       Interface Description Block (link type,
//     16-byte record header             timestamp resolution)
//     packet bytes                    Enhanced Packet Block per packet
//                                     ...and other block types, skipped
//
// The endianness of the container is discovered from its magic and is independent
// of the byte order of the payload inside — IEX ships little-endian DEEP inside a
// little-endian pcap-ng holding big-endian network headers. All three appear in
// this file.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "nanobook/byte_order.hpp"
#include "nanobook/stream_buffer.hpp"

namespace nanobook {

// Link-layer types (https://www.tcpdump.org/linktypes.html).
inline constexpr std::uint32_t kLinktypeEthernet = 1;
inline constexpr std::uint32_t kLinktypeRaw = 101;
inline constexpr std::uint32_t kLinktypeLinuxSll = 113;

struct CaptureStats {
    std::uint64_t packets = 0;
    std::uint64_t non_ip_packets = 0;
    std::uint64_t non_udp_packets = 0;
    std::uint64_t truncated_packets = 0;
    std::uint64_t blocks_skipped = 0;     // pcap-ng blocks we do not decode
    std::uint64_t bad_blocks = 0;
    std::uint64_t bytes = 0;
};

// ---------------------------------------------------------------------------
// Peel Ethernet (+ VLAN tags) / IPv4 / UDP down to the UDP payload.
//
// Network headers are big-endian, which is a pleasant trap in a file whose
// payload is little-endian. The IPv4 header length is read from the packet rather
// than assumed to be 20 bytes: IP options are rare but assuming them away shifts
// every subsequent field.
// ---------------------------------------------------------------------------
inline void extract_udp_payload(const std::byte* p, std::size_t len, std::uint32_t linktype,
                                const std::byte*& payload, std::size_t& payload_len,
                                CaptureStats& st) {
    payload = nullptr;
    payload_len = 0;
    std::size_t off = 0;

    if (linktype == kLinktypeEthernet) {
        if (len < 14) { ++st.non_ip_packets; return; }
        std::uint16_t ethertype = load_be16(p + 12);
        off = 14;
        // 802.1Q / 802.1ad: each tag inserts 4 bytes before the real ethertype.
        while ((ethertype == 0x8100 || ethertype == 0x88a8) && off + 4 <= len) {
            ethertype = load_be16(p + off + 2);
            off += 4;
        }
        if (ethertype != 0x0800) { ++st.non_ip_packets; return; }  // not IPv4
    } else if (linktype == kLinktypeLinuxSll) {
        if (len < 16) { ++st.non_ip_packets; return; }
        if (load_be16(p + 14) != 0x0800) { ++st.non_ip_packets; return; }
        off = 16;
    }
    // kLinktypeRaw starts at the IP header, so off stays 0.

    if (off + 20 > len) { ++st.non_ip_packets; return; }
    const auto vihl = static_cast<std::uint8_t>(p[off]);
    if ((vihl >> 4) != 4) { ++st.non_ip_packets; return; }
    const std::size_t ihl = static_cast<std::size_t>(vihl & 0x0f) * 4;
    if (ihl < 20 || off + ihl > len) { ++st.non_ip_packets; return; }
    if (static_cast<std::uint8_t>(p[off + 9]) != 17) { ++st.non_udp_packets; return; }  // 17 = UDP
    off += ihl;

    if (off + 8 > len) { ++st.non_udp_packets; return; }
    const std::uint16_t udp_len = load_be16(p + off + 4);
    off += 8;
    std::size_t n = (udp_len >= 8) ? static_cast<std::size_t>(udp_len - 8u) : 0u;
    // A snaplen-truncated capture holds less than the UDP header claims.
    n = std::min(n, len - off);

    payload = p + off;
    payload_len = n;
}

// ---------------------------------------------------------------------------
// One packet handed up to the caller. `data` points into the stream buffer and is
// valid only until the next call.
// ---------------------------------------------------------------------------
struct CapturedPacket {
    const std::byte* payload = nullptr;   // UDP payload, or nullptr
    std::size_t payload_len = 0;
    std::uint64_t capture_ns = 0;         // ns since the epoch
};

enum class CaptureFormat { ClassicPcap, PcapNg };

// ---------------------------------------------------------------------------
// Unified streaming capture reader.
// ---------------------------------------------------------------------------
class CaptureReader {
  public:
    // Sniffs the format from the leading magic and reads its header(s).
    bool open(StreamBuffer& in, std::string& error) {
        const std::byte* p = in.peek(4);
        if (p == nullptr) { error = "stream ended before a capture header"; return false; }
        const std::uint32_t magic = load_le32(p);

        if (magic == 0x0A0D0D0A) {
            format_ = CaptureFormat::PcapNg;
            return open_pcapng(in, error);
        }
        switch (magic) {
            case 0xa1b2c3d4: swapped_ = false; ts_div_ = 1'000'000; break;   // usec
            case 0xd4c3b2a1: swapped_ = true;  ts_div_ = 1'000'000; break;
            case 0xa1b23c4d: swapped_ = false; ts_div_ = 1'000'000'000; break;  // nsec
            case 0x4d3cb2a1: swapped_ = true;  ts_div_ = 1'000'000'000; break;
            default:
                error = "unrecognised capture magic 0x" + hex32(magic) +
                        "; not a pcap or pcap-ng stream";
                return false;
        }
        format_ = CaptureFormat::ClassicPcap;
        return open_classic(in, error);
    }

    [[nodiscard]] CaptureFormat format() const noexcept { return format_; }
    [[nodiscard]] std::uint32_t linktype() const noexcept { return linktype_; }
    [[nodiscard]] const std::string& comment() const noexcept { return comment_; }

    // Reads the next packet, or returns false at end of stream. The payload
    // pointer is valid until the following call.
    bool next(StreamBuffer& in, CapturedPacket& out, CaptureStats& st) {
        return format_ == CaptureFormat::ClassicPcap ? next_classic(in, out, st)
                                                    : next_pcapng(in, out, st);
    }

  private:
    static std::string hex32(std::uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "%08x", v);
        return b;
    }

    [[nodiscard]] std::uint16_t u16(const std::byte* p) const noexcept {
        const std::uint16_t v = load_le16(p);
        return swapped_ ? __builtin_bswap16(v) : v;
    }
    [[nodiscard]] std::uint32_t u32(const std::byte* p) const noexcept {
        const std::uint32_t v = load_le32(p);
        return swapped_ ? __builtin_bswap32(v) : v;
    }

    // ---- classic pcap ----

    bool open_classic(StreamBuffer& in, std::string& error) {
        const std::byte* p = in.peek(24);
        if (p == nullptr) { error = "truncated pcap global header"; return false; }
        linktype_ = u32(p + 20);
        in.consume(24);
        if (!supported_linktype(linktype_)) {
            error = "unsupported link-layer type " + std::to_string(linktype_);
            return false;
        }
        return true;
    }

    bool next_classic(StreamBuffer& in, CapturedPacket& out, CaptureStats& st) {
        const std::byte* h = in.peek(16);
        if (h == nullptr) return false;
        const std::uint32_t ts_sec = u32(h);
        const std::uint32_t ts_frac = u32(h + 4);
        const std::uint32_t incl = u32(h + 8);
        const std::uint32_t orig = u32(h + 12);
        in.consume(16);

        const std::byte* pkt = in.peek(incl);
        if (pkt == nullptr) return false;
        ++st.packets;
        st.bytes += incl;
        if (orig > incl) ++st.truncated_packets;

        out.capture_ns = static_cast<std::uint64_t>(ts_sec) * 1'000'000'000ull +
                         static_cast<std::uint64_t>(ts_frac) * (1'000'000'000ull / ts_div_);
        extract_udp_payload(pkt, incl, linktype_, out.payload, out.payload_len, st);
        pending_ = incl;
        return true;
    }

    // ---- pcap-ng ----

    bool open_pcapng(StreamBuffer& in, std::string& error) {
        // Section Header Block: type(4) len(4) bom(4) major(2) minor(2)
        // section_len(8) options... len(4)
        const std::byte* p = in.peek(12);
        if (p == nullptr) { error = "truncated pcap-ng section header"; return false; }
        const std::uint32_t bom = load_le32(p + 8);
        if (bom == 0x1A2B3C4D) swapped_ = false;
        else if (bom == 0x4D3C2B1A) swapped_ = true;
        else { error = "bad pcap-ng byte-order magic 0x" + hex32(bom); return false; }

        const std::uint32_t block_len = u32(p + 4);
        if (block_len < 28 || block_len > (1u << 24)) {
            error = "implausible pcap-ng section header length " + std::to_string(block_len);
            return false;
        }
        const std::byte* blk = in.peek(block_len);
        if (blk == nullptr) { error = "truncated pcap-ng section header block"; return false; }

        // opt_comment (code 1) often records provenance — worth surfacing, since
        // for IEX it says the file is a merge of the A and B feeds.
        read_shb_options(blk + 24, block_len - 28);
        in.consume(block_len);

        // The first Interface Description Block carries the link type and the
        // timestamp resolution; both are needed before any packet can be read.
        return find_first_idb(in, error);
    }

    void read_shb_options(const std::byte* p, std::size_t len) {
        std::size_t off = 0;
        while (off + 4 <= len) {
            const std::uint16_t code = u16(p + off);
            const std::uint16_t olen = u16(p + off + 2);
            off += 4;
            if (code == 0 || off + olen > len) break;   // opt_endofopt
            if (code == 1 && comment_.empty()) {
                const auto* c = reinterpret_cast<const char*>(p + off);
                std::size_t n = olen;
                while (n > 0 && (c[n - 1] == '\n' || c[n - 1] == ' ' || c[n - 1] == '\0')) --n;
                comment_.assign(c, n);
            }
            off += (olen + 3u) & ~3u;   // options are padded to 4 bytes
        }
    }

    bool find_first_idb(StreamBuffer& in, std::string& error) {
        for (;;) {
            const std::byte* h = in.peek(8);
            if (h == nullptr) { error = "pcap-ng ended before an interface description"; return false; }
            const std::uint32_t type = u32(h);
            const std::uint32_t len = u32(h + 4);
            if (len < 12 || len > (1u << 24)) { error = "corrupt pcap-ng block length"; return false; }

            if (type == 0x00000001) {   // Interface Description Block
                const std::byte* blk = in.peek(len);
                if (blk == nullptr) { error = "truncated pcap-ng interface block"; return false; }
                linktype_ = u16(blk + 8);
                read_idb_options(blk + 16, len - 20);
                in.consume(len);
                if (!supported_linktype(linktype_)) {
                    error = "unsupported link-layer type " + std::to_string(linktype_);
                    return false;
                }
                return true;
            }
            if (!in.skip(len)) { error = "pcap-ng ended mid-block"; return false; }
        }
    }

    // if_tsresol (option code 9): one byte. High bit clear means 10^-v seconds,
    // set means 2^-v. Default is microseconds. Getting this wrong scales every
    // capture timestamp by a factor of 1000.
    void read_idb_options(const std::byte* p, std::size_t len) {
        std::size_t off = 0;
        while (off + 4 <= len) {
            const std::uint16_t code = u16(p + off);
            const std::uint16_t olen = u16(p + off + 2);
            off += 4;
            if (code == 0 || off + olen > len) break;
            if (code == 9 && olen >= 1) {
                const auto v = static_cast<std::uint8_t>(p[off]);
                if ((v & 0x80) == 0) {
                    std::uint64_t d = 1;
                    for (std::uint8_t i = 0; i < (v & 0x7f) && i < 18; ++i) d *= 10;
                    ts_div_ = d;
                } else {
                    ts_div_ = 1ull << (v & 0x7f);
                }
            }
            off += (olen + 3u) & ~3u;
        }
    }

    bool next_pcapng(StreamBuffer& in, CapturedPacket& out, CaptureStats& st) {
        for (;;) {
            const std::byte* h = in.peek(8);
            if (h == nullptr) return false;
            const std::uint32_t type = u32(h);
            const std::uint32_t len = u32(h + 4);
            if (len < 12 || len > (1u << 24)) { ++st.bad_blocks; return false; }

            if (type == 0x00000006) {   // Enhanced Packet Block
                const std::byte* blk = in.peek(len);
                if (blk == nullptr) return false;
                // body: iface(4) ts_hi(4) ts_lo(4) cap_len(4) orig_len(4) data...
                const std::uint32_t ts_hi = u32(blk + 12);
                const std::uint32_t ts_lo = u32(blk + 16);
                const std::uint32_t cap_len = u32(blk + 20);
                const std::uint32_t orig_len = u32(blk + 24);
                if (28u + cap_len > len) { ++st.bad_blocks; in.consume(len); continue; }

                const std::uint64_t ticks = (static_cast<std::uint64_t>(ts_hi) << 32) | ts_lo;
                out.capture_ns = ts_div_ == 0 ? ticks
                                             : (ticks / ts_div_) * 1'000'000'000ull +
                                               (ticks % ts_div_) * (1'000'000'000ull / ts_div_);
                ++st.packets;
                st.bytes += cap_len;
                if (orig_len > cap_len) ++st.truncated_packets;

                extract_udp_payload(blk + 28, cap_len, linktype_, out.payload, out.payload_len, st);
                pending_ = len;
                return true;
            }

            if (type == 0x00000001) {   // another interface; refresh link type
                const std::byte* blk = in.peek(len);
                if (blk == nullptr) return false;
                linktype_ = u16(blk + 8);
                read_idb_options(blk + 16, len - 20);
                in.consume(len);
                continue;
            }
            if (type == 0x0A0D0D0A) {   // a new section: re-read endianness
                std::string err;
                if (!open_pcapng(in, err)) return false;
                continue;
            }
            // Simple Packet Block, Name Resolution, Interface Statistics, custom
            // blocks: counted and skipped.
            ++st.blocks_skipped;
            if (!in.skip(len)) return false;
        }
    }

    static bool supported_linktype(std::uint32_t lt) noexcept {
        return lt == kLinktypeEthernet || lt == kLinktypeRaw || lt == kLinktypeLinuxSll;
    }

  public:
    // Must be called once the payload from next() has been consumed.
    void finish(StreamBuffer& in) noexcept {
        in.consume(pending_);
        pending_ = 0;
    }

  private:
    CaptureFormat format_ = CaptureFormat::ClassicPcap;
    bool swapped_ = false;
    std::uint64_t ts_div_ = 1'000'000;   // ticks per second
    std::uint32_t linktype_ = kLinktypeEthernet;
    std::size_t pending_ = 0;
    std::string comment_;
};

}  // namespace nanobook
