# Limit Order Book Engine

> **15.15 M orders/sec · p50 = 20 ns · p99 = 60 ns · cancel = 20 ns**
> Single sandboxed core — Ryzen 5 6600H. No affinity pinning, no hugepages.

A production-grade C++20 matching engine built from scratch. Price-time priority, O(1) cancel, O(1) insert, slab pool allocator, IOC / FOK / GTC, full trade audit trail. Optimised across four engineering phases from 4.62 M → 15.15 M orders/sec — a **3.3× improvement** with no algorithmic shortcuts, only systems-level engineering.

---

## Benchmark Results

Tested on a single sandboxed core (Ryzen 5 6600H). Release build, `-O3 -march=native`.

```
+--------------------------------------------------------------------------+
|                         BENCHMARK SUMMARY                                |
|  Benchmark                              M ops/sec     ns/op              |
+--------------------------------------------------------------------------+
|  throughput (rest+match alternating)    15.15         66.0               |
|  cancel FIFO (head->tail)               48.69         20.5               |
|  cancel LIFO (tail->head)               34.34         29.1               |
|  cancel scattered (random price levels) 12.60         79.4               |
|  latency: resting (no match)             8.61        116.2               |
|  latency: matching (every order fills)  11.50         87.0               |
|  latency: IOC (partial fill + cancel)   17.34         57.7               |
|  latency: FOK pass (simulate + fill)    12.96         77.1               |
|  latency: FOK fail (simulate + reject)  19.85         50.4               |
+--------------------------------------------------------------------------+
```

**Latency percentiles (ns):**

| Benchmark | min | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| throughput | 0 | 20 | 60 | 80 | 1352 |
| resting | 10 | 100 | 430 | 3416 | 4568 |
| matching | 10 | 20 | 40 | 60 | 3626 |
| IOC | 0 | 10 | 20 | 20 | 60 |
| FOK pass | 20 | 20 | 30 | 90 | 3376 |
| FOK fail | 0 | 10 | 10 | 20 | 20 |

p50=20 ns and p99=60 ns on the throughput path reflect the true engine latency after removing `clock_gettime` from the hot path. Tail latency (max ~1.3–4.6 µs) is OS scheduler jitter on a non-isolated core.

---

## Engineering Progression

Four optimisation phases, each targeting one specific bottleneck. Every change benchmarked before the next one started.

| Phase | Change | Throughput | ns/op | Delta |
|---|---|---|---|---|
| Baseline | Original implementation | 4.62 M/s | ~216 ns | — |
| Phase 3 | Flat price array + flat `OrderIndex` | 6.30 M/s | 158.7 ns | −57 ns |
| Phase 4-1 | `SymbolId` — eliminate string hash per order | 6.62 M/s | 151.0 ns | −7.7 ns |
| Phase 4-2 | `TradeRing` — eliminate heap alloc in fill loop | 7.08 M/s | 141.3 ns | −9.7 ns |
| Phase 4-3 | Architecture split — `match()` inlined into `do_submit()` | 7.65 M/s | 130.7 ns | −10.6 ns |
| Phase 4-4 | RDTSC timing — replace `clock_gettime` on hot path | **15.15 M/s** | **66.0 ns** | −64.7 ns |

Each phase is a closed loop: identify the bottleneck, implement the fix, benchmark all 9 metrics, confirm improvement before proceeding.

---

## Architecture

```
MatchingEngine
│  Owns full order lifecycle: do_submit(), match(), make_exec(), cancel_order()
│  SymbolId array (O(1) lookup — no string hash on hot path)
│  TradeRing (64-slot pre-allocated fill buffer — zero heap alloc per fill)
│  RDTSC timing (~3 cycles vs ~23 ns for clock_gettime)
│
└── SymbolState[256]
    ├── unique_ptr<OrderBook>   (pure data — no logic)
    └── order_seq               (per-symbol sequence counter)
         │
         OrderBook (pure data)
         ├── PoolAllocator<Order>       (slab + free-list, ~5 ns alloc)
         ├── OrderIndex                 (flat array[1<<20], O(1) by id & mask)
         ├── BookSide<true>   bids
         └── BookSide<false>  asks
              │
              BookSide
              └── std::array<PriceLevel, 131072>
                  indexed by (price − base_tick)   — O(1) insert, O(1) best
                  active[] bool flags + best_idx_  — O(1) erase + advance
                   │
                  PriceLevel (64-byte cache line, intrusive doubly-linked list)
                  head ──▶ Order ──▶ Order ──▶ tail
```

