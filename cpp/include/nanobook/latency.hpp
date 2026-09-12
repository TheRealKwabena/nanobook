// nanobook — latency.hpp
//
// Timing for the replay hot path, and an honest account of its limits.
//
// MEASURED ON THIS MACHINE (Apple M5, macOS 25.5, see docs/measurement.md):
//
//   mach timebase                     125/3  =>  1 tick = 41.667 ns (24 MHz)
//   clock_gettime_nsec_np(UPTIME_RAW) 41.67 ns resolution, ~16.1 ns per call
//   clock_gettime_nsec_np(MONO_RAW)   41.67 ns resolution, ~11.8 ns per call
//   mach_absolute_time()              41.67 ns resolution, ~6.1 ns per call
//
// The consequence is the central measurement fact of this project: the userspace
// timebase on Apple Silicon ticks at 24 MHz, so a single tick is 41.67 ns, while
// processing one ITCH message costs a few tens of nanoseconds. 55-85% of
// back-to-back clock reads return the *same value*. Per-message latency here is
// below the resolution of any clock userspace can reach — on this platform the
// cycle counter (PMCCNTR_EL0) needs kernel or entitled access, unlike x86 where
// RDTSC is unprivileged and gives sub-nanosecond resolution.
//
// Pretending otherwise is how benchmarks lie. So nothing here reports a
// per-message p50. Instead:
//
//   1. AMORTIZED COST — one clock read before a multi-million-message replay and
//      one after. Instrument overhead is a single tick spread over the whole
//      run, i.e. nil. Accurate to well under a percent. This is the headline
//      throughput number.
//
//   2. BLOCK TAIL LATENCY — time blocks of kBlockSize messages. A 256-message
//      block costs ~5-15 us, which is 100-400 ticks: comfortably above the
//      granularity, so the *distribution across blocks* is real. This is what
//      surfaces the stalls that matter — page faults on first touch of the mmap,
//      order-map rehashes, ladder regrows — because those cost microseconds and
//      show up as block outliers. It cannot attribute a stall to one message,
//      and it does not claim to.
//
// To get true per-message percentiles, run on x86-64 with RDTSC, or wire up
// kperf/PMU access. docs/measurement.md records both routes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace nanobook {

// ---------------------------------------------------------------------------
// Clock. mach_absolute_time is the cheapest of the three at ~6 ns; conversion to
// nanoseconds is a multiply and divide by the cached timebase ratio, which the
// compiler turns into a multiply plus shift for the common 125/3.
// ---------------------------------------------------------------------------
class Clock {
  public:
#if defined(__APPLE__)
    // mach_absolute_time returns raw timebase ticks; converting to nanoseconds
    // needs the numer/denom ratio, which is 125/3 on Apple Silicon.
    Clock() {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        numer_ = tb.numer;
        denom_ = tb.denom;
    }

    [[nodiscard]] static std::uint64_t ticks() noexcept { return mach_absolute_time(); }

    [[nodiscard]] std::uint64_t to_ns(std::uint64_t t) const noexcept {
        return t * numer_ / denom_;
    }

    // Nanoseconds per tick — the measurement floor. 41.667 on Apple Silicon.
    [[nodiscard]] double tick_ns() const noexcept {
        return static_cast<double>(numer_) / static_cast<double>(denom_);
    }

  private:
    std::uint64_t numer_ = 1;
    std::uint64_t denom_ = 1;

#else
    // Elsewhere CLOCK_MONOTONIC_RAW already counts nanoseconds, so a tick is a
    // nanosecond and the conversion is the identity. The real resolution is
    // whatever the platform's clocksource provides — on x86-64 Linux this is
    // usually TSC-backed and far finer than Apple's 24 MHz timebase, so the
    // block-timing rationale in the header comment is conservative there rather
    // than wrong.
    Clock() = default;

    [[nodiscard]] static std::uint64_t ticks() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }

    [[nodiscard]] std::uint64_t to_ns(std::uint64_t t) const noexcept { return t; }
    [[nodiscard]] double tick_ns() const noexcept { return 1.0; }
#endif
};

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    static const Clock c;
    return c.to_ns(Clock::ticks());
}

