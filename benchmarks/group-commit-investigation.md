# Group-commit investigation

The old Windows comparison at commit `835b89e` was not evidence of a useful
tradeoff. It combined a delayed group-commit implementation with an unfair
admission path in the ungrouped baseline. The old CSV is retained as historical
evidence, not as the current engine's performance claim.

## Collection timer

The old leader called `condition_variable::wait_for` with 200 microseconds to
collect followers. `kv_wait_probe` isolates that exact wait primitive. Across
512 calls on this Windows machine, its mean was **15.95 ms**, median **16.49 ms**,
and p99 **26.39 ms**. A timeout is not a precise wakeup deadline. This reproduces
the approximately 16 ms collection cost; it does not isolate whether the OS timer,
runtime, scheduler, or their interaction accounts for every part of the delay.

The revised leader does not wait to collect requests. It commits the queued prefix
immediately, up to 64 requests. Writers arriving during that IO form the next group.
The `group_commit_delay_us` option was removed.

Raw probe: [wait-probe-windows.csv](wait-probe-windows.csv).

## Queue admission and baseline

Previously the shared state mutex remained locked throughout WAL write and sync.
With grouping disabled, a returning writer could acquire it again before other
callers had even entered the supposedly FIFO queue. FIFO inside that queue did
not provide fair admission. Flush and compaction were disabled in the measured
workload, so they cannot explain its old tail.

Now both modes release the state mutex during WAL IO, use the same queue and
leader handoff, and differ only in how many queued requests share a sync. Reads
can proceed during that IO. A checkpoint waits for the WAL owner to finish before
replacing the handle, while blocking new leaders to avoid checkpoint starvation.
Backpressure is checked before claiming leadership to avoid a wait cycle.

Deterministic tests pause one leader in sync, admit followers in a known order,
check that uncommitted data is invisible, and verify FIFO publication. They expect
five syncs with grouping off and two with grouping on (one leader plus four queued
followers). Another test overlaps a checkpoint with a paused WAL owner and checks
recovery. Fairness is guaranteed after enqueueing, not across arbitrary OS thread
scheduling or mutex admission before a request is enqueued.

## Same-machine Windows sanity check

These are three runs of 1,024 writes per mode with the same Windows Release
toolchain, value size, and local NTFS temporary directory as the old comparison.
They are a small diagnostic check, not the final Linux benchmark. Medians of runs:

| 16 writers | Writes/s | p99 ms |
|---|---:|---:|
| Old ungrouped | 1,467 | 242.6 |
| Revised ungrouped | 1,452 | 21.1 |
| Old grouped, timed collection | 994 | 33.9 |
| Revised grouped, no collection timer | 10,355 | 14.5 |

The baseline's throughput barely changes while its long tail drops. The revised
grouped path also removes the old throughput regression. This supports the
diagnosis but is not an isolated estimate of each change's contribution: scheduler
and storage variability were not controlled, and both changes were made together.

Raw rerun: [commit-results-windows-fifo.csv](commit-results-windows-fifo.csv).

## Linux protocol

**Status:** the full 80-trial Linux matrix is still running. Its partial CSV is
not included in this commit, and no completed Linux performance claim is made.
The Clang Release build and the original Linux regression suites passed before
the run started. The additional checkpoint/writer race test was subsequently
validated in the Windows regression run.

- Ubuntu 24.04.1 under WSL2, kernel 6.6.87.2-microsoft-standard-WSL2.
- Clang 18.1.3, Release build; Ryzen 5 7235HS, four cores/eight logical CPUs.
- Data under `/home/ankit` on ext4 inside the WSL virtual disk, not `/mnt/c`.
- WAL commits use **fdatasync** in both modes. Files/manifest publication and
  directory durability still use fsync. Windows retains `_commit`.
- 32,768 unique-key writes per trial, 128-byte values, 64 untimed warmup writes,
  ten runs each at 1/4/8/16 writers, grouping off and on: 80 trials, 2,621,440
  measured writes. Case order reverses between rounds.
- No timed collection in either mode. Fresh database each trial; 256 MiB MemTable
  and automatic compaction disabled to isolate commits. The harness asserts that
  no flush/compaction occurred and checks every key after reopening.
- Synchronized start, one outstanding operation per writer. All request latencies
  recorded, with mean, p50, p95, p99, p99.9, maximum, throughput, and sync counts.
  A per-trial p99.9 has about 33 samples above it; report ranges over trials too.
- No CPU pinning, power-policy control, OS cache clearing, or host-load isolation.
  WSL2 virtualizes the storage stack. Calling fdatasync is not evidence that a
  consumer device will survive power loss; no such experiment was performed.
- This is a WAL-only closed-loop benchmark, not a sustained compaction or
  externally paced workload. Recovery-time scaling and write amplification remain
  separate, unmeasured questions.

```sh
cmake -S . -B build-clang-linux -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-clang-linux -j 4
ctest --test-dir build-clang-linux --output-on-failure
./build-clang-linux/kv_commit_bench 32768 10 benchmarks/commit-results-linux-fifo.csv /home/ankit
python3 benchmarks/summarize_commit.py benchmarks/commit-results-linux-fifo.csv --rounds 10 --output benchmarks/commit-results-linux-fifo.md
```

The summary script rejects incomplete matrices and checks that ungrouped trials
performed exactly one WAL sync per measured request. Raw CSVs retain every trial's
statistics; no trials are removed for being slow.