`match()`, `make_exec()`, and `do_submit()` all live in `matching_engine.cpp`. Being in the same translation unit lets the compiler inline the entire order lifecycle — no cross-TU call overhead.

---

## Complexity

| Operation | Complexity | Implementation |
|---|---|---|
| Add order (no match) | **O(1)** | Flat array indexed by tick offset — no map |
| Add order (match) | O(F) | F = fills; best level at fixed `best_idx_` |
| Cancel | **O(1)** | Flat `OrderIndex` lookup → intrusive `unlink()` → pool free |
| Pool allocate | **~O(1)** | Free-list pop ~5 ns; slab alloc if list empty |
| Best bid / ask | **O(1)** | Direct `levels_[best_idx_]` — no tree traversal |
| Symbol lookup | **O(1)** | `SymbolId` array index — no string hash |
| Depth snapshot | O(N) | Walk N active levels |

---

## Key Design Decisions

**Flat price array — `std::array<PriceLevel, MAX_TICKS>`**
Each `BookSide` holds a pre-allocated array of 131 072 `PriceLevel` slots indexed by `(price − base_tick)`. Insert, cancel, and best-price access are all O(1) array operations. No pointer chasing, no tree rotations, no sorted vector. The best level is tracked by `best_idx_` — a single integer updated only when a level drains.

**Flat `OrderIndex` — `std::array<Locator, 1<<20>`**
Cancel and fill lookups index directly by `(id & ID_MASK)`. One array write on insert, one read on find, one pointer clear on erase. Replaced `std::unordered_map<OrderId, Locator>` — eliminated hash computation, bucket scan, and dynamic memory on every cancel.

**`SymbolId` — `uint16_t` array index**
`add_symbol()` returns a `SymbolId`. All hot-path methods (`submit_*`, `cancel_order`, queries) have `SymbolId` overloads that do a single bounds check + array slot read. The string-keyed `unordered_map` is only touched at registration time, never on the hot path.

**`TradeRing` — pre-allocated fill buffer**
`match()` writes fills directly into a 64-slot `TradeRing` owned by `MatchingEngine`. No heap allocation in the fill loop — every `push_back` that used to hit `malloc` is now an array store. The resting path (no fills) clears the ring with a single counter reset and never touches the heap.

**Architecture split — `OrderBook` as pure data**
`OrderBook` holds only `pool_`, `index_`, `bids_`, `asks_`. All matching logic (`match()`, `make_exec()`, `add_order` lifecycle) lives in `MatchingEngine`. Being in the same translation unit allows the compiler to inline the entire hot path into a single function body with no call overhead.

**RDTSC timing**
`do_submit()` brackets each order with `rdtsc()` instead of `Clock::now()`. RDTSC costs ~3 cycles (~1 ns). `clock_gettime(CLOCK_MONOTONIC)` costs ~23 ns. With two timer calls per order, the saving is ~40–46 ns — which explains the step from 130.7 ns to 66.0 ns. Calibrated against `steady_clock` once at startup via a 10 ms spin; stored as `ns_per_tick` for a multiply-based conversion.

**Cache-line-aligned `Order` — 2 × 64 bytes**
`Order` is laid out across exactly two 64-byte cache lines. Line 1 (hot): `prev`, `next`, `id`, `price`, `orig_qty`, `remaining_qty`, `filled_qty`, `side`, `type`, `tif`, `status`. Line 2 (cold): `seq`, `submit_ts`, `first_fill_ts`. The match loop only touches line 1.

**`PriceLevel` — one 64-byte cache line**
`head`, `tail`, `price`, `total_qty`, `order_count` fit in 40 bytes; 24 bytes of explicit padding complete the cache line. No padding is ever initialised in the hot path — `erase_level()` resets only the fields it uses.

