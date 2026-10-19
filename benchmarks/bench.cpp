#include "kv/store.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
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
        if (!count || !threads || threads > 128 || count > 10000000) throw std::invalid_argument("Usage: kv_bench [operations 1..10000000] [threads 1..128]");
        const auto directory = std::filesystem::temp_directory_path() /
            ("kv-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Benchmark directory already exists");
        {
            kv::Options options; options.memtable_bytes = 256 * 1024;
            kv::Store db(directory, options);
            std::mutex mutex;
            std::vector<double> samples;
            std::exception_ptr error;
            std::vector<std::thread> workers;
            auto start = Clock::now();
            for (std::uint64_t t = 0; t < threads; ++t) workers.emplace_back([&, t] {
                try {
                    std::vector<double> local;
                    for (auto i = t; i < count; i += threads) {
                        const auto op = Clock::now();
                        db.put("key-" + std::to_string(i), std::string(128, 'x'));
                        local.push_back(seconds(op) * 1e6);
                    }
                    std::lock_guard<std::mutex> guard(mutex); samples.insert(samples.end(), local.begin(), local.end());
                } catch (...) { std::lock_guard<std::mutex> guard(mutex); error = std::current_exception(); }
            });
            for (auto& worker : workers) worker.join();
            if (error) std::rethrow_exception(error);
            report("durable writes", seconds(start), std::move(samples));
            db.flush();
            for (const auto* workload : {"reads cold", "reads warm", "missing reads"}) {
                samples.clear(); start = Clock::now();
                for (std::uint64_t i = 0; i < count; ++i) {
                    const auto op = Clock::now();
                    const bool missing = workload[0] == 'm';
                    const auto result = db.get((missing ? "missing-" : "key-") + std::to_string(i));
                    if (result.has_value() == missing) throw std::runtime_error("Benchmark read validation failed");
                    samples.push_back(seconds(op) * 1e6);
                }
                report(workload, seconds(start), std::move(samples));
            }
            start = Clock::now(); const auto rows = db.scan();
            std::cout << "scan: " << rows.size() << " rows in " << seconds(start) << " s\n";
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
