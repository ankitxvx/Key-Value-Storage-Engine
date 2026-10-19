# Persistent C++ key-value storage engine

A dependency-free C++17 LSM storage library with durable writes, MVCC snapshots,
atomic batches, background flush/compaction, Bloom filters, a bounded block cache,
sparse indexes, ordered scans, group commit, live backups, and metrics.
Keys and values are arbitrary byte strings, including empty strings and NULs.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

With MinGW on Windows, add `-G "MinGW Makefiles"` to the configure command and
put the MinGW compiler and `mingw32-make` on PATH. Run `build/kv_demo` for a small
example. Multi-configuration generators put executables in `build/Release`.

## Source layout

The public API stays in `include/kv/store.hpp`. Implementation headers are private
to `src/detail/`; each module compiles separately:

| Source | Responsibility |
|---|---|
| `src/store.cpp` | Public Store API, snapshot handles, and metrics |
| `src/engine.cpp` | Startup/recovery, lifetime, and snapshot tracking |
| `src/write.cpp` | Writer queue, atomic batches, group commit, and WAL checkpointing |
| `src/read.cpp` | Point/range reads and the LRU block cache |
| `src/background.cpp` | Background flush and compaction coordination |
| `src/backup.cpp` | Consistent backup and restore |
| `src/iterator.cpp` | Materialized iterator navigation |
| `src/file.cpp` | Platform file I/O, syncing, atomic publication, and directory locks |
| `src/codec.cpp` | Binary encoding, checksums, and WAL batch decoding |
| `src/bloom.cpp` | Bloom filter hashing and membership checks |
| `src/sstable.cpp` | SSTable construction, validation, sparse indexes, and block reads |
| `src/manifest.cpp` | Manifest encoding, validation, and publication |

`src/detail/engine.hpp` declares the shared private engine state and operation
methods. The modules preserve one locking protocol and the existing disk formats.

## API examples

```cpp
#include "kv/store.hpp"

kv::Options options;
options.memtable_bytes = 4 * 1024 * 1024;
options.cache_bytes = 8 * 1024 * 1024;
kv::Store db("my-database", options);

db.put("user:100", "Ada");
auto snapshot = db.snapshot();
db.write_batch({
    kv::Mutation::put("user:100", "Grace"),
    kv::Mutation::put("user:101", "Linus"),
    kv::Mutation::erase("user:102")
});
auto current = db.get("user:100");           // optional<string>{"Grace"}
auto old = db.get("user:100", snapshot);     // optional<string>{"Ada"}

for (const auto& [key, value] : db.scan("user:100", "user:200")) {
    // Sorted, half-open range [user:100, user:200).
}
auto it = db.iterator(snapshot);
it.seek("user:100");
if (it.valid()) { auto value = it.value(); it.next(); }

db.flush();                                // Wait for a durable checkpoint.
db.compact();                              // Flush, then wait for full compaction.
db.backup("backup-001");                    // Destination must not exist.
kv::Store::restore("backup-001", "restored");
auto stats = db.metrics();
```

`scan()` accepts an optional upper bound; omit it to scan through the end.
`scan(begin, end, snapshot)` reads historical data. Scans and iterators materialize
their results into owned memory; they are consistent and remain usable after
the Store is closed. `Iterator` also exposes `begin()`/`end()` for range loops.
Calling `key()` or `value()` on an exhausted iterator throws.

## Durability and recovery

Every mutation receives an increasing 64-bit sequence. A single write uses a
checksummed WAL record; a batch uses one outer length-delimited, checksummed
envelope containing all its mutations. Repeated keys in a batch are applied in
order. Readers cannot observe a batch halfway through, and recovery either
replays the entire envelope or discards an incomplete trailing envelope. The
payload limit is 64 MiB per single record or complete batch, including framing.

Concurrent writers join a FIFO queue. Its first request becomes the leader and
immediately syncs up to 64 requests already queued, without a collection timer.
Followers arriving during IO join the next group. The leader releases the state
mutex during WAL IO so followers can enqueue and readers can proceed. Grouping
disabled uses the same admission/handoff protocol but syncs one request at a time.
FIFO applies after queue admission; OS scheduling is not a hard fairness guarantee.
Each request retains its own atomic envelope. Successful calls return only after
`fdatasync` on Linux, `_commit` on Windows, or `fsync` on other POSIX systems,
followed by in-memory publication. A failed or interrupted call
may have committed; applications must account for that ambiguity. Group commit
does not introduce read-modify-write transactions or conflict detection.

