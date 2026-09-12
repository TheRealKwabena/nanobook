// nanobook — itch_parser.hpp
//
// Streaming ITCH 5.0 decoder over an mmap'd feed file.
//
// Two decisions dominate the design:
//
// 1. mmap, not read().
//    A TotalView session file is 5-12 GB. Reading it through a userspace buffer
//    copies every byte twice (kernel page cache -> our buffer -> our structs).
//    mmap lets the parser hand message *views* straight out of the page cache,
//    so a field access is a load from a page the kernel already populated. With
//    MADV_SEQUENTIAL the kernel prefetches ahead of us and drops pages behind,
//    so resident memory stays flat regardless of file size.
//
// 2. A template handler, not a virtual interface.
//    The dispatch switch runs once per message — ~300M times a day. If the
//    handler were an abstract base class, each message would cost an indirect
//    call the branch predictor cannot resolve and the inliner cannot see
//    through. Templating on the handler lets the compiler inline the book update
//    directly into the switch arm, which is why the whole decode+book path fits
//    in a few tens of nanoseconds.
//
// Framing: NASDAQ's historical "BinaryFILE" samples prefix each message with a
// 2-byte big-endian length. Live MoldUDP64 has no such prefix, so Framing::Raw
// derives the length from the spec table instead. Both paths are supported
// because the sequencing bug you hit in production is always in the one you
// did not test.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nanobook/itch_spec.hpp"

namespace nanobook {

// ---------------------------------------------------------------------------
// RAII read-only memory map.
// ---------------------------------------------------------------------------
class MappedFile {
  public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));

        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error("fstat " + path + ": " + std::strerror(errno));
        }
        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ == 0) {  // mmap of length 0 is an error; an empty feed is not
            data_ = nullptr;
            return;
        }

        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) {
            ::close(fd_);
            throw std::runtime_error("mmap " + path + ": " + std::strerror(errno));
        }
        data_ = static_cast<const std::byte*>(p);

        // Tell the kernel we will stream through this exactly once: prefetch
        // aggressively ahead, and do not retain pages we have passed.
        ::madvise(const_cast<void*>(p), size_, MADV_SEQUENTIAL);
    }

    ~MappedFile() {
        if (data_ != nullptr) ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept : data_(o.data_), size_(o.size_), fd_(o.fd_) {
        o.data_ = nullptr; o.size_ = 0; o.fd_ = -1;
    }
    MappedFile& operator=(MappedFile&&) = delete;

    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

  private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// Parse statistics. Counters are cheap and make silent corruption impossible to
// miss: a run that reports framing errors or unknown types is a run you throw
// away rather than publish a Sharpe ratio from.
// ---------------------------------------------------------------------------
struct ParseStats {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t unknown_type = 0;
    std::uint64_t framing_errors = 0;
    std::uint64_t length_mismatch = 0;   // framed length disagreed with the spec
    std::uint64_t truncated_tail = 0;    // bytes left over at EOF
    std::uint64_t by_type[128] = {};     // indexed by ASCII message type

    [[nodiscard]] std::uint64_t count(char t) const noexcept {
        const auto u = static_cast<unsigned char>(t);
        return u < 128 ? by_type[u] : 0;
    }
};

enum class Framing {
    BinaryFile,  // 2-byte big-endian length prefix per message (NASDAQ samples)
    Raw,         // no prefix; length comes from the spec table (MoldUDP64 payload)
};

// ---------------------------------------------------------------------------
// Inherit to get no-op defaults, then override only the messages you consume.
// Every hook is non-virtual on purpose: the parser resolves them statically.
// ---------------------------------------------------------------------------
struct HandlerBase {
    // Called for every framed message before type dispatch, regardless of type
    // or symbol. Exists so a handler can count or time the full message stream
    // including the messages it filters out. No-op by default, so the compiler
    // deletes it entirely when unused.
    void on_message(const itch::MsgView&) {}

    void on_system_event(const itch::SystemEvent&) {}
    void on_stock_directory(const itch::StockDirectory&) {}
    void on_trading_action(const itch::StockTradingAction&) {}
    void on_add_order(const itch::AddOrder&) {}
    void on_add_order_mpid(const itch::AddOrderMpid&) {}
    void on_order_executed(const itch::OrderExecuted&) {}
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice&) {}
    void on_order_cancel(const itch::OrderCancel&) {}
    void on_order_delete(const itch::OrderDelete&) {}
    void on_order_replace(const itch::OrderReplace&) {}
    void on_trade(const itch::TradeNonCross&) {}
    void on_cross_trade(const itch::CrossTrade&) {}
    void on_broken_trade(const itch::BrokenTrade&) {}
    void on_noii(const itch::Noii&) {}
    // Called for spec'd messages we do not decode, so a handler can still count
    // or log them.
    void on_other(const itch::MsgView&) {}
};

