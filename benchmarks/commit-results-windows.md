# Durable commit comparison - 2026-10-19

**Historical result for commit `835b89e`.** The collection timer and queue
admission have since been revised. See the [investigation](group-commit-investigation.md)
for the diagnosis and same-machine rerun; do not use this old comparison as the
current implementation's performance claim.

The previous resume claim, ?500 to 125 syncs,? was a batching observation, not
proof of better throughput or commit latency. This experiment compares a strict
one-sync-per-request baseline against the existing 200 microsecond collection
policy, with durable sync enabled in both modes.

## Environment and method

- Windows NT 10.0.26300.0, AMD Ryzen 5 7235HS, 8 reported logical processors.
- GCC/MinGW 15.2.0, CMake Release build; this is not a Clang/Linux measurement.
- Data under the Windows local temporary directory on C: (NTFS), outside OneDrive.
  Physical disk identity was not available to this session.
- `_commit` provides the Windows durable sync primitive. The POSIX build uses
  `fsync`; Linux results have not been measured here.
- Three runs per configuration, 1,024 measured writes per run, 128-byte values,
  distinct keys, and 64 untimed warmup writes. All latencies are retained.
- Writers start together and each has at most one outstanding request. This is
  a closed-loop test, not a fixed-rate load test; it does not characterize
  latency under externally queued traffic.
- Fresh database per case; case order reverses on alternating rounds. Key/value
  preparation and thread creation are outside the measured region. Throughput
  runs from barrier release to the last writer's completion. Request latency
  includes contention, queueing, collection delay, sync, and publication.
- 256 MiB MemTable, automatic compaction disabled. The harness asserts no flush
  or compaction occurred. This isolates WAL commits, not steady-state LSM writes.
- Every acknowledged key is checked after reopening, outside the timed region.
- Background system load, CPU affinity, power policy, OS page cache, and device
  write caches were not controlled. Each run's p99 is a nearest-rank estimate
  with only about 10 samples in the upper one percent. Treat these as preliminary
  local results, not a production capacity or latency guarantee.

## Results

Entries are medians of three per-run measurements. Ranges show min?max over
those runs. P99 columns are the median of per-run p99s, not a pooled percentile.

| Writers | Grouping | Writes/s median (range) | p99 ms median (range) |
|---:|---|---:|---:|
| 1 | Off | 1,365.4 (1,320.0?2,610.7) | 13.50 (0.63?15.39) |
| 1 | On, 200 us | 61.1 (60.7?61.4) | 33.49 (30.49?34.21) |
| 4 | Off | 1,446.8 (1,389.8?2,334.6) | 44.49 (30.43?60.85) |
| 4 | On, 200 us | 246.7 (243.4?249.5) | 29.81 (29.47?35.07) |
| 8 | Off | 1,454.0 (1,423.3?1,579.8) | 123.37 (68.62?148.69) |
| 8 | On, 200 us | 496.6 (483.2?505.0) | 29.15 (26.77?36.12) |
| 16 | Off | 1,467.2 (1,420.4?1,574.5) | 242.61 (220.30?282.63) |
| 16 | On, 200 us | 994.2 (950.3?995.8) | 33.86 (29.16?35.24) |

## Interpretation

The current grouping policy reduces throughput at every measured writer count.
At 16 writers, median throughput falls from 1,467 to 994 writes/s while median
p99 improves from 242.6 to 33.9 ms. This is a throughput/tail-latency tradeoff,
not an unconditional optimization. Ungrouped single-writer results also vary
substantially between runs, which makes a broad performance claim inappropriate.

The grouped single-writer throughput implies approximately 16 ms per request,
far above the configured 0.2 ms collection interval. Timer/scheduling overhead
is a plausible contributor, but this experiment did not isolate its cause.
Profiling and a separate delay-policy experiment are needed before attributing
it or changing the implementation. No tuning was performed to select favorable
numbers for this report.

Recovery time versus WAL size and write amplification remain unmeasured.
Reopening validates correctness here; it is not a timed crash-recovery benchmark.
A sustained mixed workload with background compaction is also still needed.

## Reproduce

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 4
ctest --test-dir build-release --output-on-failure
.\build-release\kv_commit_bench.exe 1024 3 benchmarks\commit-results-new.csv
```

Use a new CSV path for each invocation. Prefer a larger sample count (for example
16,384) and additional repetitions for stronger tail estimates. Keep comparisons
on the same device/filesystem and record compiler, sync primitive, and load conditions.

Raw measurements: [commit-results-windows.csv](commit-results-windows.csv).
Harness: [commit_bench.cpp](commit_bench.cpp).
