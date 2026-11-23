# Measured benchmark results

Measured on 2026-11-23. Engine source revision:
`c50484365004906751b75dd17c0719fd9c75d1b0`, with the benchmark harness changes
in this working tree. No engine performance changes were made for this measurement.

## Results and resume wording

Each summary is the median of three independent trials, not the best run.
Latency summaries are medians of the three per-trial percentiles, not pooled
percentiles. Raw output: [trial 1](results/2026-11-23-run-1.txt),
[trial 2](results/2026-11-23-run-2.txt), [trial 3](results/2026-11-23-run-3.txt).

| Workload | Median ops/sec | Trial range ops/sec | Median p50 | Median p95 | Median p99 |
|---|---:|---:|---:|---:|---:|
| Durable writes, four writers | 936 | 699–1,081 | 2.731 ms | 17.569 ms | 22.552 ms |
| Random reads, first pass | 271,897 | 197,973–305,784 | 1.177 µs | 2.181 µs | 57.864 µs |
| Random reads, second/cached pass | 776,903 | 502,540–877,344 | 1.121 µs | 1.900 µs | 3.052 µs |
| Missing-key lookups | 2,449,970 | 1,919,550–2,543,380 | 0.353 µs | 0.476 µs | 1.191 µs |

- Full 100,000-row scan: median **107.21 ms**, range 99.63–123.36 ms.
- Explicit final compaction: median **0.892 s**, range 0.803–1.009 s.
- Final live SSTable: **16,188,906 bytes**, one table in every trial.
- Commit WAL syncs: **25,008 median** (25,005–25,012) for 100,000 writes,
  **74.992% fewer** than one commit sync per write. This is a counter-derived
  comparison, not a separately measured baseline.
- Cached-pass block reads: **zero** in all trials.
- Absent-key Bloom checks rejected **99.22%** of table probes at the median
  (595,336 / 600,000); range 99.19–99.28%. This is not a latency speedup claim.

Suggested resume bullets:

> Built a C++17 LSM key-value engine delivering 777K cached random reads/sec
> with 3.05 µs p99 latency across 100K keys (128-byte values, 32 MiB cache;
> median of three local WSL2 trials).

> Implemented WAL group commit, coalescing 100K durable writes into 25K commit
> syncs with four concurrent writers—75% fewer syncs than one per write.

Durable writes are much slower than cached reads here. Do not describe the
777K figure as write throughput, disk-read throughput, or general mixed-workload
performance. Both test suites passed before measurement, and all three trials
passed full read-value and scan validation.

### Follow-up correctness fix

A post-measurement smoke test with cache and Bloom filters disabled exposed an
existing lifetime bug in `scan`: dereferencing a temporary block `shared_ptr`
left uncached records unowned during iteration. `src/read.cpp` now keeps the
owner alive through the loop, with current/snapshot scan regression checks.
The recorded performance trials preceded this fix and used a cache large enough
to retain their blocks. They are not measurements of the post-fix binary.
After the fix, both correctness suites passed (4.95 s total), and the previously
failing `kv_bench 20 2 0 0` completed with full value/scan validation.

## Conditions

- Intel Core i5-1135G7, 4 physical cores / 8 logical processors; host RAM 7.70 GiB.
- Windows build 26200; Ubuntu under WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`.
- WSL reported 3.7 GiB RAM and 1 GiB swap. Host reports Intel NVMe SSD devices
  `HBRPEKNX0202A` and `HBRPEKNX0202AO`; the specific virtual-disk backing device
  was not independently mapped.
- GCC 15.2.0, CMake Release, `-O3 -DNDEBUG -std=c++17`.
- Database directories on `/var/tmp`, disk-backed WSL ext4 (`/dev/sdd`).
  `/tmp` is tmpfs on this machine and was deliberately excluded.
- Each trial creates a fresh database with 100,000 unique `key-N` keys and
  128-byte values (12.8 MB of value payload).
- Four concurrent writers, one `put` per request; default 200 microsecond
  group-commit delay; successful writes wait for the POSIX WAL `fsync` path.
- MemTable 256 KiB (benchmark setting); block size 16 KiB; cache **32 MiB**
  (library default is 8 MiB); Bloom filter 10 bits/key; automatic compaction
  trigger eight tables. All other options retain library defaults.
- After a flush, one reader performs two shuffled passes (seed 42), then
  100,000 absent-key lookups interspersed in the populated key range.
  Point reads check complete values. A full ordered scan checks count,
  ordering, and values, followed by explicit full compaction.
- Throughput includes workload-loop overhead. Per-operation latency includes
  key construction and, for reads, value validation. Percentiles are sampled
  per operation; each trial records 100,000 samples per workload.

## Reproduce

From Ubuntu/WSL in the repository:

```sh
bash benchmarks/run-resume.sh
```

The script builds Release binaries, runs both correctness suites, and saves
three fresh-database trials. It assumes `/var/tmp` is disk-backed; inspect the
printed filesystem type. Run trials serially to avoid cross-trial contention.

## Interpretation limits

This is a local synthetic benchmark, not a production capacity estimate or a
comparison with RocksDB/LevelDB. The 32 MiB application cache is sized to hold
this dataset. The second pass measures cached reads; the OS page cache is never
cleared. Engine block-read counters count application reads, not physical SSD
IOs. WSL virtual storage and other host activity affect timings. Power-loss
durability was not tested.

WAL sync counters count foreground commit groups, excluding checkpoint WAL,
manifest, and SSTable syncs. Any percentage reduction refers specifically to
commit syncs relative to one commit sync per write, not all storage syncs or a
measured throughput speedup. Scan results materialize in memory. Explicit
compaction timing excludes earlier automatic compaction work.
