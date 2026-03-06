# Limit Order Book Engine

A high-performance matching engine that simulates how exchanges match buyers and sellers. Built for speed, transparency, and my portfolio.

## What It Does
- Processes limit orders (buy/sell at specific prices)
- Handles market orders (execute immediately at best price)
- Supports order cancellations
- Maintains price-time priority (fair queuing)
- Streams real-time order book updates

## Why I Built It
Every exchange runs on this logic. I wanted to understand what really happens under the hood when you click "buy" - and prove I can build low-latency systems from scratch.

## Tech Stack
- **C++** - No compromises on performance
- **Lock-free data structures** - No mutexes, no waiting
- **Memory pools** - Custom allocation, zero fragmentation
- **Latency benchmarking** - Every nanosecond counted

## Performance
Targeting 1-5 million orders per second with microsecond-level latency. The engine prints detailed stats so you can see exactly how it performs.
