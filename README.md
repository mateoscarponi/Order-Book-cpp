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

`demo/server.py` (Python standard library only) starts `build/interactive`, a line-based front end to one `OrderBook`, and forwards each command from the page to it. Load the sample book, send an order that crosses the spread, click an order to cancel it, or turn on auto-flow to stream random orders, with a slider for the mix of buyers and sellers. The page shows the ladder with each price level's queue in time order, the spread, and a live trade tape.

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

## Author's note: watching supply and demand happen

Every economics class teaches the same rule: when many people want to buy and few want to sell, the price goes up, and when everyone is selling and few are buying, the price goes down. I knew the rule. Building this order book let me watch it happen, one order at a time.

The book is a picture of supply and demand. The **sell orders (asks) are the supply**: everyone willing to sell, and their minimum price. The **buy orders (bids) are the demand**: everyone willing to buy, and their maximum price. The price is simply wherever the two sides meet.

**Experiment 1: a wave of buyers.** I loaded the demo's sample book (best price to buy: 150.05) and sent eight market buy orders of 100 shares in a row. A market order means "buy now, at whatever price there is."

| Buy order | Price paid for the last share | Shares still for sale |
|---|---|---|
| 1st | 150.05 | 525 |
| 3rd | 150.15 | 325 |
| 5th | 150.20 | 125 |
| 6th | 150.25 | 25 |
| 7th | 150.25 (only 25 filled) | 0 |
| 8th | nothing filled | 0 |

Each buyer used up the cheapest sellers, so the next buyer had to pay the next price up. The price climbed from 150.05 to 150.25 without anyone "deciding" to raise it. It rose because supply ran out. The 7th buyer could only get 25 shares, and the 8th got nothing: with no sellers left there is no price at all, only buyers waiting.

**Experiment 2: a wave of sellers.** The mirror image. Eight market sells of 100 pushed the price down step by step, from 149.95 to 149.75, until no buyers were left.

**Experiment 3: a market that leans one way.** The demo's auto-flow sends random orders. I ran it 20 times, 500 orders each, with three different mixes of buyers and sellers:

| Mix | Typical final price (start: 150.00) |
|---|---|
| 50% buyers, 50% sellers | 150.01 (it wanders, but stays put) |
| 60% buyers | 150.17 (range 150.09 to 150.30) |
| 60% sellers | 149.85 (range 149.69 to 149.95) |

A small tilt is enough. Nobody in the simulation knows anything about the "value" of the stock. A few more buyers than sellers, sustained over time, steadily pushes the price up, and the opposite pushes it down. That is supply and demand in its simplest form.

**What this taught me about real trading.** The first share of a big order is cheap and the last one is expensive. In Experiment 1, buying 625 shares at market cost an average of 150.14 per share, not the 150.05 shown on screen. Traders call this gap *slippage*, and avoiding it is a big reason execution software exists: it splits large orders into small pieces over time, so they do not eat through the book all at once.

You can repeat all three experiments in the [live demo](#live-demo-in-the-browser): load the sample book, then send market orders or move the buyers/sellers slider with auto-flow on.

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
