#include "kv/store.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>

using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
void report(const char* name, double elapsed, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double p) { return samples[static_cast<std::size_t>(p * (samples.size() - 1))]; };
    std::cout << name << ": " << samples.size() / elapsed << " ops/s; latency us p50=" << percentile(.50)
              << " p95=" << percentile(.95) << " p99=" << percentile(.99) << '\n';
}
int main(int argc, char** argv) {
    try {
        const auto count = argc > 1 ? std::stoull(argv[1]) : 1000;
        const auto threads = argc > 2 ? std::stoull(argv[2]) : 4;
        const auto cache_bytes = argc > 3 ? std::stoull(argv[3]) : 8 * 1024 * 1024;
        const auto bloom_bits = argc > 4 ? std::stoull(argv[4]) : 10;
        if (argc > 5 || !count || !threads || threads > 128 || count > 10000000 || bloom_bits > 64)
            throw std::invalid_argument("Usage: kv_bench [operations 1..10000000] [threads 1..128] [cache bytes] [Bloom bits/key 0..64]");
        const auto directory = std::filesystem::temp_directory_path() /
            ("kv-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Benchmark directory already exists");
        {
            kv::Options options; options.memtable_bytes = 256 * 1024;
            options.cache_bytes = cache_bytes;
            options.bloom_bits_per_key = bloom_bits;
            kv::Store db(directory, options);
            const std::string value(128, 'x');
            std::vector<std::uint64_t> order(count);
            std::iota(order.begin(), order.end(), 0);
            std::mt19937 random(42);
            std::shuffle(order.begin(), order.end(), random);
            std::cout << "config: operations=" << count << "; writers=" << threads
                      << "; value_bytes=128; memtable_bytes=" << options.memtable_bytes
                      << "; cache_bytes=" << cache_bytes << "; bloom_bits_per_key=" << bloom_bits
                      << "; read_seed=42; directory=" << directory.string() << '\n';
            std::mutex mutex;
            std::vector<double> samples;
            std::exception_ptr error;
            std::vector<std::thread> workers;
            auto start = Clock::now();
            for (std::uint64_t t = 0; t < threads; ++t) workers.emplace_back([&, t] {
                try {
                    std::vector<double> local;
                    local.reserve((count + threads - 1) / threads);
                    for (auto i = t; i < count; i += threads) {
                        const auto op = Clock::now();
                        db.put("key-" + std::to_string(i), value);
                        local.push_back(seconds(op) * 1e6);
                    }
                    std::lock_guard<std::mutex> guard(mutex); samples.insert(samples.end(), local.begin(), local.end());
                } catch (...) { std::lock_guard<std::mutex> guard(mutex); error = std::current_exception(); }
            });
            for (auto& worker : workers) worker.join();
            if (error) std::rethrow_exception(error);
            report("durable writes", seconds(start), std::move(samples));
            const auto written = db.metrics();
            std::cout << "write metrics: WAL syncs=" << written.wal_syncs
                      << "; grouped requests=" << written.grouped_requests << '\n';
            db.flush();
            for (const auto* workload : {"random reads first pass", "random reads second pass", "missing reads"}) {
                samples.clear(); samples.reserve(count);
                const auto before = db.metrics();
                start = Clock::now();
                for (std::uint64_t i = 0; i < count; ++i) {
                    const auto op = Clock::now();
                    const bool missing = workload[0] == 'm';
                    const auto result = db.get("key-" + std::to_string(order[i]) + (missing ? "-absent" : ""));
                    if (result.has_value() == missing) throw std::runtime_error("Benchmark read validation failed");
                    if (result && *result != value) throw std::runtime_error("Benchmark value validation failed");
                    samples.push_back(seconds(op) * 1e6);
                }
                report(workload, seconds(start), std::move(samples));
                const auto after = db.metrics();
                std::cout << "read metrics: cache_hits=" << after.cache_hits - before.cache_hits
                          << "; block_reads=" << after.disk_block_reads - before.disk_block_reads
                          << "; bloom_checks=" << after.bloom_checks - before.bloom_checks
                          << "; bloom_skips=" << after.bloom_negatives - before.bloom_negatives << '\n';
            }
            start = Clock::now(); const auto rows = db.scan();
            std::cout << "scan: " << rows.size() << " rows in " << seconds(start) << " s\n";
            if (rows.size() != count) throw std::runtime_error("Benchmark scan count validation failed");
            for (std::size_t i = 0; i < rows.size(); ++i)
                if (rows[i].second != value || (i && rows[i - 1].first >= rows[i].first))
                    throw std::runtime_error("Benchmark scan validation failed");
            start = Clock::now(); db.compact();
            const auto m = db.metrics();
            std::cout << "compaction: " << seconds(start) << " s; disk=" << m.sstable_bytes
                      << " bytes; tables=" << m.sstables << "; WAL syncs=" << m.wal_syncs
                      << "; grouped requests=" << m.grouped_requests << "; cache hits=" << m.cache_hits
                      << "; block reads=" << m.disk_block_reads << "; Bloom skips=" << m.bloom_negatives << '\n';
        }
        std::filesystem::remove_all(directory);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