The worker freezes the active MemTable and writes an immutable SSTable outside
the main mutex. Writes proceed into a new active MemTable. If that fills before
the worker finishes, writers wait to limit the backlog. SSTables are sorted by
key and descending sequence, with a record count and per-record CRC32 checksums.

Publication order is:

1. Write and sync the temporary SSTable, then atomically rename it.
2. Write and sync a checksummed manifest listing the complete live table set and
   durable checkpoint sequence, then atomically replace `MANIFEST`.
3. Replace the WAL atomically with a synced file containing only active versions
   newer than the checkpoint.

WAL replacement waits for the current leader's IO and publication to finish,
and blocks new leaders until the checkpoint completes. This prevents replacing
a WAL handle underneath a writer. Backpressure is applied before leader election,
so it cannot deadlock a checkpoint waiting for that leader.
The WAL rewrite holds the mutex; large active MemTables can briefly stall calls.
Old WAL records left after a crash are skipped using the manifest checkpoint.
Unpublished temporary files are ignored; unreferenced SSTables are reclaimed on
reopen. Published tables missing from disk or with invalid checksums cause opening
to fail. The checkpoint survives even when compaction removes every table.

Existing v1 stores without a manifest are adopted on first open. Do not remove a
manifest from a newer store: it is authoritative, especially after compaction.
New atomic WAL batches are not readable by the original implementation.

POSIX publication syncs the containing directory. Windows file publication uses
`MoveFileExW` with `MOVEFILE_WRITE_THROUGH`. Guarantees require a local filesystem
and device that honor sync and atomic rename. Checksums detect accidental payload
corruption, not adversarial tampering or every form of damaged framing. Incomplete
final WAL records are treated as torn writes. Do not modify database files externally.

## Compaction and snapshots

Automatic compaction merges the live SSTables once the configured table count is
reached. `compact()` requests it explicitly, including for a single table. The
merge runs on the worker while foreground operations continue. It replaces all
input tables through one manifest update and only then retires the old files.
Backups hold references to their source tables, delaying physical deletion.

Snapshots register a shared sequence pin. Copies share that pin; releasing the
last copy unpins it. Compaction retains all versions newer than the oldest live
snapshot plus the newest version at or below that sequence. With full table
coverage, obsolete tombstones and their older values can be removed. An old
snapshot can therefore delay reclamation. Snapshots belong to one open Store
instance and cannot be used with another instance or after reopening.

## Read optimizations and metrics

Point reads check active/frozen MemTables, then SSTables newest first. Per-table
Bloom filters skip definite misses. A sparse index stores the first key and file
boundaries of each block, rather than an entry for every key/version. Blocks never
split a key's version history, so a hot key can exceed the target block size.

An LRU cache stores decoded blocks within an approximate accounted byte budget.
Oversized blocks bypass the cache. Indexes and Bloom filters are rebuilt while
validating tables on open; this saves permanent index format complexity but makes
startup linear in live table data. Blocks are decoded sequentially on a cache miss.

`metrics()` exposes per-open-instance counters for mutations, batches, reads,
scans, WAL syncs/bytes, grouped requests, cache hits/misses, disk block reads, Bloom
skips, flushes, compactions, reclaimed versions, and cumulative successful point
read/write latency. Gauges include sequence/checkpoint, active snapshots, live
SSTable bytes/count, sparse index entries, Bloom bytes, cache bytes, and MemTable
bytes and pending writers. Retired files pinned by a backup are excluded from live SSTable gauges.

| Option | Default | Meaning |
|---|---:|---|
| `memtable_bytes` | 4 MiB | Approximate encoded bytes before background freeze |
| `block_bytes` | 16 KiB | Target sparse-index/cache block size |
| `cache_bytes` | 8 MiB | Accounted decoded block budget; zero disables caching |
| `bloom_bits_per_key` | 10 | Filter size, seven hash probes; zero disables filters |
| `compaction_table_trigger` | 8 | Automatic merge threshold; zero disables, one is invalid |
| `group_commit` | `true` | Set false for one durable sync per request with no collection delay |
| `fault_injector` | empty | Thread-safe test callback for named failure/crash points |

