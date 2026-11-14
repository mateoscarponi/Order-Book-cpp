# Order-Book-cpp

A limit order book and multithreaded matching engine in modern C++17.

It is the core piece of any trading venue or execution system: orders come in, the book matches buyers with sellers by **price-time priority**, and every match becomes a trade. On top of the single-instrument book, a matching engine runs many instruments in parallel across worker threads.

No external dependencies. It builds with CMake on Linux, macOS and Windows (GCC or Clang), and CI runs every test on Linux under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

## Features

- **Order types:** limit and market orders, with GTC (rest on the book) or IOC (cancel what does not fill right away).
- **Price-time priority:** the best price trades first; at the same price, the oldest order trades first.
- **Cancel and modify:** cancel by id in O(1). Modify keeps the order's place in the queue when it only shrinks, and sends it to the back when price changes or size grows, which is how real exchanges treat it.
- **Validation:** rejects zero quantity, non-positive limit prices, duplicate ids and unknown orders.
- **Market depth:** best bid/ask, volume at a price, and an N-level depth snapshot.
- **Multithreaded engine:** one book per symbol, each owned by exactly one worker thread. Orders can be handed over one by one or in batches.

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/replay examples/orders.csv      # replay a small trading session
./build/bench 2000000 16 8              # orders, symbols, worker threads
```

Sanitizer builds: add `-DOB_SANITIZE=address` (ASan + UBSan) or `-DOB_SANITIZE=thread` (TSan).

### Live demo in the browser

A small web page drives the real C++ book, so you can watch orders rest, queue up and match:

```bash
cmake -S . -B build && cmake --build build
python demo/server.py          # then open http://localhost:8000
```

`demo/server.py` (Python standard library only) starts `build/interactive`, a line-based front end to one `OrderBook`, and forwards each command from the page to it. Load the sample book, send an order that crosses the spread, click an order to cancel it, or turn on auto-flow to stream random orders. The page shows the ladder with each price level's queue in time order, the spread, and a live trade tape.

### Example

`examples/orders.csv` builds an AAPL book, then sends an aggressive buy, a market sell, an IOC, a cancel and a modify:

```
TRADE  AAPL  50 @ 150.05  (buy #6, sell #2)
TRADE  AAPL  75 @ 150.05  (buy #6, sell #3)
TRADE  AAPL  25 @ 150.10  (buy #6, sell #1)
TRADE  AAPL  200 @ 149.95  (buy #4, sell #7)
TRADE  AAPL  50 @ 149.90  (buy #5, sell #7)
TRADE  AAPL  75 @ 150.10  (buy #8, sell #1)
CANCEL AAPL #9
TRADE  MSFT  10 @ 412.50  (buy #1, sell #3)
REJECT MSFT #4 (invalid quantity)

AAPL
        BID            ASK
      50 @ 149.90      60 @ 150.15
```

Order #6 fills against #2 before #3 because both rest at 150.05 and #2 arrived first.

## Design

### The book

```
bids: std::map<Price, Level, std::greater<>>   best (highest) bid at begin()
asks: std::map<Price, Level, std::less<>>      best (lowest) ask at begin()
Level: std::list<Order> (FIFO) + total quantity at that price
index: std::unordered_map<OrderId, {side, price, list iterator}>
```

| Decision | Why |
|---|---|
| Prices as integer ticks (`int64_t`) | Floating-point prices do not compare exactly, and price levels are map keys. 150.25 is stored as 15025. |
| Sorted `std::map` per side | The best price is always `begin()`, and levels are created and removed in O(log L). |
| `std::list` inside each level | Iterators stay valid when other orders come and go, so the index can point straight at an order. |
| Hash index by order id | Cancel and modify find the order in O(1) instead of scanning the book. |
| Trades at the resting price | The order that was waiting sets the price; the incoming order gets any price improvement. |

Complexity: submit is O(log L + F) for L price levels and F fills, cancel is O(1) plus O(log L) if a level empties, best bid/ask is O(1).

### The engine: single writer per book

```
producer threads ──► queue[hash(symbol) % N] ──► worker thread ──► its own OrderBooks
```

Each symbol always goes to the same worker, and only that worker touches the symbol's books. So:

- **No locks on the matching path.** The only synchronization is the hand-off queue (mutex + condition variable).
- **Deterministic results per symbol.** Orders for one symbol are processed strictly in arrival order. A test checks that the engine produces exactly the same trades as a single-threaded replay of 200,000 orders.
- **Scales with symbols, not within one symbol.** One very busy symbol is still bound to one core, which is the same trade-off most real matching engines make.

The worker drains its whole queue in one swap, so it takes the lock once per batch, not once per order. Producers only wake a worker when its queue goes from empty to non-empty.

### What the benchmark showed

2,000,000 orders over 16 symbols (≈85% limit, ≈5% market, ≈10% cancels), Release build, 12-thread desktop CPU:

| Mode | Orders/sec |
|---|---|
| Single thread, books only | ~4.3 M |
| Engine, 4 workers, one order at a time | ~6.2 M |
| Engine, 8 workers, one order at a time | ~1.0 M |
| Engine, 8 workers, batches of 1024 | ~10.8 M |

The interesting row is the third one. Handing orders over one at a time stops scaling past a few workers: each worker empties its queue faster than one producer can fill it, so it keeps going to sleep, and nearly every order pays for a lock plus a thread wake-up. Batching hands over many orders per lock and per wake-up, and with 8 workers throughput reaches about 2.5x the single-threaded book. All modes produce the same 1,322,668 trades.

Numbers vary by machine; run `./build/bench` to measure yours.

## Tests

`tests/tests.cpp` (18 tests, built-in runner, no framework needed) covers:

- price priority, time priority, partial fills, sweeping several levels;
- market orders and IOC never resting;
- cancel, modify keeping or losing priority, modify that crosses the spread;
- rejects;
- a 50,000-order random flow checking that the book is never crossed and that level totals stay consistent;
- the engine matching a single-threaded replay trade by trade, batched submission, and four producer threads submitting at once.

## Project layout

```
include/orderbook/  types, OrderBook, MatchingEngine, BlockingQueue, workload generator
src/                OrderBook and MatchingEngine implementation
apps/replay.cpp     CSV replay tool
apps/bench.cpp      throughput benchmark
apps/interactive.cpp line-based front end used by the web demo
demo/               local web demo (server.py + index.html)
tests/tests.cpp     unit and concurrency tests
examples/           sample order file
```

## Possible next steps

- Replace `std::list` nodes with a preallocated pool to avoid heap allocation per order.
- A lock-free single-producer/single-consumer ring buffer for the hand-off.
- Latency percentiles (p50/p99) per order, not only throughput.
- A FIX-style text protocol front end.

## License

MIT License

Copyright (c) 2026 Mateo Gimenez Scarponi

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
