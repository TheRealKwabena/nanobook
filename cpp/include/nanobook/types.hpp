// nanobook — types.hpp
//
// Core scalar types shared by every protocol the library speaks.
//
// These started life inside itch_spec.hpp. Adding IEX DEEP as a second feed made
// it clear they are not ITCH-specific: a price level is a price level, and the
// book data structures should not know which exchange's wire format produced it.
// nanobook::itch re-exports them so existing code is unaffected.
#pragma once

#include <cstdint>

namespace nanobook {

// Prices are held as integers with 4 implied decimal places, end to end.
//
// Both protocols use this scale, which is convenient, but they disagree on the
// container: ITCH sends uint32, IEX DEEP sends *signed int64*. The book stores
// the narrow unsigned form because it needs a compact array index, and the IEX
// decoder range-checks on the way in rather than assuming (see iex_spec.hpp).
//
// Nothing in the engine converts a price to double. A double cannot represent
// 0.01 exactly, and a book that rounds is a book that silently loses shares.
using Price4 = std::uint32_t;

// IEX DEEP's wire price: signed, 8 bytes, same 4 implied decimals.
using Price8 = std::int64_t;

using Shares = std::uint32_t;

// Nanoseconds. ITCH counts from midnight US/Eastern in 48 bits; IEX counts from
// the POSIX epoch UTC in signed 64 bits. Both fit here; which epoch applies is a
// property of the feed, and the readers say which they produce.
using Nanos = std::uint64_t;

inline constexpr Price4 kPriceScale = 10000;

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] inline Side side_from_char(char c) noexcept {
    return c == 'B' ? Side::Buy : Side::Sell;
}

[[nodiscard]] inline char side_to_char(Side s) noexcept {
    return s == Side::Buy ? 'B' : 'S';
}

}  // namespace nanobook
