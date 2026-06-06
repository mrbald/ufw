uFW dispatch fabric
===================

Actors pinned to workers, sending each other messages through resolved **inbox
handles** — a direct method call when the target shares the caller's worker, an
SPSC ring hop when it doesn't. The whole fabric collapses to a single thread (one
worker that is both the dispatcher and the IO loop) and scales to N pinned hot
threads, with the choice made **purely in YAML**.

The seam (the one design decision everything hangs on)
------------------------------------------------------

The polymorphic thing is NOT the run loop — it is the **`poll_source`**: one
bounded, non-blocking unit of pollable work (`poll() -> count`). A **`worker`** is
an ordered list of poll_sources plus an optional **blocking backstop**
(`blocking_source`: adds `poll_blocking()`, must be `wakeable`). Loop flavours
differ only in idle policy:

| flavour    | each turn                          | when idle                       |
|------------|-------------------------------------|---------------------------------|
| `spinning` | drain all sources                   | `cpu_relax()`; backstop on a cadence |
| `blocking` | drain all sources once              | park in the backstop until IO or `wake()` |
| busy-poll IO (future) | NIC poller is just the first source | as spinning |

Adding a flavour touches neither this seam, nor the command, the handle, or the
matrix. The whole-app-on-one-thread case is a *configuration* (everything on
worker 0), not a special code path: all sends resolve direct, zero rings exist.

The dispatch
------------

- **`dispatch_cmd`** — the erased invocation, one 64-byte ring slot: a bound
  trampoline (a non-virtual delegate), the receiver, a latency stamp, and 40 bytes
  of tightly-packed trivially-copyable args. **Oversize or non-trivial args fail to
  compile** — a bigger payload travels by its own ring or an interned handle, as a
  deliberate decision at the call site, never a silent heap allocation.
- **`inbox_handle<void(Args...)>`** — the resolved send: trivially copyable, no
  `std::function`, no allocation. Direct-vs-enqueue is frozen at resolve time;
  the cross-worker path stamps, spin-pushes (loss-free back-pressure) and pokes the
  target's `wakeable` (a no-op branch for spinning targets — wired from day one so
  a parked blocking worker can become a matrix target later without re-plumbing).
- **`dispatch_matrix`** — cell `[from][to]` is the SPSC ring from worker `from` to
  worker `to`; single-producer/single-consumer by construction. Cells are created
  lazily at resolve; the diagonal never exists. `column_drainer` (a poll_source)
  drains a worker's inbound column with the rings' batch API and stamps dispatch
  latency into the worker's stats.

App integration (ufw/app)
-------------------------

```yaml
application:
  workers:                  # OPT-IN; absent => classic single-threaded context_.run()
    - { id: 0, loop: spinning, pin_core: 1 }
    - { id: 1, loop: spinning, pin_core: 2 }
  entities:
    - name: ping            # worker 0 by default
      ...
    - name: pong
      worker: 1             # owned by worker 1 -> ping<->pong sends cross the matrix
```

An actor declares `ufw::inbox_ref<void(Args...)>` members (signature-typed, so
mutually-messaging actors compose) and binds them in `init()` with
`ref.resolve<&target_type::method>()`. Worker 0 runs on the main thread and
services the `io_context` (signals, timers, posts) via its backstop; blocking
workers park in it.

Rules of the road
-----------------

- **One worker thread per actor** — a handler always runs on its owner's worker.
  Static assignment; no migration (yet).
- **Direct sends are synchronous**: a same-worker send LOOP (A→B→A→…) recurses.
  Break it by re-posting via the io_context (`inbox_ref::direct()` tells you), or
  put the actors on different workers — rings break the cycle naturally.
- Handlers must be noexcept-ish and non-blocking; blocking disk/network work is
  the future cool-down path (offload + reply), not something to do on a worker.
- Resolve in `init()`, never later: cells must exist before `wire_workers()` runs.
- Stats are single-writer (the worker's own thread); read them after stop/join.
- An empty io_context poll is a syscall: ring-fed spinners poll the backstop on a
  cadence (every 256th turn), io-only workers every turn — set at wiring time.

The three sandbox apps (the flavour regression set)
---------------------------------------------------

`examples/dispatch_{blocking,spinning,two_worker}.yaml` drive the same ping/pong
plugin (`dispatch_example.cpp`) through each shipped flavour — run all three after
touching the fabric; optimizing one must not degrade another. They have already
caught a teardown use-after-free, an unbounded backstop poll, invisible blocking
utilization, and an io-poll-on-the-reply-path latency bug.

Performance (Apple Silicon reference; `tools/bench.py dispatch`)
----------------------------------------------------------------

| path                                   | cost                   |
|----------------------------------------|------------------------|
| resolved direct call                   | ~0.26 ns (beats a 0.75 ns virtual call) |
| empty drain turn                       | ~0.8 ns per inbound cell |
| cross-worker round trip (2 cells)      | ~95 ns (~14 ns over raw-ring ping-pong) |
| cross-worker fire-and-forget           | ~26 ns/msg with latency telemetry on |

Deferred (the seam already supports; do not rebuild)
----------------------------------------------------

Blocking-worker-as-matrix-target (only `asio_backstop::wake()` is already real —
the send path is wired); busy-poll IO sources (io_uring/onload); the MPSC
dispatcher flavour (will abstract the handle's enqueue side); `schedule_up()`
through inboxes; the full telemetry subsystem (mmap file, getrusage, HDR,
CLI/prometheus); dynamic actor migration.
