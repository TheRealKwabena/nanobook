// nanobook — stream_buffer.hpp
//
// A growable read-ahead buffer over a FILE*, so the decoders can work on a pipe
// rather than only on an mmap'd file.
//
// This exists for one concrete reason: a single day of IEX DEEP is 11-12 GB
// gzipped, and there are ~250 trading days a year. Landing that on a laptop daily
// is not viable, but nothing requires it — the pipeline only needs the derived
// features, which are megabytes. So the feed is consumed as
//
//     curl <url> | gunzip | iex_replay --symbols AAPL,MSFT --out features.csv
//
// and the 12 GB never touches disk. Peak memory is one buffer.
//
// mmap is still the right choice where the file is already local (see
// itch_parser.hpp): it avoids the copy entirely. This is the complement, not a
// replacement — a decoder written against `peek`/`consume` works on both.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nanobook {

class StreamBuffer {
  public:
    // 1 MiB holds hundreds of IEX-TP segments, so refills are rare and each one
    // is a large sequential read — which is what the kernel and the decompressor
    // upstream both want.
    static constexpr std::size_t kDefaultCapacity = 1u << 20;

    explicit StreamBuffer(std::FILE* f, std::size_t capacity = kDefaultCapacity) : f_(f) {
        buf_.resize(std::max<std::size_t>(capacity, 4096));
    }

    // Guarantee `n` contiguous readable bytes, or return nullptr if the stream
    // ends first. The returned pointer is invalidated by the next peek/consume.
    //
    // Callers must therefore finish with a message before asking for the next
    // one, which is exactly how the framing loops are written.
    [[nodiscard]] const std::byte* peek(std::size_t n) {
        if (avail() >= n) return buf_.data() + head_;
        if (n > buf_.size()) grow(n);
        fill(n);
        return avail() >= n ? buf_.data() + head_ : nullptr;
    }

    void consume(std::size_t n) noexcept {
        head_ += std::min(n, avail());
        consumed_ += n;
    }

    // Skip forward, reading and discarding as needed. Used to step over packets
    // and protocols we do not decode without materialising them.
    bool skip(std::size_t n) {
        while (n > 0) {
            if (avail() == 0 && !fill(1)) return false;
            const std::size_t take = std::min(n, avail());
            head_ += take;
            consumed_ += take;
            n -= take;
        }
        return true;
    }

    [[nodiscard]] std::size_t avail() const noexcept { return tail_ - head_; }
    [[nodiscard]] std::uint64_t bytes_consumed() const noexcept { return consumed_; }
    [[nodiscard]] bool eof() const noexcept { return eof_ && avail() == 0; }

  private:
    // Compact live bytes to the front, then read until `need` is satisfied or the
    // stream ends. Compaction is what makes a message that straddles a refill
    // boundary contiguous — the case that a naive fixed-window reader gets wrong.
    bool fill(std::size_t need) {
        if (head_ > 0) {
            const std::size_t live = avail();
            if (live > 0) std::memmove(buf_.data(), buf_.data() + head_, live);
            head_ = 0;
            tail_ = live;
        }
        while (avail() < need) {
            if (tail_ == buf_.size()) grow(buf_.size() * 2);
            const std::size_t got = std::fread(buf_.data() + tail_, 1, buf_.size() - tail_, f_);
            if (got == 0) { eof_ = true; return avail() >= need; }
            tail_ += got;
        }
        return true;
    }

    void grow(std::size_t want) {
        std::size_t cap = buf_.size();
        while (cap < want) cap *= 2;
        buf_.resize(cap);
    }

    std::FILE* f_;
    std::vector<std::byte> buf_;
    std::size_t head_ = 0, tail_ = 0;
    std::uint64_t consumed_ = 0;
    bool eof_ = false;
};

}  // namespace nanobook
