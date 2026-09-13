# nanobook

[![ci](https://github.com/TheRealKwabena/nanobook/actions/workflows/ci.yml/badge.svg)](https://github.com/TheRealKwabena/nanobook/actions/workflows/ci.yml)

A NASDAQ TotalView-ITCH 5.0 feed handler and limit order book reconstructor in C++20,
built to be **fast enough to matter** and **verified well enough to trust**.

Reconstructing an order book from ITCH is deceptively easy to do *almost* right.
The feed is differential — there is no intraday snapshot — so the book you hold at
15:00 is the accumulated result of every message since the opening bell. A single
mishandled message silently corrupts every feature derived from the book for the
rest of the day, and nothing in the feed tells you. Most implementations are
wrong in at least one of the five ways documented [below](#the-five-ways-this-goes-wrong),
and they look fine.

So this project spends as much effort on *proving* the book is right as on making
it fast: the synthetic feed generator maintains an independent `std::map`-based
shadow book and emits ground-truth top-of-book after **every message**, which the
reconstruction is then checked against event by event.

```
77.0 M messages/sec   13.0 ns/message   2.3 GB/s        single core, Apple M5
0 mismatches across 333,045 verified events             byte-exact vs. oracle
58 tests, 186,913 assertions, clean under ASan + UBSan
```

---

## Results

Apple M5, single core, `-O3 -march=native`, 10M-message synthetic feed (300 MB,
3 interleaved symbols). Reproduce with `make bench`.

### Throughput

| | |
|---|---|
| Messages/sec | **77.0 M** |
| ns/message (mean) | **13.0** |
| Feed rate | **2,315 MB/s** |
| Wall time, 10M messages | 0.130 s |

At 13 ns/message, a full 300M-message NASDAQ session-day decodes and reconstructs
in about **4 seconds** on one core.

### Per-message cost distribution

Derived from the spread across 256-message blocks — see
[Measurement honesty](#measurement-honesty) for why it is not measured per message.

| Percentile | ns/message |
|---|---|
| p50 | 13.8 |
| p90 | 17.2 |
| p99 | 26.1 |
| p99.9 | 53.8 |
| p99.99 | 176.1 |
| max block | 837.9 |

### Data structures vs. the standard library

`make bench` runs both head-to-head on a replayed, deterministic operation script.

| Workload | nanobook | Standard library | Speedup |
|---|---|---|---|
| Order-reference map<br><sub>200k live orders, near-sequential keys, 55% lookup / 23% erase / 22% insert</sub> | `OrderMap` **9.84 ns/op** | `std::unordered_map` 12.60 ns/op | **1.28x** |
| Book update + best-price query<br><sub>updates clustered near the touch</sub> | `PriceLadder` **7.35 ns/op** | `std::map` 14.91 ns/op | **2.03x** |

These ratios are platform-dependent and should not be quoted as universal. The
same benchmark on CI's shared x86-64 runners reports 1.44x and 1.23x — different
cache hierarchy, different standard library, noisier host. The numbers above are
Apple M5 with libc++; reproduce your own with `make bench`.

1.28x on the mean is a real but modest win, and worth stating plainly: libc++'s
`unordered_map` is not slow. The reasons to keep the custom table are the ones a
mean does not show — it performs **no allocation at all** in the steady state, so
there is no allocator lock and no `malloc` slow path in the tail, and its load
factor and probe distribution are observable rather than opaque. For a quoting
strategy the bounded tail is worth more than the mean.

---

## Quickstart

No dependencies beyond a C++20 compiler. Nothing to download.

```bash
make tools                        # build gen_itch, replay, bench_structures
make data                         # synthesise feeds + ground truth into data/
make check                        # tests under ASan/UBSan, then end-to-end verify
```

Reconstruct a book and see everything at once:

```bash
./build/bin/replay --feed data/small.itch --symbol NBSYN --truth data/small_truth.csv
```

```
book  (NBSYN, locate 1000)
  events applied      333045
  add/exec/cancel/del/repl  136478 / 20206 / 19805 / 109757 / 40176
  live orders at end  24261
  levels bid/ask      15 / 18
  tape trades/shares  26186 / 6124990
  hidden ('P') trades 6623 (1999600 shares, book untouched)

order map
  capacity            524288 (load 0.05)
  mean probes/lookup  1.016
  longest probe run   4

integrity  (all zero == clean reconstruction)
  unknown ref  E/X/D/U   0 / 0 / 0 / 0
  overfill / overcancel  0 / 0
  negative level qty     0
  removal, no such level 0
  off-tick prices        0
  duplicate order refs   0
  crossed book at end    no
  TOTAL                  0

book at end of feed (top 3)
               bid   shares   ords  |           ask   shares   ords
   0      100.0000  2407662   5630  |      100.0100  2410334   5636
   1       99.9900  1400255   3027  |      100.0200  1386797   2930
   2       99.9800   736728   1571  |      100.0300   753129   1594
  spread 0.0100 (1 ticks)

verification against ground truth
  rows compared       333045
  mismatches          0
  events beyond truth 0
  truth rows unused   0
  csv parse errors    0
  RESULT              PASS — byte-exact match on every event
```

`replay` exits nonzero if any integrity counter is nonzero or any event mismatches,
so it works as a CI gate.

### Running against real NASDAQ data

The synthetic feed exists so the repo is self-contained, not as a substitute for
real data. NASDAQ publishes free historical TotalView-ITCH 5.0 sample files (one
full session, 5–12 GB) on their FTP site. `replay` reads them directly — the
BinaryFILE framing is the default:

```bash
./build/bin/replay --feed 01302019.NASDAQ_ITCH50 --symbol AAPL
```

Expect nonzero `unknown ref` counters if the file starts mid-session: the adds for
those orders happened before the first byte. That is a property of the data, not a
bug, which is exactly why the counters are reported rather than asserted on.

---

## The five ways this goes wrong

Each of these produces a book that looks plausible and passes an end-of-day
sanity check. Each has a dedicated test in
[`cpp/tests/test_order_book.cpp`](cpp/tests/test_order_book.cpp).

**1. `X` (Order Cancel) carries shares *cancelled*, not shares *remaining*.**
Read it as "remaining" and every partial cancel silently inflates or deflates the
level. The order also stays resting with its queue priority intact, so the level's
order count must not change unless the cancel takes it to zero.

**2. `C` (Executed With Price) must decrement the book at the order's *resting*
price, not at the price the trade printed at.** A `C` message exists precisely
because the two differ. Using the execution price to locate the level reads
naturally and corrupts the book.

**3. `U` (Order Replace) changes the order reference.** It is an atomic
delete-then-add in which the old reference is retired permanently and queue
priority is lost. Forget to erase the old reference and the order map leaks an
entry per replace — and replaces are ~12% of the feed, so the table grows
unbounded and lookups degrade for the rest of the session. `U` also carries no
buy/sell indicator, so the side has to be read from the original entry *before*
it is erased.

**4. `P` (Trade, non-cross) is an execution against a *hidden* order.** There is
no book entry to decrement, because the liquidity was never displayed. Touching
the book here double-counts, and this is the most common reconstruction bug.

**5. A level going empty is not the same as a level not existing.** Removing
shares from an in-window price that holds no liquidity means a message was
missed — a feed gap. Attributing it to arithmetic (a "negative quantity") blames
the wrong thing and hides the gap. `nanobook` counts the two separately, and
finding that distinction missing is what
[one of these tests](cpp/tests/test_price_ladder.cpp) was written to catch.

---

## Design

### Zero-copy decode

The feed is `mmap`ed and messages are handed to the handler as **views** — a
pointer into the page cache plus typed accessors. A 12 GB session file is never
copied into userspace buffers, and resident memory stays flat because
`MADV_SEQUENTIAL` has the kernel prefetch ahead and drop pages behind.

ITCH is big-endian with unaligned fields (the 64-bit order reference sits at
offset 11). `reinterpret_cast` into the buffer would be undefined behaviour twice
over — unaligned load plus strict-aliasing violation — so every field decode is
`memcpy` into a local followed by a byte swap, which clang folds into a single
unaligned load plus one `rev` instruction at `-O2`.

### Static dispatch, not virtual

The parser is templated on its handler. The dispatch switch runs once per
message — 300M times a day — and an abstract base class would cost an indirect
call the branch predictor cannot resolve and the inliner cannot see through.
Templating lets the compiler inline the book update directly into the switch arm.
Unused hooks on `HandlerBase` are no-ops that compile away entirely.

### Price ladder, not a tree

`std::map<Price, Level>` is the textbook answer and the wrong one. Real equity
books are extremely narrow — an S&P name spends the whole session inside a few
dollars — so the price axis can just be an array:

```
index i  <->  price  base + i * tick
```

Add and cancel become an array store at a computed index: O(1), one cache line,
no allocation. The cost is memory proportional to the price *range* rather than
the live level count, which for a 65,536-slot penny ladder is 1.5 MB — and the
working set actually touched is the few hundred cents around the touch, so it
stays resident in L1/L2.

Tick size is 100 (one cent in ITCH's 1/10000 units), which is correct for
essentially every name: SEC Reg NMS Rule 612 forbids displaying sub-penny quotes
for stocks at or above $1.00. Sub-dollar names need `--tick 1`; off-tick prices
are **counted and reported, never rounded**, because rounding a price is how a
backtest invents free money.

### Hierarchical occupancy bitmap

When the best bid is consumed, a naive ladder walks downward looking for the next
non-empty level. That is usually 1–2 steps — but when a large order sweeps
several levels, which is exactly the moment a latency-sensitive strategy cares
about, it degenerates into a scan over hundreds of empty cents. Tail latency is
what gets you picked off, so the worst case is the one that matters.

A two-level bitmap fixes it. For a 65,536-slot ladder:

```
L0: 1024 words, one bit per slot       8 KB  — fits in L1d
L1:   16 words, one bit per L0 word  128 B  — two cache lines
```

Best-price lookup becomes: scan ≤16 summary words, one CTZ/CLZ to pick the L0
word, one more to pick the bit. Three dependent operations, no data-dependent
branching on ladder contents.

### Open-addressing order map

Flat array of **16-byte entries** — four per cache line — with linear probing.
Side is packed into the top bit of the price word, which is safe because ITCH
prices are `uint32` with 4 implied decimals and every real equity price fits in
31 bits; the invariant is asserted on insert rather than assumed.

Two details that matter more than the layout:

- **splitmix64 finalizer, not identity hashing.** Order references arrive
  near-sequentially. Identity hashing is fine for insertion order, but deletions
  leave the live set clustered in strided runs, which linear probing handles
  badly. Three multiplies and three shifts flatten the probe distribution.
- **Backward-shift deletion, not tombstones.** Tombstones mean probe sequences
  never shorten, so a session of adds and deletes degrades the table toward a
  linear scan. Shifting the tail of the probe run into the hole keeps the table
  tombstone-free forever. There is a
  [test for exactly this](cpp/tests/test_order_map.cpp): 160,000 churn
  operations at a stable live-set size, asserting the longest probe run stays
  bounded.

On the 10M-message feed (242k live orders at the close) the table settles at
**1.256 mean probes per lookup** with a longest run of 25 at load factor 0.46.

---

## Measurement honesty

The headline number is measured with one clock read either side of a
10M-message replay, so instrument overhead is nil and the figure is trustworthy.

Per-message latency is a different story, and this is the central measurement
fact of the project. Probed on this machine (`docs/measurement.md`):

```
mach timebase                      125/3  =>  1 tick = 41.667 ns  (24 MHz)
clock_gettime_nsec_np(UPTIME_RAW)  41.67 ns resolution, ~16.1 ns/call
clock_gettime_nsec_np(MONO_RAW)    41.67 ns resolution, ~11.8 ns/call
mach_absolute_time()               41.67 ns resolution,  ~6.1 ns/call
```

One tick is 41.67 ns. Processing one message costs ~13 ns. **55–85% of
back-to-back clock reads return the same value.** Per-message latency on Apple
Silicon is below the resolution of any clock userspace can reach — the cycle
counter (`PMCCNTR_EL0`) needs kernel or entitled access, unlike x86 where `RDTSC`
is unprivileged.

So `nanobook` does not report a per-message p50, because it cannot honestly
measure one. It reports the distribution across 256-message blocks instead: each
block spans ~88 ticks, comfortably above the granularity, so the spread is real
and the upper tail surfaces the stalls that matter — page faults on first touch
of the `mmap`, order-map rehashes, ladder regrows. It cannot attribute a stall to
a single message, and it does not claim to.

---

## Verification

Three layers, because performance claims about a wrong book are worthless.

**Differential testing against an independent oracle.** `gen_itch` maintains a
`std::map`-based shadow book — the slow, transparently correct implementation —
and writes top-of-book after every message. `replay --truth` compares the fast
reconstruction against it event by event. Two implementations that disagree
localise a bug to one of them; a fast implementation alone can only be checked
against your own assumptions.

**Independent encoder and decoder.** `itch_writer.hpp` and `itch_spec.hpp` are
separate transcriptions of the NASDAQ spec. The round-trip tests encode with one
and decode with the other, so a wrong field offset in either fails the test. A
single shared offset constant would have made both wrong together and the test
vacuous.

**Property and differential unit tests.** `BitsetIndex` against `std::set`,
`OrderMap` against `std::unordered_map`, `PriceLadder` against `std::map` —
across sizes chosen to straddle every word and summary-word boundary, all under
AddressSanitizer and UndefinedBehaviorSanitizer.

```
58 tests in 6 suites, 186,913 assertions, 0 failures
```

Interleaved decoy symbols in the generated feed are deliberate: single-symbol
test data silently passes a book that ignores the locate filter entirely.

---

## Layout

```
cpp/include/nanobook/
  byte_order.hpp     unaligned big-endian loads/stores
  itch_spec.hpp      ITCH 5.0 wire format — message layouts, zero-copy views
  itch_writer.hpp    encoders (independent transcription; used by tests + generator)
  itch_parser.hpp    mmap + framing + templated dispatch
  bitset_index.hpp   two-level occupancy bitmap
  price_ladder.hpp   dense price-indexed book side
  order_map.hpp      open-addressing order-reference table
  order_book.hpp     the ITCH state machine
  book_builder.hpp   parser -> book glue, locate-based symbol filtering
  latency.hpp        clock, histogram, block timer
cpp/tools/
  gen_itch.cpp       synthetic feed + ground-truth oracle
  replay.cpp         reconstruct, verify, report
  bench_structures.cpp  head-to-head vs the standard library
cpp/tests/           58 tests, zero dependencies
```

The library is header-only; `make tools` and `make tests` are the only build
steps. `CMakeLists.txt` drives the same sources for IDE and CI use.

---

## Roadmap

Stage 1 — the feed handler and book above — is complete and verified. Next:

- [ ] **`pybind11` bindings** exposing the book as a streaming event iterator, so
      research runs on the exact reconstructed state rather than a re-derivation.
- [ ] **Microstructure features** computed in C++ on the event stream: order-flow
      imbalance (Cont–Kukanov–Stoikov), queue imbalance, micro-price, depth
      profile, trade sign autocorrelation.
- [ ] **L3 queue position** — per-level intrusive FIFO order lists, so
      fill-probability given queue rank is measurable rather than assumed.
- [ ] **Short-horizon prediction study** with honest statistics: purged
      walk-forward CV, Newey–West HAC standard errors, deflated Sharpe against
      the number of configurations actually tried.
- [ ] **Transaction-cost model** grounded in reconstructed queue position, since a
      microstructure signal that ignores the spread it must cross is not a signal.

---

## Non-goals

- **This is not a matching engine.** It reconstructs an exchange's book from a
  public feed; it does not match orders or maintain its own.
- **No multicast / MoldUDP64 transport.** Framing for the raw stream is
  implemented and tested (`--raw`), but gap recovery and retransmission are not.
- **Single-threaded by design.** Feed handling is inherently sequential — the book
  is a state machine over an ordered stream — and 77 M msg/s on one core is past
  the point where threading the decode would help. Parallelism belongs one level
  up, across symbols or across session-days.

## References

- Nasdaq, *TotalView-ITCH 5.0 Specification*, 2023-07 revision.
- SEC, *Regulation NMS Rule 612* (sub-penny quoting increments).
- Cont, Kukanov & Stoikov, *The Price Impact of Order Book Events*, 2014.
- Gould et al., *Limit Order Books*, Quantitative Finance 13(11), 2013.
