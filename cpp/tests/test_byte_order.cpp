// Byte-order primitives. These are three lines each, and every single field
// decode in the project depends on them being right, so they get pinned against
// hand-written byte patterns rather than against another implementation.
#include "framework.hpp"
#include "nanobook/byte_order.hpp"

using namespace nanobook;

NB_TEST(byte_order, big_endian_loads_against_literal_bytes) {
    const std::uint8_t raw[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    CHECK_EQ(load_be8(raw), 0x12u);
    CHECK_EQ(load_be16(raw), 0x1234u);
    CHECK_EQ(load_be32(raw), 0x12345678u);
    CHECK_EQ(load_be48(raw), 0x123456789ABCull);
    CHECK_EQ(load_be64(raw), 0x123456789ABCDEF0ull);
}

NB_TEST(byte_order, unaligned_loads_match_aligned) {
    // Every ITCH field of interest is unaligned: the order reference sits at
    // offset 11. Shift the same pattern through every byte offset in a word.
    alignas(16) std::uint8_t buf[32] = {};
    for (std::size_t shift = 0; shift < 8; ++shift) {
        std::uint8_t* p = buf + shift;
        p[0] = 0xDE; p[1] = 0xAD; p[2] = 0xBE; p[3] = 0xEF;
        p[4] = 0xCA; p[5] = 0xFE; p[6] = 0xBA; p[7] = 0xBE;
        CHECK_EQ(load_be16(p), 0xDEADu);
        CHECK_EQ(load_be32(p), 0xDEADBEEFu);
        CHECK_EQ(load_be48(p), 0xDEADBEEFCAFEull);
        CHECK_EQ(load_be64(p), 0xDEADBEEFCAFEBABEull);
    }
}

NB_TEST(byte_order, store_then_load_round_trips) {
    std::byte b[8];
    store_be16(b, 0xABCD);            CHECK_EQ(load_be16(b), 0xABCDu);
    store_be32(b, 0x01020304);        CHECK_EQ(load_be32(b), 0x01020304u);
    store_be64(b, 0x0102030405060708ull); CHECK_EQ(load_be64(b), 0x0102030405060708ull);
    store_be48(b, 0x0000FFEEDDCCBBAAull & 0xFFFFFFFFFFFFull);
    CHECK_EQ(load_be48(b), 0xEEDDCCBBAAull | 0xFF0000000000ull);
}

NB_TEST(byte_order, store_is_actually_big_endian_not_host_order) {
    // The bug this catches: forgetting the swap entirely on a little-endian host,
    // which round-trips perfectly and corrupts every real file.
    std::byte b[4];
    store_be32(b, 0x000000FF);
    CHECK_EQ(static_cast<unsigned>(b[0]), 0x00u);
    CHECK_EQ(static_cast<unsigned>(b[3]), 0xFFu);  // low byte goes LAST on the wire
}

NB_TEST(byte_order, timestamp_48bit_covers_a_trading_day) {
    // 48 bits must hold nanoseconds since midnight through the close.
    const std::uint64_t ns_at_close = 16ull * 3600 * 1000000000ull;
    std::byte b[8] = {};
    store_be48(b, ns_at_close);
    CHECK_EQ(load_be48(b), ns_at_close);
    CHECK(ns_at_close < (1ull << 48));
}
