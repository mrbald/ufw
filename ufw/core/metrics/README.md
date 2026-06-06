uFW telemetry
=============

Gauges in a memory-mapped file: the hot path writes plain memory, anything that
can map the file reads — live from another process, or post-mortem after a crash.
The format is versioned and deliberately boring; `tools/metrics.py` mirrors it
field for field, and if a ~150-line dependency-free Python script can parse it,
any exporter sidecar can.

The three tiers (the sampling pattern)
--------------------------------------

| tier | who | what |
|------|-----|------|
| **hot** (inline) | workers, drainers | gauge writes straight into the mmap pages — no syscalls, seqlock bumps, single writer per gauge |
| **warm** (~1Hz, `sampler`) | one cold in-process thread | things needing a syscall (rusage) or a cross-thread snapshot (worker stats → utilization) |
| **cold** (out of process) | `tools/metrics.py`, an exposition sidecar | collection, aggregation, history, charting — the LGTM-stack side |

The file is the handoff boundary. The app never aggregates, never exports, never
opens a socket for telemetry; an exporter never perturbs the app and can be
restarted (or post-mortem-run) independently.

Gauge types
-----------

- **`counter`** — cumulative monotonic u64. Rates/deltas are derived DOWNSTREAM
  (PromQL `rate()`), never in-process. `add(n)` for owned counts, `set(v)` for
  mirroring an external cumulative source (CPU ns, worker stats).
- **`value<i64|f64>` / `value_str`** — instantaneous readings.
- **`series`** — u64 samples (latencies in ns, sizes, ...): count/sum/min/max,
  Welford mean/std, EWMA(α=1/16), an HDR-style log-linear histogram (16 minors per
  power of two ⇒ ≤6% relative error; cumulative bucket counts map 1:1 onto
  prometheus `le`-buckets / native histograms), and the worst-8 samples with tick
  stamps (≈ exemplars).

Contracts (the parts that keep it lock-free)
--------------------------------------------

- **Single writer per gauge.** Cross-process readers get torn-free snapshots via a
  per-gauge seqlock (even = stable, odd = mid-write — the in-tree multicast_feed
  pattern); single-u64 payloads are plain atomic reads. All writer-side atomics
  are `std::atomic_ref` over the mapped bytes — relaxed single-writer stores, no
  RMW on the hot path.
- **Registration is init-phase**, single-threaded, fixed capacity (`max_entries`,
  `arena_bytes`), zero allocation afterwards. Entities register their own gauges
  in `init()` via `application::metrics()`.
- **Null handles are inert no-ops** — instrument unconditionally, enable by config.
- **Names are prometheus-convention** (`ufw_dispatch_latency_ns{worker="1"}`) so
  exporters pass them through verbatim and Grafana gets labels for free.

Wiring (ufw/app)
----------------

```yaml
application:
  telemetry:
    file: myapp.metrics   # the mmap file (created/truncated at startup)
    interval_ms: 1000     # the warm tier cadence
```

With a worker pool, you get per worker: `iterations/useful/dispatched` mirrors,
thread CPU ns (with a terminal self-sample at loop exit, so runs shorter than an
interval still report), delta-based `utilization_pct`, and a per-message
`dispatch_latency_ns` series fed inline by the column drainer; plus process
maxrss + user/system CPU. The sampler flushes once more on stop — the file always
holds the end state.

Reading
-------

```sh
tools/metrics.py dump  myapp.metrics      # one snapshot
tools/metrics.py watch myapp.metrics      # refresh in place, 1s
```

Sample series line from the two-worker sandbox (Debug):

```
ufw_dispatch_latency_ns{worker="1"}: count=50000 min=101 max=3458 mean=258.9
    std=71.2 ewma=258.2 ~p50=248 ~p99=416 ~p99.9=448 worst=[3458, 2334, ...]
```

Costs (Apple Silicon reference; `tools/bench.py metrics`)
---------------------------------------------------------

See the recorded block in `benchmarks/ufw-metrics-benchmarks.cpp`. The number that
gates instrumentation decisions is `metrics_series_record` — the full per-sample
record (seqlock + Welford + EWMA + bucket + worst-N scan) the drainer pays per
message when latency telemetry is on.

Deferred (the format already supports; do not rebuild)
------------------------------------------------------

The prometheus exposition sidecar (a `--prom` mode of the same reader);
per-message-TYPE series (needs a naming story — likely the topics payload-id);
config knobs for EWMA α / histogram resolution (`hist_minor_bits` is the upgrade
path); mlock for the gauge pages; getrusage extras (faults, ctx switches).