**FOK simulate-only pass**
Fill-or-Kill pre-check runs a zero-state-change walk over the book using a stack-allocated probe order. If the probe cannot fill fully, the real order is never allocated and the book is never touched. The simulate path is also used for reject-path latency measurement — FOK fail is the fastest path in the engine at 50.4 ns mean.

**Integer tick prices — `int64_t`**
All prices stored as integer ticks (4 decimal places: 1 tick = $0.0001). Eliminates floating-point comparison hazards entirely. `PRICE(102.50)` macro handles conversion at the API boundary.

**Single-threaded by design**
No mutexes, no atomics, no lock-free overhead. For multi-symbol parallelism: shard by symbol — one `MatchingEngine` instance per core. This is the canonical HFT architecture. The `SymbolState` struct and pure-data `OrderBook` make per-symbol sharding trivial.

---

## Order Types

| Type | TIF | Behaviour |
|---|---|---|
| Limit | GTC | Rests in book until filled or cancelled |
| Limit | IOC | Fills immediately, cancels remainder |
| Limit | FOK | Fills entirely or rejected — book never touched on reject |
| Market | IOC | Sweeps book at any price, discards remainder |

---

## Project Structure

```
Limit-Orderbook/
├── include/
│   ├── order.hpp              # Primitives: Order, Trade, enums, PRICE()
│   ├── order_book.hpp         # PoolAllocator, OrderIndex, PriceLevel,
│   │                          #   BookSide, OrderBook (pure data), TradeRing,
│   │                          #   LatencyStats, rdtsc()
│   └── matching_engine.hpp    # MatchingEngine, SymbolId, EngineStats, OrderResult
├── src/
│   ├── orders.cpp             # Order::to_string, Trade::to_string
│   ├── order_book.cpp         # LatencyStats::compute(), queries, rdtsc_ns_per_tick()
│   └── matching_engine.cpp    # Full lifecycle: do_submit(), match(), make_exec()
├── benchmarks/
│   ├── bench_main.cpp         # Benchmark binary entry point
│   ├── bench_throughput.cpp   # Alternating rest + aggressive match
│   ├── bench_cancel.cpp       # FIFO / LIFO / scattered cancel
│   ├── bench_latency.cpp      # Per-path latency: resting, matching, IOC, FOK
│   └── bench_utils.hpp        # Timer, BenchResult, make_result()
├── demo/
│   └── main.cpp               # Correctness scenarios
├── CMakeLists.txt
└── README.md
```

**Include hierarchy — no circular dependencies:**
```
order.hpp  ←  order_book.hpp  ←  matching_engine.hpp
```

---

## Build

Requires: **CMake ≥ 3.20**, **GCC ≥ 12** or **Clang ≥ 15**, C++20, x86-64.

```bash
git clone https://github.com/Not-Human07/Limit-Orderbook.git
cd Limit-Orderbook

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
cd ..
```

**Run correctness demo:**
```bash
./build/demo
```

**Run full benchmark suite:**
```bash
./build/benchmarks
```

> Always benchmark `Release` builds. Debug builds with ASan/UBSan are ≈10× slower by design.

---

## Roadmap

- [ ] Per-symbol thread sharding — one `MatchingEngine` per core, no locks needed (architecture already supports it)
- [ ] LTO (`-flto`) — enables cross-TU inlining for remaining call boundaries
- [ ] Hugepage-backed `PoolAllocator` — eliminates TLB misses on the 8 MB `BookSide` arrays
- [ ] SPSC lock-free queue between feed handler and engine — realistic production architecture
- [ ] Order modify — price / qty amendment without cancel-replace round-trip
- [ ] `perf`-annotated flame graph in README
- [ ] Market data replay — drive the engine with a real order flow trace

---

## About

Built as a portfolio project demonstrating HFT-grade systems engineering in C++20. Every design decision is motivated by real exchange architecture: integer prices, slab allocation, intrusive data structures, flat array price levels, RDTSC timing, and single-threaded sharding are all patterns used in production matching engines.

The 3.3× throughput improvement from 4.62 M to 15.15 M orders/sec was achieved purely through systems engineering — better data structures, eliminating allocations, compiler-visible inlining, and removing syscall overhead from the hot path. No algorithmic changes, no SIMD, no hardware tricks beyond a single `rdtsc` instruction.

**License:** MIT
