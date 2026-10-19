#include "kv/store.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Result {
    double seconds, throughput, p50_us, p95_us, p99_us, max_us;
    std::uint64_t syncs;
};

Result run(const fs::path& directory, std::size_t count, std::size_t writers, bool grouped) {
    kv::Options options;
    options.group_commit = grouped;
    options.memtable_bytes = 256 * 1024 * 1024;
    options.compaction_table_trigger = 0;
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) keys.push_back("key-" + std::to_string(i));
    const std::string value(128, 'v');
    Result result{};
    {
        kv::Store db(directory, options);
        for (int i = 0; i < 64; ++i) db.put("warmup-" + std::to_string(i), value);
        const auto before = db.metrics();
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t ready = 0;
        bool go = false;
        std::exception_ptr error;
        std::vector<std::thread> workers;
        std::vector<std::vector<double>> samples(writers);
        std::vector<Clock::time_point> finishes(writers);
        for (std::size_t t = 0; t < writers; ++t) {
            samples[t].reserve(count / writers + 1);
            workers.emplace_back([&, t] {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ++ready; cv.notify_all();
                    cv.wait(lock, [&] { return go; });
                }
                try {
                    for (auto i = t; i < count; i += writers) {
                        const auto begin = Clock::now();
                        db.put(keys[i], value);
                        samples[t].push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex); error = std::current_exception();
                }
                finishes[t] = Clock::now();
            });
        }
        Clock::time_point begin;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return ready == writers; });
            begin = Clock::now(); go = true; cv.notify_all();
        }
        for (auto& worker : workers) worker.join();
        if (error) std::rethrow_exception(error);
        result.seconds = std::chrono::duration<double>(*std::max_element(finishes.begin(), finishes.end()) - begin).count();
        std::vector<double> all;
        all.reserve(count);
        for (const auto& part : samples) all.insert(all.end(), part.begin(), part.end());
        if (all.size() != count) throw std::runtime_error("Missing latency samples");
        std::sort(all.begin(), all.end());
        const auto quantile = [&](double q) { return all[static_cast<std::size_t>(std::ceil(q * all.size())) - 1]; };
        result.throughput = count / result.seconds;
        result.p50_us = quantile(.50); result.p95_us = quantile(.95); result.p99_us = quantile(.99);
        result.max_us = all.back();
        const auto after = db.metrics();
        result.syncs = after.wal_syncs - before.wal_syncs;
        if (after.writes - before.writes != count || (!grouped && result.syncs != count))
            throw std::runtime_error("Invalid durable baseline");
        if (after.flushes || after.compactions) throw std::runtime_error("Unexpected background IO in commit benchmark");
    }
    // Validate recovery outside the timed region, including every acknowledged key.
    {
        kv::Store reopened(directory, options);
        for (const auto& key : keys)
            if (reopened.get(key) != value) throw std::runtime_error("Recovery validation failed");
    }
    fs::remove_all(directory);
    return result;
}

int main(int argc, char** argv) {
    try {
        const auto count = argc > 1 ? std::stoull(argv[1]) : 4096;
        const auto rounds = argc > 2 ? std::stoull(argv[2]) : 3;
        if (count < 1024 || count > 1000000 || !rounds || rounds > 100)
            throw std::invalid_argument("Usage: kv_commit_bench [operations 1024..1000000] [rounds 1..100] [new CSV path] [existing data parent]");
        const fs::path csv = argc > 3 ? fs::path(argv[3]) : fs::path("commit-results.csv");
        if (fs::exists(csv)) throw std::invalid_argument("CSV already exists; select a new output file");
        const auto parent = argc > 4 ? fs::path(argv[4]) : fs::temp_directory_path();
        const auto root = parent / ("kv-commit-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
        if (!fs::create_directory(root)) throw std::runtime_error("Could not create unique benchmark directory");
        std::ofstream output(csv);
        if (!output) throw std::runtime_error("Cannot open CSV output");
        output << "round,writers,group_commit,operations,value_bytes,delay_us,seconds,ops_per_sec,p50_us,p95_us,p99_us,max_us,wal_syncs\n";
        output << std::fixed << std::setprecision(3);
        std::cout << "Data parent: " << fs::absolute(parent) << "\nDurable sync enabled for every request/group.\n";
        // Reverse case order on alternating rounds to reduce order/warmup bias.
        std::vector<std::pair<std::size_t, bool>> cases;
        for (std::size_t writers : {1, 4, 8, 16}) for (bool grouped : {false, true}) cases.emplace_back(writers, grouped);
        for (std::size_t round = 1; round <= rounds; ++round) {
            for (const auto& test : cases) {
                const auto writers = test.first;
                const auto grouped = test.second;
                const auto r = run(root / (std::to_string(round) + "-" + std::to_string(writers) + (grouped ? "-on" : "-off")), count, writers, grouped);
                output << round << ',' << writers << ',' << grouped << ',' << count << ",128," << (grouped ? 200 : 0)
                       << ',' << r.seconds << ',' << r.throughput << ',' << r.p50_us << ',' << r.p95_us
                       << ',' << r.p99_us << ',' << r.max_us << ',' << r.syncs << '\n';
                output.flush();
                if (!output) throw std::runtime_error("CSV write failed");
                std::cout << "Round " << round << ", writers=" << writers << ", group=" << grouped
                          << ": " << r.throughput << " ops/s, p99=" << r.p99_us << " us\n" << std::flush;
            }
            std::reverse(cases.begin(), cases.end());
        }
        fs::remove_all(root);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
