# Limit Order Book Engine

> **4.62 M orders/sec · p50 = 173 ns · p99 = 587 ns · cancel = 32 ns avg**
> Single sandboxed core — Ryzen 5 6600H. Bare-metal expected: **7–10 M orders/sec**.

A production-grade C++20 matching engine built from scratch. Price-time priority, O(1) cancel, slab pool allocator, IOC / FOK / GTC, full trade audit trail. Written as a proof-of-ability for quant dev / HFT roles.

---

## Benchmark Results

Tested on a single sandboxed core (Ryzen 5 6600H, no affinity pinning, no hugepages).

```
=== Limit Order Book Benchmark ===

[throughput]  300,000 orders  →  4.62 M orders/sec
[cancel]      50,000 cancels  →  32 ns avg
[latency]     min=112  p50=173  p99=587  p99.9=1924  max=38255  ns
```

| Metric | Value | Notes |
|--------|-------|-------|
| Throughput | **4.62 M orders/sec** | Alternating rest + aggressive match |
| p50 latency | **173 ns** | Median per-order, hot path |
| p99 latency | **587 ns** | 99th percentile |
| p99.9 latency | **1924 ns** | Tail — dominated by OS scheduler jitter |
| Cancel avg | **32 ns** | O(1) intrusive-list pointer surgery |
| Min latency | **112 ns** | Pool hit + zero match |

Session-over-session improvement after struct layout optimisation (removing `std::string trader_id` from `Order`):

| Metric | Before | After | Δ |
|--------|--------|-------|---|
| Throughput | 3.85 M/sec | 4.62 M/sec | **+20%** |
| p50 latency | 218 ns | 173 ns | **−21%** |
| p99 latency | 787 ns | 587 ns | **−25%** |

---

## Architecture

```
                        MatchingEngine
                        (symbol → OrderBook map)
                               │
                    ┌──────────▼──────────┐
                    │      OrderBook       │
                    │  seq, trade_seq      │
                    │  LatencyStats        │
                    └──────┬──────┬───────┘
                           │      │
              ┌────────────▼─┐  ┌─▼────────────┐
              │  BookSide    │  │  BookSide     │
              │  bids        │  │  asks         │
              │  (greater<>) │  │  (less<>)     │
              └──────┬───────┘  └───────┬───────┘
                     │                  │
              ┌──────▼──────────────────▼──────┐
              │         std::map<Price,         │
              │           PriceLevel>           │
              │   best level always at begin()  │
              └──────────────┬─────────────────┘
                             │
              ┌──────────────▼─────────────────┐
              │           PriceLevel            │
              │   intrusive doubly-linked list  │
              │   head ──▶ Order ──▶ Order ──▶ tail
              └──────────────┬─────────────────┘
                             │
              ┌──────────────▼─────────────────┐
              │        PoolAllocator<Order>     │
              │   slab vector + free-list       │
              │   alloc ≈ 5 ns  (free-list pop) │
              └─────────────────────────────────┘
```

**`OrderIndex`** — shared `unordered_map<OrderId, Locator>` pre-reserved to `pool_size` buckets. Gives O(1) lookup of `(PriceLevel*, Order*)` for cancel and routing.

---

## Complexity

| Operation | Complexity | Implementation |
|-----------|-----------|----------------|
| Add order (no match) | O(log P) | `std::map::try_emplace` — P = distinct price levels |
| Add order (match) | O(F) | F = number of fills; best level always at `begin()` |
| Cancel | **O(1)** | `OrderIndex` lookup → intrusive `unlink()` → pool free |
| Pool allocate | **~O(1)** | Free-list pop ≈ 5 ns; no `new`/`delete` in hot path |
| Best bid / ask | O(1) | `map::begin()` — always the best price |
| Depth snapshot | O(N) | Walk N levels of the map |

---

## Key Design Decisions

