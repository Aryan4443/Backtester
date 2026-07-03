# Design Notes — Deterministic Backtester & Execution Simulator

Keep this file open during interviews. Every choice here has a "why."

---

## 1. Core determinism guarantee

**Claim:** same input → identical output, every run.

**How it is enforced:**

| Requirement | Implementation |
|---|---|
| No wall-clock time | `SimClock` is the only clock. It advances only when `advance(ts)` is called by the event loop. `std::chrono::now()` is never called anywhere. |
| No nondeterministic iteration order | `std::map` everywhere in decision paths (bid/ask book, queue positions). `std::unordered_map` is banned from any path that affects fills or orders. |
| Reproducible randomness | `synth_gen.cpp` seeds `std::mt19937_64` with a fixed or user-supplied seed. No `std::rand()`. |
| Total event ordering | `(timestamp_ns, seq, det_id)` — three keys guarantee a strict weak ordering with no ties. |
| Reproducible order IDs | `ExecutionSimulator::next_order_id_` is a monotone counter reset to 1 at construction. The strategy never assigns IDs. |

**Why determinism matters for a backtester:**  
If two runs on the same data produce different results, you cannot tell whether a strategy improvement is real or noise. Non-determinism also makes debugging impossible — you cannot reproduce a bad fill to understand why it happened.

---

## 2. Event ordering: `(timestamp_ns, seq, det_id)`

**timestamp_ns** — primary sort key. All simulation time is in nanoseconds.  
**seq** — the feed-sequence number embedded in the L2/L3 data stream. Two events with the same timestamp but different seq values are ordered by the feed's own ordering, which is the ground truth.  
**det_id** — assigned by `DeterministicEventQueue::push()` using a monotone counter. This breaks any remaining ties (e.g. two strategy orders submitted at the same sim tick). Without this, `std::priority_queue` can arbitrarily reorder equal elements.

**Data structure choice: `std::priority_queue`**  
A binary heap gives O(log n) push and pop. A calendar queue or van Emde Boas tree would be faster for dense timestamps, but adds complexity. For a backtester processing millions of events, `priority_queue` is sufficient; profile before optimising.

---

## 3. `SimBook`: `std::map` not `std::unordered_map`

**Why `std::map`:**
- Iteration over bid/ask levels is in sorted price order — essential for correctly walking the book during fills.
- Deterministic iteration order — `std::unordered_map` iteration order is implementation-defined and can vary between compilers, platforms, and even runs with address-space randomisation.
- The performance cost (log n vs amortised O(1)) is negligible at typical book depths (5–20 levels).

**Why `std::greater<Price>` for bids:**  
Bids should be iterated from highest to lowest price. `std::map<Price, Level, std::greater<Price>>` achieves this with zero additional logic; `begin()` always points to the best (highest) bid.

---

## 4. Latency model

**Assumption:** a strategy order submitted at sim-time `T` becomes active at the exchange at `T + L`, where `L` is a fixed configurable latency (default: 50 µs).

**Implementation:**  
`on_order_submit()` pushes an `ORDER_ARRIVE` event at timestamp `T + L` into `DeterministicEventQueue`. All market events with timestamps in `[T, T+L)` will have smaller timestamps and therefore fire before the `ORDER_ARRIVE` event. This naturally applies adverse selection: if the market moves against you during your network latency, those moves are already in the book when your order arrives.

**What this models well:** network round-trip, exchange processing delay.  
**What this does not model:** intra-exchange queue depth, co-location variability, feed handler jitter. These are acknowledged simplifications.

**Latency numbers to defend:**
- 50 µs: realistic for a co-located strategy on a modern exchange.
- 200–500 µs: realistic for a strategy not co-located.
- <10 µs: only achievable with FPGA or kernel-bypass networking.

---

## 5. Queue-position modeling

**On posting a passive limit order at price P:**  
`size_ahead = current total_qty at level P` in the sim book at the moment of arrival.

**Advancing the queue:**  
Every `MARKET_TRADE` at price P on the passive side decrements `size_ahead` by the trade quantity. When `size_ahead ≤ 0`, the order fills.

**Cancellation assumption: FIFO cancel (conservative)**  
When another order at our price level is cancelled, we do NOT advance our queue position. This is the conservative choice — it underestimates fill probability. The alternative (pro-rata cancel) would advance us, overestimating fill probability.

**Why FIFO cancel is the right default:**  
Most lit exchanges (NYSE, NASDAQ, LSE) use FIFO priority. Cancellations do not give you queue priority — only trades do. If the exchange uses pro-rata allocation, document this explicitly and switch the logic.

