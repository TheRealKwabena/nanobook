# Measurement notes

Everything in the README's results table comes from the procedures here. The
point of this document is that the numbers should be reproducible and their
limits explicit.

## Machine

```
Apple M5 (arm64), macOS 25.5
Apple clang 21.0.0
L1d 64 KB, L2 6 MB
```

## Clock characterisation

The measurement floor is set by the hardware timebase, so it was probed rather
than assumed. Source: `tools/` probe reproduced below.

```
mach timebase                      125/3  =>  1 tick = 41.667 ns  (24 MHz)

                                   zero-deltas  min nonzero  amortised call
clock_gettime_nsec_np(UPTIME_RAW)     54.9%        41 ns        16.1 ns
clock_gettime_nsec_np(MONO_RAW)       64.1%        41 ns        11.8 ns
mach_absolute_time()                  85.3%         1 tick       6.1 ns
```

"zero-deltas" is the fraction of back-to-back reads returning an identical value.

### What this means

One tick is 41.667 ns. A single ITCH message costs ~13 ns to decode and apply.
**Per-message latency is below the resolution of any clock available to userspace
on this platform.** On x86-64, `RDTSC` is unprivileged and gives sub-nanosecond
resolution; on Apple Silicon the cycle counter `PMCCNTR_EL0` requires kernel or
entitled access, so there is no userspace equivalent.

Two routes to true per-message percentiles, neither taken here:

1. **Run on x86-64** and use `RDTSC` with a serialising fence. Straightforward,
   and the honest way to get the number — it just measures a different machine.
2. **`kperf` / PMU access on macOS.** Requires a kernel extension or Apple's
   private `kperf` framework and elevated privileges. Out of scope for a
   repository intended to build and run from a clean clone.

### Consequence for reported numbers

- **Throughput and mean ns/message**: one clock read before a 10M-message replay
  and one after. Instrument cost is a single tick amortised across the whole run,
  i.e. nil. Trustworthy to well under a percent.
- **Per-message distribution**: timed in 256-message blocks. Each block spans
  ~88 ticks, comfortably above the granularity, so the spread across blocks is
  real. A block's cost is divided by 256 and recorded in picoseconds so
  sub-nanosecond differences land in distinct histogram buckets.
- **Not reported**: a per-message p50. It would be an artefact of the instrument.

The block approach surfaces stalls that cost microseconds — page faults on first
touch of the `mmap`, order-map rehashes, ladder regrows — because those dominate
a block. It cannot attribute a stall to one message, and the tool's output says so.

## Histogram

HdrHistogram bucketing: 64 sub-buckets per octave, giving ~1.5% relative error
across the range. Recording is a CLZ, a shift and an increment — no allocation
and no data-dependent branching, so instrumenting does not distort the
measurement. Verified against a uniform distribution: for `[0, 100000)` the
histogram reports p50 = 49,664 and p99 = 98,304, both inside the expected 1.5%.

## Benchmark protocol

### End-to-end replay

```bash
make bench
```

Generates a 10M-message, 3-symbol feed (300 MB), warms the page cache with one
discarded run, then reports three consecutive runs. The first cold run is
excluded deliberately: it measures disk, not the engine. Cold-start for reference
is ~61 M msg/s against ~77 M warm.

Run-to-run spread across the three warm runs is under 0.3%.

### Data structures

```bash
./build/bin/bench_structures --ops 20000000 --live 200000 --reps 3
```

Both implementations replay a **single pre-generated operation script**, so each
sees byte-identical work in an identical order. Best-of-three wall time is
reported rather than the mean, because the quantity of interest is achievable
cost, not a distribution polluted by scheduler noise. Checksums derived from
lookups are accumulated and consumed so the optimiser cannot delete the work.

The order-map workload is built to match a session's access pattern specifically
where it is adversarial: **near-sequential 64-bit keys** (so a weak hash shows
up), a **stable live-set size** with sustained churn (so tombstone degradation
shows up), and keys that are never reused (so the table cannot be helped by
repeated lookups of a hot subset).

## Reproducing

```bash
make check      # correctness first — a fast wrong book is worthless
make bench      # then the numbers
```

`make check` must print `PASS` and `TOTAL 0` before any performance figure means
anything. `replay` exits nonzero on either a mismatch or a nonzero integrity
counter, so this works unmodified as a CI gate.