## Backup and restore

`backup(destination)` flushes through the writes visible at its start, captures a
consistent checkpoint, and copies its pinned tables while later writes and
compactions continue. It writes a manifest and empty WAL into a sibling staging
directory, then renames that completed directory into place. The backup can be
opened directly or copied with `Store::restore`. Existing destinations are rejected.
The destination parent must exist. Use a separate destination outside the database.

Failed backups can leave `<destination>.backup-tmp` for inspection; remove or
rename that incomplete directory before retrying the same destination. Restore
validates and exclusively opens its source and requires an existing manifest.
Backups capture data, not runtime options, metrics, or live snapshot handles.

## Tests and benchmarks

`kv_tests` covers CRUD, binary data, historical reads, exclusive ownership,
reopening, process exit without destructors, all final-record truncation boundaries,
randomized reference-model comparison, and concurrent access.

`kv_advanced_tests` covers reclamation with pinned/released snapshots, tombstone
removal with an empty table set, ordered scans/iterators, every partial batch
boundary, sparse indexes, cache eviction, Bloom filtering, group commit/failure
notification, foreground progress during background IO, automatic compaction,
backups overlapping compaction, restore, v1 adoption, orphan rejection, manifest
corruption, and disabled optimizations. Child processes exit at 15 named WAL,
flush, manifest, and compaction boundaries. Exception injection simulates disk-full
and sync failures. These are process/file fault simulations, not power-loss tests
or a real full-disk test.

Run the benchmark with operation and writer-thread counts:

```sh
./build/kv_bench 10000 4
```

It uses a unique temporary database and reports durable write/read throughput,
p50/p95/p99 operation latency, scan/compaction time, live disk usage, WAL syncs,
grouping, cache activity, and Bloom skips. Its first read pass starts with an empty
application cache; it does not clear the OS page cache. Successful runs remove
their temporary database. Compare Release builds on the same filesystem/device;
these numbers are not a general storage performance claim.

For a controlled comparison of the current group-commit policy against an
ungrouped durable baseline, run:

```sh
./build/kv_commit_bench 32768 10 commit-results.csv
```

This runs 1, 4, 8, and 16 synchronized writer threads with grouping off/on,
alternating case order across rounds. Each case uses a fresh database, 64 untimed
warmup writes, 128-byte values, and records every operation's end-to-end latency.
CSV output includes throughput, mean and nearest-rank p50/p95/p99/p99.9/max
latency, and sync counts. Sync remains enabled in both modes (`fdatasync` on Linux,
`fsync` on other POSIX systems, `_commit` on Windows). Neither mode uses a
collection delay; the former `group_commit_delay_us` option has been removed.
Use `kv_wait_probe` to reproduce the old 200-microsecond wait independently.

The comparison isolates WAL commits: a 256 MiB MemTable and disabled automatic
compaction keep background IO outside the timed region. It uses a closed-loop
workload (one outstanding request per writer), not fixed-rate arrival traffic.
All acknowledged keys are checked after reopening each case. Supply an existing
data-parent directory as a fourth argument to choose the tested filesystem.
The CSV path must not already exist. Recovery validation is not a recovery-time
benchmark; write amplification is not measured by this tool.

The [group-commit investigation](benchmarks/group-commit-investigation.md) records
the old timer/admission problems, deterministic regression tests, and the larger
Linux benchmark protocol. Generate a table with ranges from a complete matrix:

```sh
python3 benchmarks/summarize_commit.py commit-results.csv --rounds 10 --output commit-summary.md
```

## Remaining boundaries

This is a working learning-scale engine, not a production database. Full compaction
materializes its input in memory, and scans materialize results. It uses one worker
and a mutex for foreground operations; it has no lock-free reads, leveled/tiered
scheduling, compression, replication, or transactional read-modify-write API.
Compaction is prioritized after pending flush work and can be delayed by sustained
writes. OS allocator overhead is not included exactly in memory gauges.

Write/flush/compaction IO failures poison the instance and propagate to waiters;
close and reopen before further use. Backup-copy failures leave the source usable.
Destruction joins the worker and closes files; remaining active data is recovered
from the synced WAL. Stop all application calls before destroying the Store.