**Interview answer:**  
"I chose FIFO cancel because it's the realistic model for FIFO exchanges, and because it's conservative — if my strategy looks profitable under this assumption, it's more likely to be profitable in live trading than if I assumed pro-rata."

---

## 6. MarketView: structural look-ahead prevention

`MarketView` is a **value snapshot** constructed after the current event is applied, containing:
- Copied bid/ask levels (not references or pointers to the live `SimBook`)
- The timestamp `as_of_` set to `clock.now()`

The strategy receives `const MarketView&` and `const SimClock&`. There is no handle to the live `SimBook`, no handle to the event queue, and no way to read `SimClock` values beyond `clock.now()`.

**Why this prevents look-ahead by construction:**  
Even if a malicious strategy tried to read future data, it simply doesn't have the object. The only way to introduce look-ahead is through a global variable or a captured reference — both of which the parity harness detects by running two independent paths.

---

## 7. Parity harness

**Two paths:**
- **Backtest:** load all events → process in DeterministicEventQueue.
- **Live-replay:** identical code path, fresh strategy instance, same events.

**Hash:** FNV-1a 64-bit over `(order_id, fill_price, fill_qty, fill_ns)` tuples in fill order. FNV-1a is fast, non-cryptographic, and sufficient for detecting any divergence.

**What the harness catches that a normal backtest misses:**

| Bug class | How harness detects it |
|---|---|
| Look-ahead bias | Strategy reads future state in path A but not B → different fills → hash mismatch |
| Wall-clock leakage | `std::chrono::now()` returns different values in each path → different decisions → hash mismatch |
| Hash-map ordering | `unordered_map` iteration differs between paths on some platforms → hash mismatch |
| State mutation | Strategy state not reset between paths → second path sees polluted state → hash mismatch |
| Sequence number bugs | Events arrive in different orders → different fills if ordering logic is wrong |

**Why this matters more than unit tests alone:**  
A unit test checks one component in isolation. The parity harness checks the *composition* of all components under realistic load, which is where subtle interaction bugs live.

---

## 8. Execution quality metrics

**Why these matter more than raw PnL:**

- **Fill rate** (fills / orders submitted): a strategy that submits many orders but fills rarely is either too aggressive about cancelling or is posting limit orders that never get queue priority. High order-to-fill ratio is also suspicious — many real exchanges charge for excessive order messages.

- **Realized slippage**: the difference between fill price and mid-price at submit time. Positive slippage (you paid above mid to buy, received below mid to sell) is the true cost of immediacy. A strategy that looks profitable in PnL terms but has large slippage may be entirely explained by the sim having unrealistically tight slippage.

- **Latency cost**: the incremental slippage caused by the latency window `[T, T+L)`. If you reduce latency from 500 µs to 50 µs, how much does slippage improve? This tells you whether co-location is worth paying for.

---

## 9. Fixed-point prices (`int64_t` cents)

Prices are stored as `int64_t` in cents (e.g. `$100.50 = 10050`).

**Why not `double`:**
- Floating-point arithmetic is not associative: `(a + b) + c ≠ a + (b + c)` in general.
- Cross-platform float behaviour can differ in the last bit.
- Both break the determinism guarantee.

**Why cents and not ticks:**  
Ticks vary by instrument. Using cents gives a universal representation; tick-normalised analysis can be done in the report layer.

**Limitation:** this representation overflows for prices above ~$92 quadrillion, which is acceptable.

---

## 10. Directory layout rationale

```
include/backtester/  — public API headers (installed alongside library)
src/                 — implementation files (not exposed to consumers)
strategies/          — example strategies (header-only for simplicity)
tools/               — standalone executables (synth_gen)
tests/               — GoogleTest suite (one file per component)
```

Separating `include/` from `src/` allows the library to be consumed by other projects (e.g. a live trading system) without exposing implementation details. The strategy headers are separate from the core library so that adding a new strategy doesn't require recompiling the engine.

---

## 11. Things to add for production

- **Binary event format** (e.g. FlatBuffers or custom packed struct) instead of CSV — 10–100× faster parsing.
- **Persistent order map** for multi-leg strategies.
- **Slippage model calibration** against real execution reports.
- **Signal IC vs forward returns** — compute information coefficient of the strategy's signals against actual price moves N ticks forward.
- **Multiple instruments** — extend `SimBook` to a map of symbol → book.
- **Transaction costs** — exchange fees, rebates, clearing costs.