// ---------------------------------------------------------------------------
// Log-linear histogram — HdrHistogram bucketing, 64 sub-buckets per octave, so
// ~1.5% relative error across the whole range. Recording is a CLZ, a shift and
// an increment: no allocation, no data-dependent branching, so instrumenting
// does not distort what is being measured.
// ---------------------------------------------------------------------------
class LatencyHistogram {
  public:
    static constexpr std::size_t kSubBuckets = 64;
    static constexpr std::size_t kBucketCount = 59 * kSubBuckets;

    LatencyHistogram() : counts_(kBucketCount, 0) {}

    void record(std::uint64_t v) noexcept {
        ++total_;
        sum_ += v;
        if (v > max_) max_ = v;
        ++counts_[bucket_of(v)];
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] std::uint64_t sum() const noexcept { return sum_; }
    [[nodiscard]] double mean() const noexcept {
        return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0;
    }

    // Lower bound of the bucket holding the requested quantile.
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(total_));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= target) return value_of(i);
        }
        return max_;
    }

  private:
    [[nodiscard]] static std::size_t bucket_of(std::uint64_t v) noexcept {
        if (v < kSubBuckets) return static_cast<std::size_t>(v);
        const auto e = static_cast<std::size_t>(63 - __builtin_clzll(v));  // floor(log2 v)
        const std::size_t sub = static_cast<std::size_t>(v >> (e - 6)) & (kSubBuckets - 1);
        return std::min((e - 5) * kSubBuckets + sub, kBucketCount - 1);
    }

    [[nodiscard]] static std::uint64_t value_of(std::size_t idx) noexcept {
        if (idx < kSubBuckets) return idx;
        const std::size_t e = idx / kSubBuckets + 5;
        const std::size_t sub = idx % kSubBuckets;
        return static_cast<std::uint64_t>(kSubBuckets + sub) << (e - 6);
    }

    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
};

// ---------------------------------------------------------------------------
// Block timer. Accumulates messages and takes one clock reading per block, so
// the instrument cost is amortised over kBlockSize messages (~6 ns / 256 =
// 0.02 ns per message) and each sample spans enough ticks to be meaningful.
//
// Records picoseconds per message so that sub-nanosecond per-message costs still
// land in distinct histogram buckets.
// ---------------------------------------------------------------------------
class BlockTimer {
  public:
    static constexpr std::uint64_t kBlockSize = 256;

    explicit BlockTimer(std::uint64_t block_size = kBlockSize) : block_(block_size) {}

    void start() noexcept {
        t0_ = Clock::ticks();
        run_start_ = t0_;
        in_block_ = 0;
    }

    // Call once per message processed.
    void tick() noexcept {
        if (++in_block_ < block_) return;
        const std::uint64_t t1 = Clock::ticks();
        const std::uint64_t ns = clock_.to_ns(t1 - t0_);
        // picoseconds per message
        hist_.record(ns * 1000 / block_);
        blocks_ += 1;
        t0_ = t1;
        in_block_ = 0;
    }

    void stop() noexcept { run_ns_ = clock_.to_ns(Clock::ticks() - run_start_); }

    [[nodiscard]] std::uint64_t run_ns() const noexcept { return run_ns_; }
    [[nodiscard]] std::uint64_t blocks() const noexcept { return blocks_; }
    [[nodiscard]] std::uint64_t block_size() const noexcept { return block_; }
    [[nodiscard]] const LatencyHistogram& hist() const noexcept { return hist_; }
    [[nodiscard]] double tick_ns() const noexcept { return clock_.tick_ns(); }

    // Per-message ns at a percentile of the per-block distribution.
    [[nodiscard]] double block_percentile_ns(double p) const noexcept {
        return static_cast<double>(hist_.percentile(p)) / 1000.0;
    }

  private:
    Clock clock_;
    LatencyHistogram hist_;
    std::uint64_t block_ = kBlockSize;
    std::uint64_t in_block_ = 0;
    std::uint64_t t0_ = 0;
    std::uint64_t run_start_ = 0;
    std::uint64_t run_ns_ = 0;
    std::uint64_t blocks_ = 0;
};

}  // namespace nanobook
