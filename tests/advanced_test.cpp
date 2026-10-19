#include "kv/store.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void throws(F action, const char* message) {
    bool caught = false;
    try { action(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}
std::string bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void replace(const fs::path& file, std::string_view data) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(bool(out), "test write failed");
}
void crash_child(const fs::path& exe, const fs::path& dir, const std::string& stage) {
#ifdef _WIN32
    const std::wstring wide_stage(stage.begin(), stage.end());
    auto command = L"\"" + exe.wstring() + L"\" --crash \"" + dir.wstring() + L"\" " + wide_stage;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &process), "CreateProcess failed");
    const auto waited = WaitForSingleObject(process.hProcess, 20000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 99);
    DWORD code = 0; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    check(waited == WAIT_OBJECT_0 && code == 77, "crash hook did not execute");
#else
    const auto child = fork(); check(child >= 0, "fork failed");
    if (!child) {
        execl(exe.c_str(), exe.c_str(), "--crash", dir.c_str(), stage.c_str(), nullptr);
        std::_Exit(99);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77,
          "crash hook did not execute");
#endif
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--crash") {
        const std::string stage = argv[3];
        std::atomic<bool> armed{false};
        kv::Options options; options.compaction_table_trigger = 0; options.group_commit_delay_us = 0;
        options.fault_injector = [&](std::string_view point) { if (armed && point == stage) std::_Exit(77); };
        kv::Store db(argv[2], options);
        db.write_batch({kv::Mutation::put("stable", "old"), kv::Mutation::put("gone", "old")});
        db.flush();
        if (stage.substr(0, 4) == "wal." && stage != "wal.checkpoint") {
            armed = true;
            db.write_batch({kv::Mutation::put("a", "atomic"), kv::Mutation::put("b", "atomic")});
        } else {
            db.write_batch({kv::Mutation::put("stable", "new"), kv::Mutation::erase("gone"),
                            kv::Mutation::put("extra", "committed")});
            if (stage.substr(0, 11) == "compaction.") { db.flush(); armed = true; db.compact(); }
            else { armed = true; db.flush(); }
        }
        return 98;
    }
    const auto root = fs::temp_directory_path() /
        ("kv-advanced-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root);
        kv::Options manual; manual.compaction_table_trigger = 0; manual.group_commit_delay_us = 0;
        {
            kv::Store db(root / "compact", manual);
            db.put("key", "one"); db.put("gone", "old"); db.flush();
            {
                const auto old = db.snapshot();
                db.put("key", "two"); db.flush();
                const auto middle = db.snapshot();
                db.erase("gone"); db.put("key", "three"); db.flush();
                db.compact();
                check(db.get("key", old) == "one" && db.get("gone", old) == "old", "old snapshot lost in compaction");
                check(db.get("key", middle) == "two", "middle snapshot lost in compaction");
                check(db.get("key") == "three" && !db.get("gone"), "latest compaction view");
                check(db.metrics().active_snapshots == 2 && db.metrics().sstables == 1, "snapshot registration");
            }
            db.compact();
            check(db.metrics().versions_reclaimed >= 4 && db.metrics().active_snapshots == 0, "version reclamation");
            db.erase("key"); db.compact();
            check(db.metrics().sstables == 0, "last tombstone not reclaimed");
        }
        {
            kv::Store db(root / "compact", manual);
            check(!db.get("key") && !db.get("gone"), "deleted key resurrected");
            const auto before = db.snapshot().sequence();
            db.put("new", "value"); check(db.snapshot().sequence() == before + 1, "empty-table checkpoint lost");
        }
        {
            kv::Store db(root / "ranges", manual);
            db.write_batch({kv::Mutation::put("a", "1"), kv::Mutation::put("b", "2"), kv::Mutation::put("c", "3")});
            const auto old = db.snapshot(); db.flush();
            db.write_batch({kv::Mutation::erase("b"), kv::Mutation::put("c", "4"), kv::Mutation::put("d", "5")});
            check(db.scan("b", "d") == kv::Rows{{"c", "4"}}, "half-open scan");
            check(db.scan("b", "d", old) == kv::Rows{{"b", "2"}, {"c", "3"}}, "snapshot scan");
            check(db.scan("c", "c").empty(), "empty range");
            throws([&] { db.scan("z", "a"); }, "reversed bounds accepted");
            auto it = db.iterator(old); it.seek("b");
            check(it.valid() && it.key() == "b" && it.value() == "2", "iterator seek");
            it.next(); check(it.key() == "c", "iterator next");
            it.next(); check(!it.valid(), "iterator end");
            throws([&] { it.value(); }, "exhausted iterator accepted");
            it.seek(""); check(it.key() == "a", "iterator rewind");
            db.write_batch({kv::Mutation::put("x", "1"), kv::Mutation::erase("x"), kv::Mutation::put("x", "2")});
            check(db.get("x") == "2", "duplicate keys in batch");
        }
        {
            std::string wal;
            std::size_t stable_end;
            {
                kv::Store db(root / "batch-tail", manual);
                db.put("stable", "yes"); stable_end = bytes(root / "batch-tail" / "WAL").size();
                db.write_batch({kv::Mutation::put("a", "one"), kv::Mutation::put("b", "two")});
                wal = bytes(root / "batch-tail" / "WAL");
            }
            for (auto end = stable_end; end <= wal.size(); ++end) {
                replace(root / "batch-tail" / "WAL", std::string_view(wal).substr(0, end));
                kv::Store db(root / "batch-tail", manual);
                check(db.get("stable") == "yes", "stable prefix lost");
                check(db.get("a").has_value() == (end == wal.size()) && db.get("b").has_value() == (end == wal.size()),
                      "partial atomic batch visible");
            }
        }
        {
            auto opts = manual; opts.block_bytes = 128; opts.cache_bytes = 1200;
            kv::Store db(root / "cache", opts);
            for (int i = 0; i < 100; ++i) db.put("key-" + std::to_string(100 + i), std::string(20, 'v'));
            db.flush();
            const auto before = db.metrics();
            db.get("key-150"); db.get("key-150");
            auto after = db.metrics();
            check(after.cache_hits > before.cache_hits && after.disk_block_reads > before.disk_block_reads, "block cache unused");
            check(after.sparse_index_entries < 100 && after.bloom_bytes > 0, "index is not sparse");
            for (int i = 0; i < 100; ++i) check(!db.get("absent-" + std::to_string(i)), "Bloom false positive became result");
            check(db.metrics().bloom_negatives > 80, "Bloom filter ineffective");
            for (int i = 0; i < 100; ++i) check(db.get("key-" + std::to_string(100 + i)).has_value(), "cache eviction lost value");
            check(db.metrics().cache_bytes <= opts.cache_bytes, "cache over budget");
        }
        {
            auto opts = manual; opts.group_commit_delay_us = 10000;
            kv::Store db(root / "group", opts);
            std::atomic<int> ready{0}; std::atomic<bool> go{false}, okay{true};
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; ++t) threads.emplace_back([&, t] {
                ++ready; while (!go) std::this_thread::yield();
                try { for (int i = 0; i < 8; ++i) db.put(std::to_string(t), std::to_string(i)); }
                catch (...) { okay = false; }
            });
            while (ready != 8) std::this_thread::yield(); go = true;
            for (auto& t : threads) t.join();
            check(okay && db.metrics().writes == 64 && db.metrics().wal_syncs < 64 && db.metrics().grouped_requests > 0,
                  "group commit did not combine writes");
        }
        {
            auto opts = manual; opts.group_commit = false; opts.group_commit_delay_us = 1000000;
            kv::Store db(root / "group-disabled", opts);
            std::vector<std::thread> writers;
            std::atomic<bool> okay{true};
            for (int t = 0; t < 4; ++t) writers.emplace_back([&, t] {
                try { for (int i = 0; i < 8; ++i) db.put(std::to_string(t), std::to_string(i)); }
                catch (...) { okay = false; }
            });
            for (auto& writer : writers) writer.join();
            check(okay && db.metrics().writes == 32 && db.metrics().wal_syncs == 32 && db.metrics().grouped_requests == 0,
                  "disabled group commit combined requests");
        }
        {
            // Hold table IO outside the mutex; a foreground write must still finish.
            auto opts = manual; opts.memtable_bytes = 80;
            std::promise<void> reached, release;
            auto released = release.get_future().share();
            std::atomic<bool> once{false};
            opts.fault_injector = [&](std::string_view point) {
                if (point == "table.write" && !once.exchange(true)) { reached.set_value(); released.wait(); }
            };
            kv::Store db(root / "background", opts);
            db.put("big", std::string(100, 'x'));
            check(reached.get_future().wait_for(5s) == std::future_status::ready, "background flush did not start");
            auto write = std::async(std::launch::async, [&] { db.put("foreground", "progress"); });
            const bool progressed = write.wait_for(2s) == std::future_status::ready;
            release.set_value(); write.get();
            check(progressed, "table IO blocked foreground writes"); db.flush();
        }
        {
            kv::Store db(root / "backup-source", manual);
            db.write_batch({kv::Mutation::put("a", "old"), kv::Mutation::put("b", "deleted")}); db.flush();
            db.erase("b"); db.put("a", "current");
            db.backup(root / "backup");
            db.put("a", "later"); db.compact();
            kv::Store::restore(root / "backup", root / "restored");
            kv::Store restored(root / "restored", manual);
            check(restored.get("a") == "current" && !restored.get("b"), "backup consistency");
            restored.put("new", "writable");
            throws([&] { db.backup(root / "backup"); }, "backup overwrote destination");
            throws([&] { kv::Store::restore(root / "missing-backup", root / "invalid-restore"); }, "restore created missing backup");
            check(!fs::exists(root / "missing-backup"), "restore mutated missing source");
        }
        {
            // Pin captured files while compaction retires them during a slow backup.
            auto opts = manual;
            std::promise<void> reached, release;
            auto released = release.get_future().share();
            std::atomic<bool> once{false};
            opts.fault_injector = [&](std::string_view point) {
                if (point == "backup.copy" && !once.exchange(true)) { reached.set_value(); released.wait(); }
            };
            kv::Store db(root / "live-backup", opts); db.put("key", "captured");
            auto copy = std::async(std::launch::async, [&] { db.backup(root / "live-copy"); });
            const bool started = reached.get_future().wait_for(5s) == std::future_status::ready;
            if (!started) { release.set_value(); copy.get(); }
            check(started, "backup did not start");
            try { db.put("key", "newer"); db.compact(); }
            catch (...) { release.set_value(); copy.wait(); throw; }
            release.set_value(); copy.get();
            kv::Store copy_db(root / "live-copy", manual);
            check(copy_db.get("key") == "captured" && db.get("key") == "newer", "backup file pinning failed");
        }
        {
            auto opts = manual; opts.compaction_table_trigger = 2;
            std::promise<void> reached, release;
            auto released = release.get_future().share();
            std::atomic<bool> once{false};
            opts.fault_injector = [&](std::string_view point) {
                if (point == "compaction.merge" && !once.exchange(true)) { reached.set_value(); released.wait(); }
            };
            kv::Store db(root / "auto-compact", opts);
            db.put("key", "old"); db.flush(); db.put("key", "new"); db.flush();
            const bool started = reached.get_future().wait_for(5s) == std::future_status::ready;
            if (!started) release.set_value();
            check(started, "automatic compaction did not start");
            auto writer = std::async(std::launch::async, [&] { db.put("foreground", "progress"); });
            const bool progressed = writer.wait_for(2s) == std::future_status::ready;
            release.set_value(); writer.get(); db.compact();
            check(progressed && db.get("key") == "new" && db.get("foreground") == "progress", "background compaction blocked writes");
        }
        {
            // All waiters must receive a group failure without hanging or losing the lock.
            auto opts = manual; opts.group_commit_delay_us = 10000;
            opts.fault_injector = [](std::string_view point) { if (point == "wal.sync") throw std::runtime_error("sync failed"); };
            kv::Store db(root / "group-failure", opts);
            std::atomic<int> failed{0}, ready{0}; std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; ++t) threads.emplace_back([&, t] {
                ++ready; while (!go) std::this_thread::yield();
                try { db.put(std::to_string(t), "value"); } catch (...) { ++failed; }
            });
            while (ready != 8) std::this_thread::yield(); go = true;
            for (auto& t : threads) t.join();
            check(failed == 8, "group failure did not notify every writer");
        }
        {
            // Original format files are adopted exactly once; later orphan files are ignored.
            const auto dir = root / "legacy";
            { kv::Store db(dir, manual); db.put("key", "old"); db.flush(); }
            fs::remove(dir / "MANIFEST");
            { kv::Store db(dir, manual); check(db.get("key") == "old", "legacy migration failed"); db.erase("key"); db.compact(); }
            // A valid old table copied from another database is not part of this manifest.
            for (const auto& file : fs::directory_iterator(root / "cache"))
                if (file.path().extension() == ".sst") { fs::copy_file(file.path(), dir / "table-999.sst"); break; }
            kv::Store db(dir, manual);
            check(!db.get("key") && !db.get("key-150"), "orphan SSTable resurrected data");
        }
        {
            auto opts = manual; opts.cache_bytes = 0; opts.bloom_bits_per_key = 0; opts.block_bytes = 1;
            kv::Store db(root / "no-cache", opts);
            std::vector<kv::Snapshot> views;
            for (int i = 0; i < 50; ++i) { db.put("same", std::to_string(i)); views.push_back(db.snapshot()); }
            db.flush(); db.compact();
            for (int i = 0; i < 50; ++i) check(db.get("same", views[i]) == std::to_string(i), "large version block lost history");
            check(db.metrics().cache_bytes == 0 && db.metrics().bloom_bytes == 0, "disabled optimizations allocated memory");
        }
        for (const std::string stage : {"wal.write", "wal.sync", "table.record", "table.sync", "table.publish",
                                       "manifest.write", "manifest.sync", "manifest.publish", "flush.after_manifest", "wal.checkpoint"}) {
            const auto dir = root / ("fault-" + stage);
            std::atomic<bool> armed{false};
            auto opts = manual;
            opts.fault_injector = [&](std::string_view point) {
                if (armed && point == stage) throw std::runtime_error("injected disk-full/sync failure: " + stage);
            };
            {
                kv::Store db(dir, opts); db.put("stable", "yes"); db.flush();
                if (stage.substr(0, 4) == "wal." && stage != "wal.checkpoint") {
                    armed = true;
                    throws([&] { db.write_batch({kv::Mutation::put("a", "one"), kv::Mutation::put("b", "two")}); }, "WAL fault not raised");
                } else {
                    db.put("acknowledged", "durable"); armed = true;
                    throws([&] { db.flush(); }, "flush fault not raised");
                }
                throws([&] { db.put("blocked", "value"); }, "poisoned Store accepted write");
            }
            kv::Store recovered(dir, manual);
            check(recovered.get("stable") == "yes", "fault lost checkpoint");
            if (stage.substr(0, 4) != "wal." || stage == "wal.checkpoint")
                check(recovered.get("acknowledged") == "durable", "fault lost acknowledged write");
            check(recovered.get("a").has_value() == recovered.get("b").has_value(), "fault split batch");
        }
        for (const std::string stage : {"table.write", "table.record", "table.sync", "table.publish", "manifest.write",
                                       "manifest.sync", "manifest.publish", "flush.after_manifest", "wal.checkpoint",
                                       "compaction.merge", "compaction.before_publish", "compaction.after_manifest",
                                       "wal.after_write", "wal.sync", "wal.after_sync"}) {
            const auto dir = root / ("crash-" + stage);
            crash_child(fs::absolute(argv[0]), dir, stage);
            kv::Store db(dir, manual);
            if (stage.substr(0, 4) == "wal." && stage != "wal.checkpoint") {
                check(db.get("a").has_value() == db.get("b").has_value(), "crash split batch");
                if (stage == "wal.after_sync") check(db.get("a") == "atomic", "synced batch lost");
            } else {
                check(db.get("stable") == "new" && !db.get("gone") && db.get("extra") == "committed",
                      "crash lost committed state");
            }
            db.put("after", "recovery"); db.compact();
        }
        {
            { kv::Store db(root / "bad-manifest", manual); db.put("x", "y"); db.flush(); }
            auto manifest = bytes(root / "bad-manifest" / "MANIFEST"); manifest.back() ^= 1;
            replace(root / "bad-manifest" / "MANIFEST", manifest);
            throws([&] { kv::Store db(root / "bad-manifest"); }, "manifest corruption accepted");
        }
        fs::remove_all(root);
        std::cout << "All advanced storage tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\nTest data: " << root << '\n'; return 1;
    }
}