**Integer tick prices (`int64_t`)**
All prices stored as integer ticks (4 decimal places: 1 tick = $0.0001). Eliminates floating-point comparison hazards entirely — no epsilon checks, no rounding drift. `PRICE(102.50)` macro handles conversion at the boundary.

**Intrusive doubly-linked list inside `Order`**
`prev`/`next` pointers live directly in the `Order` struct. Cancel is pointer surgery — no `deque::erase`, no O(n) scan. Unlink is three pointer assignments regardless of queue depth.

**Slab pool allocator**
`PoolAllocator<Order>` pre-allocates a contiguous slab. Allocation = free-list pop ≈ 5 ns. No `new`/`delete` in the matching hot path. No heap fragmentation across a benchmark run.

**FOK simulate-only pass**
Fill-or-Kill pre-check runs a zero-state-change pass over the book using a stack-allocated probe order. If the probe can't fill fully, the real order is never allocated and the book is never touched.

**Single-threaded by design**
No mutexes, no atomics, no lock-free overhead. For multi-symbol parallelism: shard by symbol, one engine instance per core. This is the canonical HFT architecture.

**Layered include hierarchy**
```
order.hpp               ← primitives, Order, Trade, enums
    ↑
order_book.hpp          ← PoolAllocator, PriceLevel, BookSide, OrderBook
    ↑
matching_engine.hpp     ← MatchingEngine, EngineStats, OrderResult
```
Each layer includes only the one below it. No circular dependencies.

---

## Project Structure

```
Limit-Orderbook/
├── include/
│   ├── order.hpp               # Primitives: Order, Trade, enums, PRICE()
│   ├── order_book.hpp          # PoolAllocator, PriceLevel, BookSide, OrderBook
│   └── matching_engine.hpp     # MatchingEngine, EngineStats, OrderResult
├── src/
│   ├── orders.cpp              # Order::to_string, Trade::to_string
│   ├── order_book.cpp          # Matching engine core
│   └── matching_engine.cpp     # MatchingEngine implementation
├── demo/
│   └── main.cpp                # Correctness scenarios (limit, market, IOC, FOK, cancel)
├── bench/
│   ├── bench_main.cpp          # Dedicated benchmark binary (no I/O in hot path)
│   └── results/                # Timestamped benchmark output files (gitignored)
├── CMakeLists.txt
└── README.md
```

---

## Build

Requires: **CMake ≥ 3.20**, **GCC ≥ 12** or **Clang ≥ 15**, C++20 support.

```bash
git clone https://github.com/Not-Human07/Limit-Orderbook.git
cd Limit-Orderbook

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
cd ..
```

**Run correctness demo first:**
```bash
./build/demo
```

**Run benchmark:**
```bash
./build/bench 2>&1 | tee bench/results/$(date +%Y%m%d_%H%M%S).txt
```

> Always benchmark `Release` builds. Debug builds with ASan/UBSan are ≈10× slower by design.

---

## Order Types Supported

| Type | TIF | Behaviour |
|------|-----|-----------|
| Limit | GTC | Rests in book until filled or cancelled |
| Limit | IOC | Fills what it can immediately, cancels remainder |
| Limit | FOK | Fills entirely or is rejected — book never touched on reject |
| Market | IOC | Sweeps book at any price, discards unfilled remainder |

---

## Roadmap

- [ ] Replace `std::map` price levels with a flat sorted array — eliminates pointer-chasing, expected +40% throughput
- [ ] SPSC lock-free queue between feed handler and engine — simulates realistic production architecture
- [ ] `perf`-annotated flame graph in README
- [ ] Google Benchmark integration for statistical output
- [ ] Order modify (price / qty amendment without cancel-replace)

---

## About

Built as a portfolio project demonstrating HFT-grade systems engineering in C++20. Every design decision is motivated by real exchange architecture — integer prices, pool allocation, intrusive data structures, and single-threaded sharding are all patterns used in production matching engines.

**License:** MIT