// ---------------------------------------------------------------------------
// The hot loop.
// ---------------------------------------------------------------------------
template <class Handler>
ParseStats parse(const std::byte* data, std::size_t size, Handler& h,
                 Framing framing = Framing::BinaryFile) {
    ParseStats st;
    std::size_t off = 0;

    while (off < size) {
        std::size_t body_off = off;
        std::size_t len = 0;

        if (framing == Framing::BinaryFile) {
            if (off + 2 > size) { st.truncated_tail = size - off; break; }
            len = load_be16(data + off);
            body_off = off + 2;
            if (len == 0) { ++st.framing_errors; break; }  // desynced; do not guess
        }

        if (body_off >= size) { st.truncated_tail = size - off; break; }

        const char type = static_cast<char>(data[body_off]);
        const std::size_t spec_len = itch::message_length(type);

        if (framing == Framing::Raw) {
            if (spec_len == 0) { ++st.unknown_type; ++st.framing_errors; break; }
            len = spec_len;
        } else if (spec_len != 0 && spec_len != len) {
            // Framed length disagrees with the spec: trust the frame so we stay
            // in sync, but record it — this is the signature of a file written
            // by a different ITCH version.
            ++st.length_mismatch;
        }

        if (body_off + len > size) { st.truncated_tail = size - off; break; }

        const std::byte* p = data + body_off;
        ++st.messages;
        st.bytes += len;
        if (const auto u = static_cast<unsigned char>(type); u < 128) ++st.by_type[u];

        h.on_message(itch::MsgView(p));

        // Ordered roughly by message frequency on a real session: A/D/E/X/U
        // together are ~95% of the feed, so they sit at the top of the switch.
        // (The compiler builds a jump table regardless; the order documents
        // intent for the next reader.)
        switch (type) {
            case 'A': { itch::AddOrder m(p);               h.on_add_order(m); break; }
            case 'D': { itch::OrderDelete m(p);            h.on_order_delete(m); break; }
            case 'E': { itch::OrderExecuted m(p);          h.on_order_executed(m); break; }
            case 'X': { itch::OrderCancel m(p);            h.on_order_cancel(m); break; }
            case 'U': { itch::OrderReplace m(p);           h.on_order_replace(m); break; }
            case 'F': { itch::AddOrderMpid m(p);           h.on_add_order_mpid(m); break; }
            case 'C': { itch::OrderExecutedWithPrice m(p); h.on_order_executed_with_price(m); break; }
            case 'P': { itch::TradeNonCross m(p);          h.on_trade(m); break; }
            case 'Q': { itch::CrossTrade m(p);             h.on_cross_trade(m); break; }
            case 'B': { itch::BrokenTrade m(p);            h.on_broken_trade(m); break; }
            case 'I': { itch::Noii m(p);                   h.on_noii(m); break; }
            case 'R': { itch::StockDirectory m(p);         h.on_stock_directory(m); break; }
            case 'H': { itch::StockTradingAction m(p);     h.on_trading_action(m); break; }
            case 'S': { itch::SystemEvent m(p);            h.on_system_event(m); break; }
            default: {
                if (spec_len == 0) ++st.unknown_type;
                itch::MsgView m(p);
                h.on_other(m);
                break;
            }
        }

        off = body_off + len;
    }

    return st;
}

template <class Handler>
ParseStats parse(const MappedFile& f, Handler& h, Framing framing = Framing::BinaryFile) {
    return parse(f.data(), f.size(), h, framing);
}

}  // namespace nanobook
