# nanobook

[![ci](https://github.com/TheRealKwabena/nanobook/actions/workflows/ci.yml/badge.svg)](https://github.com/TheRealKwabena/nanobook/actions/workflows/ci.yml)

Order book reconstruction from raw exchange feeds in C++20, built to be **fast
enough to matter** and **verified well enough to trust**. Two protocols:

- **NASDAQ TotalView-ITCH 5.0** — full order-by-order (L3) reconstruction at
  77 M messages/sec, verified byte-exact against an independent oracle.
- **IEX DEEP 1.0** — aggregated (L2) reconstruction straight from real exchange
  captures, which IEX publishes **free on a T+1 basis**. That makes a *daily*
  pipeline possible without a market-data budget.

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
91 C++ tests + 20 research tests, clean under ASan + UBSan
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

## The daily loop

```bash
./nanobook daily        # fetch the latest IEX day, score it out-of-sample, print the brief
```

Three commands, and the order is enforced: **score before retraining**, or every
number becomes in-sample.

```
./nanobook fetch        stream a day of IEX DEEP into data/features/
./nanobook research     score unscored days with a model that predates them, then retrain
./nanobook brief        print the morning brief
```

Because IEX publishes T+1 and a model file for day D is written before day D+1
exists, predictions are out-of-sample in the strong sense — made before the outcome
was knowable, not merely held out from a shuffle. The scorecard is append-only and
records which model file made each prediction and that model's training
fingerprint, so the ordering is auditable rather than asserted.

### Automating it

```bash
make install-daily      # launchd, 04:10 nightly; make uninstall-daily to stop
make status-daily       # installed? last exit? last run's brief?
```

Opt-in on purpose: each run pulls 11-12 GB. The job walks back to the newest
published session, so weekends, holidays and a missed night all take care of
themselves.

### What it says today

Five out-of-sample days, ten symbols, 285,750 one-second bars of real IEX DEEP:

```
  date           rows       IC    hit     gross       net   t(net)   spread
  20191219     56,679  +0.1330  0.566    +0.072    -1.403   -56.10    2.95b
  20191220     63,240  +0.1341  0.573    +0.115    -1.254   -56.53    2.74b
  20191223     54,429  +0.1231  0.565    +0.093    -1.250   -50.75    2.69b
  20191226     54,750  +0.1411  0.580    +0.156    -1.389   -50.00    3.09b
  20191227     56,652  +0.1646  0.582    +0.124    -1.262   -51.78    2.77b

  information coefficient   +0.1392     (reversal baseline +0.1140)
  gross / net per bar       +0.112 bp  /  -1.310 bp
  mean quoted spread        2.84 bp
  predictions beating cost  1.6%

  No tradeable edge. Net of 2.8 bp of spread the signal loses 1.31 bp per bar.
    The direction is informative (IC +0.1392) but the moves it predicts are
    smaller than the cost of trading them.
```

**Book imbalance genuinely predicts the direction of the next ten seconds** — IC
+0.14, 58% directional accuracy, stable across every day, gross returns positive at
t = +7 to +13. And it is **not tradeable**: the moves it predicts average 0.11 bp
while crossing the spread costs 1.4 bp, so only 1.6% of predictions are even large
enough to pay for themselves.

That is the honest answer, it is what the microstructure literature finds at this
horizon, and it is the answer this pipeline is built to be *able* to give. A
research loop that can only report success is not measuring anything.

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

## IEX DEEP: a daily pipeline on free real data

ITCH work runs on one-off historical samples, because real TotalView costs
thousands a month. IEX Exchange publishes its full depth-of-book feed for free
with a one-day lag, so `nanobook` also speaks DEEP — and that turns the project
from a static backtest into something that accumulates.

```bash
make tools
python3 scripts/fetch_day.py --date latest --symbols SPY,AAPL,MSFT,NVDA,TSLA
```

Verified against the real feed for 2026-09-11 — 460,930 messages decoded, zero
transport defects, zero crossed books, clean under ASan and UBSan:

```
IEX-TP
  segments            400000      sequence gaps        0 (0 messages missing)
  messages            460930      sequence regressions 0
  heartbeats            2716      unknown msg types    0

books  (system event 'S')
  symbol      updates       txns   in-transit   trades  crossed    bad px
  AAPL            667        549          118       49        0         0
  MSFT            811        550          261       35        0         0
  SPY           15349      15348            1       39        0         0

  transport defects   0  (clean)
```

### The 12 GB problem, and why nothing touches disk

A single day of IEX DEEP is **11-12 GB gzipped**. Downloading that daily to keep a
few megabytes of features would be absurd, so the pipeline never lands it:

```
curl -sL "$URL" | gunzip | iex_replay --pcap - --symbols ... | gzip > day.csv.gz
```

Peak disk usage is the output. Peak memory is one 1 MiB stream buffer plus one
ladder per watched symbol. This is why `stream_buffer.hpp` exists alongside the
`mmap` path: a decoder written against `peek`/`consume` works on a pipe and on a
mapped file, and the ITCH side still gets the zero-copy benefit of `mmap` where
the file is already local.

### What DEEP peels through

A HIST download is a raw network capture, so there are four wrappers before
anything tradeable:

```
pcap-ng file
  └─ Enhanced Packet Block
       └─ Ethernet (+ VLAN tags)
            └─ IPv4 (+ options) / UDP
                 └─ IEX-TP segment (40-byte header, sequence numbers)
                      └─ [2-byte length][DEEP message]  x N
```

Every layer is validated rather than assumed. Two that bite:

- **The files are pcap-ng, not classic pcap** — despite being named `*.pcap.gz`
  and despite the spec saying "PCAP or PCAP-NG". They begin with `0x0A0D0D0A` and
  carry an option reading "File created by merging:", because IEX merges its A and
  B multicast captures. A classic-pcap-only reader rejects every real file. Both
  formats are supported and both are tested.
- **IPv4 header length is read, not assumed.** IP options are rare, but assuming
  20 bytes shifts every later field and decodes as plausible garbage.

IEX-TP carries sequence numbers, so gaps are detectable — and a book rebuilt
across a gap is wrong with no other symptom. Gaps, replays and session changes are
counted, and `iex_replay` exits nonzero when any defect is nonzero.

### Four ways DEEP differs from ITCH

| | ITCH 5.0 | IEX DEEP 1.0 |
|---|---|---|
| Byte order | big-endian | **little-endian** |
| Price field | `uint32` | **signed `int64`** |
| Update model | deltas (add / cancel / execute) | **absolute level size** |
| Book validity | every message | **only at transaction boundaries** |
| Depth | order-by-order (L3) | aggregated (L2) |
| Symbol key | `uint16` locate | 8-byte ticker |

The last two rows are the ones that produce silent corruption:

**Absolute, not delta.** A Price Level Update carries the aggregate size at that
price *after* the update; size 0 removes the level. Routing that through ITCH's
delta logic diverges from the real book within seconds. This is what
`PriceLadder::set_level` exists for, and there is a
[test](cpp/tests/test_price_ladder.cpp) asserting it replaces rather than
accumulates.

**The book is only valid at transaction boundaries.** One order book event may
change several price levels at once. DEEP describes that as a run of updates with
the event flag OFF terminated by one with it ON, and the spec is explicit that the
book keeps its previous BBO throughout — an intermediate BBO "never truly
existed". So `DeepBook` maintains two things: the ladders, updated on every
message, and `stable_top()`, the BBO as of the last *completed* transaction, which
is the only one a feature may read.

This is the most dangerous bug in the codebase precisely because it makes results
*better*: sampling mid-transition manufactures spreads that gapped and mids that
jumped and came back, and a backtest will happily trade them. The `in-transit`
column in the output above is the count of updates that occurred inside a
multi-level transaction — 261 for MSFT in that run, all correctly suppressed.

DEEP also carries **no order count and no order references**, so there is no L3
and no queue position on this path. The ladder reports an order count of zero
rather than fabricating one, so a queue-position feature cannot silently compute
nonsense from it.

### Honest limits of IEX data

IEX is roughly **2-5% of US equity volume**. It is a real, complete view of one
venue's displayed book — excellent for microstructure research and for building a
daily habit — but it is not the consolidated market, and non-displayed orders and
reserve portions never appear in DEEP at all. Any result from it is a statement
about IEX, not about the NBBO.

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
91 tests in 9 suites, 187,114 assertions, 0 failures      (C++)
20 passed                                                 (research layer)
```

The IEX decoder gets a fourth layer: its tests decode the **worked examples
printed in the DEEP specification itself**, byte for byte. That is stronger than a
round-trip against our own encoder — those bytes came from the exchange's own
document, so agreeing with them means agreeing with IEX rather than with
ourselves.

Interleaved decoy symbols in the generated feed are deliberate: single-symbol
test data silently passes a book that ignores the locate filter entirely.

---

## Layout

```
cpp/include/nanobook/
  types.hpp          scalar types shared by both protocols
  byte_order.hpp     unaligned big- and little-endian loads/stores
  itch_spec.hpp      ITCH 5.0 wire format — message layouts, zero-copy views
  itch_writer.hpp    encoders (independent transcription; used by tests + generator)
  itch_parser.hpp    mmap + framing + templated dispatch
  bitset_index.hpp   two-level occupancy bitmap
  price_ladder.hpp   dense price-indexed book side
  order_map.hpp      open-addressing order-reference table
  order_book.hpp     the ITCH state machine
  book_builder.hpp   parser -> book glue, locate-based symbol filtering
  latency.hpp        clock, histogram, block timer
  stream_buffer.hpp  growable read-ahead buffer, so decoders work on a pipe
  pcap.hpp           classic pcap + pcap-ng container readers
  iex_spec.hpp       IEX DEEP 1.0 wire format
  iex_transport.hpp  IEX-TP framing, sequence-gap detection
  iex_book.hpp       DEEP book, atomic transactions, Lee-Ready classification
cpp/tools/
  gen_itch.cpp       synthetic feed + ground-truth oracle
  replay.cpp         reconstruct ITCH, verify, report
  iex_replay.cpp     reconstruct DEEP from a capture or a pipe
  bench_structures.cpp  head-to-head vs the standard library
python/nanobook/
  features.py        model matrix; time-based targets, trailing z-scores, filters
  model.py           walk-forward ridge, serialised for audit
  scoring.py         IC, HAC t-stats, cost-aware PnL, deflated Sharpe
  store.py           on-disk layout, append-only scorecard, trial registry
scripts/
  fetch_day.py       stream one day of IEX DEEP, store the feature table
  run_day.py         score out-of-sample, then retrain
  brief.py           the morning brief
  selftest_loop.py   the calibration pair (planted signal / pure noise)
nanobook             one entry point: fetch | research | brief | daily
cpp/tests/           91 tests, zero dependencies
tests/               20 research tests (pytest)
```

The library is header-only; `make tools` and `make tests` are the only build
steps. `CMakeLists.txt` drives the same sources for IDE and CI use.

---

## Roadmap

Stage 1 (ITCH feed handler and L3 book) and stage 2 (IEX DEEP, streaming
transport, daily pipeline) are complete and verified. Next:

- [x] **Daily brief and live scorecard** — `./nanobook daily`.
- [ ] **Accumulate 20+ scored days** so the scorecard stops being provisional.
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

## How the research layer avoids fooling itself

Every item below is here because the pipeline **did** fool itself first, and the
check is what caught it.

### The calibration pair

`scripts/selftest_loop.py` runs the real walk-forward loop twice: once on synthetic
data with a planted signal, once on pure noise.

```
=== loop on data WITH a planted signal ===
  out-of-sample IC: +0.829, +0.841, +0.799, +0.835   -> mean +0.8261  recovered
=== loop on PURE NOISE ===
  out-of-sample IC: +0.003, -0.017, -0.002, +0.007   -> mean -0.0022  found nothing
```

Both halves are necessary. A pipeline tuned until it finds nothing passes the null
test; one with a leak passes the recovery test. It runs in CI.

### Three leaks the null test caught

**Rows stamped with the grid boundary instead of the event time.** `iex_replay`
samples on a one-second grid and skips quiet intervals. Stamping a row with the
boundary it crossed meant that on a thinly quoted symbol a row could claim time T
while carrying book state from T+100s — so its features were newer than its own
timestamp, and any forward return measured from it was partly measuring the past.
That produced an IC of +0.24 and a hit rate of *0.40*, and the contradiction between
those two numbers is what gave it away. Fixed in `iex_book.hpp`, with
[a regression test](cpp/tests/test_iex_transport.cpp).

**A price level smuggled in as a feature.** `spread_bps` is `spread / mid`, which
for a name quoting a fixed number of ticks is nearly a deterministic function of the
*price level*. Regressing forward returns on a level along a single random-walk path
manufactures correlation from nothing — the classic integrated-variable regression
trap. On synthetic noise it reached Spearman −0.28 against the forward return and
the model reported IC +0.22 on data containing no signal at all. The feature is now
`spread_ticks`, which carries no level; basis points survive only as the *cost*,
never as an input.

**A test that passed for the wrong reason.** The signal-recovery test used to tilt
the book and move the mid on the same bar — contemporaneous, never predictive. It
"recovered" a signal anyway, through the level channel above. The planted state now
strictly leads the return.

### Statistical practice

- **Newey-West standard errors.** A 10-second target sampled every second shares 9
  seconds of path with its neighbour, so ordinary errors are far too small. The HAC
  correction shrinks the t-statistic on overlapped data by ~60% and leaves iid data
  untouched; both directions are tested.
- **Deflated Sharpe with a trial counter.** Every configuration ever scored is
  recorded in `data/trials.json`, so changing the horizon to get a better number
  increments the divisor. The best of 50 noise strategies over 20,000 bars shows a
  scaled Sharpe of 2.28 — that is the bar. (An early version reported 1.000 for
  everything because it was fed a √n-scaled Sharpe instead of a per-observation
  one; there is now a test pinning the units.)
- **Costs that can kill the signal**, and do.
- **Baselines.** Plain short-horizon reversal is scored alongside, and the brief
  says so out loud when the baseline wins.
- **Trailing-only standardisation.** Features are z-scored per symbol on a
  backward-looking window; sample-selection filters use an expanding median rather
  than the whole session's, because "small look-ahead" is not a category worth
  having.
- **Tradeability filters.** Bars with a stale book, a sub-round-lot touch, or a
  spread above 25 bp are dropped. Unfiltered, one December session had a 99th
  percentile spread of 2,331 bp and forward returns with a 232 bp standard
  deviation — artefacts of a near-empty single-venue book, not market moves.

### Data quality gates correctness

`iex_replay` exits nonzero on any transport defect — sequence gap, replayed
segment, unknown message type, truncated packet. A book rebuilt across a gap is
wrong with no other symptom, so the pipeline refuses to produce features rather
than producing quietly wrong ones.

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
