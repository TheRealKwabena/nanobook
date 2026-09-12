// nanobook — byte_order.hpp
//
// ITCH is a big-endian wire protocol; arm64/x86-64 are little-endian, and the
// fields are NOT aligned to their natural boundaries (a uint64 order reference
// sits at offset 11 of a message). So we cannot simply reinterpret_cast into
// the buffer: that is undefined behaviour twice over (unaligned load + strict
// aliasing violation).
//
// The idiom below — memcpy into a local, then byte-swap — is the portable,
// UB-free way to do it, and every mainstream compiler folds it into a single
// unaligned load plus one `rev` (arm64) / `bswap` (x86) instruction at -O2.
// Verified: see docs/codegen.md.
#pragma once

#include <cstdint>
#include <cstring>

namespace nanobook {

// ---------------------------------------------------------------------------
// Unaligned big-endian loads.
// `src` need not be aligned and need not outlive the call.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::uint8_t load_be8(const void* src) noexcept {
    std::uint8_t v;
    std::memcpy(&v, src, 1);
    return v;
}

[[nodiscard]] inline std::uint16_t load_be16(const void* src) noexcept {
    std::uint16_t v;
    std::memcpy(&v, src, 2);
    return __builtin_bswap16(v);
}

[[nodiscard]] inline std::uint32_t load_be32(const void* src) noexcept {
    std::uint32_t v;
    std::memcpy(&v, src, 4);
    return __builtin_bswap32(v);
}

[[nodiscard]] inline std::uint64_t load_be64(const void* src) noexcept {
    std::uint64_t v;
    std::memcpy(&v, src, 8);
    return __builtin_bswap64(v);
}

// ITCH timestamps are 48-bit: nanoseconds since midnight Eastern. 48 bits is
// ~78 hours, comfortably more than a trading day, and saves 2 bytes per
// message versus a uint64 — at ~300M messages/day that is 600 MB of wire.
//
// Read as an 8-byte load where possible (cheaper than 6 separate byte loads),
// but only when the caller guarantees 2 readable bytes past `src`. The safe
// variant below is used at buffer boundaries.
[[nodiscard]] inline std::uint64_t load_be48(const void* src) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(src);
    return (static_cast<std::uint64_t>(p[0]) << 40) |
           (static_cast<std::uint64_t>(p[1]) << 32) |
           (static_cast<std::uint64_t>(p[2]) << 24) |
           (static_cast<std::uint64_t>(p[3]) << 16) |
           (static_cast<std::uint64_t>(p[4]) << 8) |
           (static_cast<std::uint64_t>(p[5]));
}

// ---------------------------------------------------------------------------
// Big-endian stores — only used by the synthetic feed generator, so clarity
// beats speed here.
// ---------------------------------------------------------------------------

inline void store_be8(void* dst, std::uint8_t v) noexcept { std::memcpy(dst, &v, 1); }

inline void store_be16(void* dst, std::uint16_t v) noexcept {
    const std::uint16_t s = __builtin_bswap16(v);
    std::memcpy(dst, &s, 2);
}

inline void store_be32(void* dst, std::uint32_t v) noexcept {
    const std::uint32_t s = __builtin_bswap32(v);
    std::memcpy(dst, &s, 4);
}

inline void store_be64(void* dst, std::uint64_t v) noexcept {
    const std::uint64_t s = __builtin_bswap64(v);
    std::memcpy(dst, &s, 8);
}

inline void store_be48(void* dst, std::uint64_t v) noexcept {
    auto* p = static_cast<std::uint8_t*>(dst);
    p[0] = static_cast<std::uint8_t>(v >> 40);
    p[1] = static_cast<std::uint8_t>(v >> 32);
    p[2] = static_cast<std::uint8_t>(v >> 24);
    p[3] = static_cast<std::uint8_t>(v >> 16);
    p[4] = static_cast<std::uint8_t>(v >> 8);
    p[5] = static_cast<std::uint8_t>(v);
}

}  // namespace nanobook
