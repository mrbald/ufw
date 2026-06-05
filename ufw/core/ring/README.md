uFW core rings
==============

Lock-free, **single-producer** message-passing primitives over a fixed ring of
`trivially_copyable` records. One thread produces; one or many threads consume.
Pick the ring by your delivery needs; the fast paths are wait-free and the wait
policy (spin / yield / block) is always yours.

At a glance
-----------

| Ring                 | Consumers | Every consumer sees every record? | Producer blocks when a consumer is slow? | Slow consumer... |
|----------------------|-----------|-----------------------------------|------------------------------------------|------------------|
| `spsc_ring`          | exactly 1 | — (single consumer)               | **yes** — back-pressure, never drops     | stalls the producer |
| `multicast_channel`  | N (fixed) | **yes**                           | **yes** — gated by the *slowest*          | stalls the producer |
| `multicast_feed`     | N (dynamic) | yes, *if it keeps up*           | **no** — never waits, overwrites          | gets a gap report (`lapped`, `skipped`) |

Which one?
----------

- **One producer, one consumer, can't drop anything** → `spsc_ring`. The fastest
  path (~2 ns/op). This is your default for a point-to-point hand-off.
- **One producer fanning out to a known set of consumers, can't drop anything** →
  `multicast_channel`. Every subscriber sees the whole stream in order; the
  producer is held back by the slowest (bounded memory, real back-pressure). All
  subscribers must be created up front and all must keep draining, or the producer
  stalls.
- **One producer that must never wait** (a live feed, a market data tap, anything
  where stale-and-dropped beats blocking-the-source) → `multicast_feed`. The
  producer overwrites the oldest slot and runs at ~SPSC speed regardless of how
  many readers exist or how far behind they fall. A reader that gets lapped is
  *told* (`read_status::lapped` + how many records it skipped) and resynced to the
  oldest record still in the buffer. Readers can join at any time.

Rule of thumb: **loss-free ⇒ the producer must wait** (`spsc_ring`,
`multicast_channel`). **Never-wait ⇒ lossy** (`multicast_feed`). You can't have
bounded memory, never drop, *and* never block all at once — pick the two that
match the problem.

Design in one breath
--------------------

Everything shares one mechanism (`sequencer.hpp`):

1. The producer **claims** a range of monotonic positions and **publishes** a
   cursor (a release-store) once the data is written.
2. Each consumer reads up to the producer cursor (an acquire-load) and publishes
   **its own** cursor — its progress.
3. The producer is gated by the **completion floor**: the smallest position any
   consumer still needs. Below the floor, slots are free to reuse.

The *only* thing that differs between rings is **how the floor is computed** — a
`Gate`: one cursor for SPSC, a `min` over N cursors for multicast, or no gate at
all (the feed just overwrites and stamps each slot so readers can detect a lap).
The producer code is identical across rings (`ring_producer` in `slots.hpp`).

Scalar vs batch
---------------

Every ring has a one-at-a-time API and a batch API:

- **Scalar**: `try_push(v)` / `try_pop(out)` (and the zero-copy `try_claim` →
  fill → `commit`, `try_peek` → read → `release`). Use for latency-bound or
  low-rate paths.
- **Batch**: `try_claim_batch(n) → span` → fill → `commit`; `peek_batch() → span`
  → read → `release(k)`. One cursor publish per *run* instead of per record.

When to batch: **throughput-bound bulk stages, and only when both sides batch.**
A batch amortizes the cross-core cursor store over the run (up to ~5–25× on
drain-heavy paths), but it has a fixed per-run overhead — below ~16–32 records it
*loses* to the already-tight scalar path, and a scalar producer starves a batch
consumer down to the penalty regime. Don't route latency-sensitive or
single-record traffic through it. The span is clamped to the ring end, so a batch
that straddles the wrap comes back short — loop to claim/drain the tail.

Gates (advanced — you usually don't touch this)
-----------------------------------------------

`multicast_channel<T, Gate>` is templated on the gate; the default is right almost
always:

- `multicast_gate` (default) — `min` over the N subscriber cursors. O(N) per gate
  read, but reads happen only on back-pressure refresh.
- `lazy_min_gate` — caches the slowest subscriber so the floor is O(1) while one
  reader persistently lags. **Niche:** under back-pressure the producer is bounded
  by the laggard's rate anyway, so this barely moves throughput (it's a
  spin-CPU / very-large-fan-out optimization, not a throughput lever). Reach for
  it only with many subscribers and a known persistent laggard; otherwise keep the
  default.

Rules of the road
-----------------

- **One producer thread.** One consumer thread per `spsc_ring` / per subscriber
  `reader` / per `feed_reader`. Readers are independent and move-only.
- `T` must be **trivially copyable**. Capacity rounds **up to a power of two**.
- The `try_*` calls **never block** — they return `false` / `nullptr` / `empty`.
  Spin, `pause`, yield, or park: the wait strategy is the caller's.
- `multicast_channel`: subscribe **all** readers before producing (no late join);
  every one must keep draining or the producer stalls. `multicast_feed`: subscribe
  whenever; laggards are reported, not waited for.
- Correct and clean under ASan. (`multicast_feed`'s seqlock payload is a benign
  race that ThreadSanitizer will flag until it is made `atomic_ref`-clean.)

Performance
-----------

Representative, Apple Silicon (see the recorded block at the bottom of
`benchmarks/ufw-ring-benchmarks.cpp` for the live set — numbers are
machine-specific, trust the *relative* deltas):

| Path                                   | Cost           |
|----------------------------------------|----------------|
| `spsc_ring` push+pop                   | ~1.9 ns/op     |
| `spsc_ring` batch (256)                | ~0.4 ns/item   |
| `multicast_channel` drain, 1 sub       | ~8 ns/item     |
| `multicast_channel` batch drain        | ~0.4 ns/item   |
| `multicast_channel` throughput, 4 subs | ~36 ns/item (gated fan-out) |
| `multicast_feed` push (never blocks)   | ~1.8 ns, flat in subscriber count |

Regenerate with an optimized build and `python3 tools/bench.py ring` (it refuses
Debug/sanitized builds — numbers there are noise).
