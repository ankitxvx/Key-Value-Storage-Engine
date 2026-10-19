#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kv {

struct Options {
    std::size_t memtable_bytes = 4 * 1024 * 1024;
    std::size_t block_bytes = 16 * 1024;
    std::size_t cache_bytes = 8 * 1024 * 1024;
    std::size_t bloom_bits_per_key = 10; // Zero disables Bloom filters.
    std::size_t compaction_table_trigger = 8; // Zero disables automatic compaction.
    std::uint32_t group_commit_delay_us = 200;
    // Test seam: may throw or terminate the process. Called on caller/worker threads.
    std::function<void(std::string_view)> fault_injector;
};

struct Mutation {
    enum class Type { Put, Erase };
    Type type;
    std::string key;
    std::string value;
    static Mutation put(std::string key, std::string value) {
        return {Type::Put, std::move(key), std::move(value)};
    }
    static Mutation erase(std::string key) { return {Type::Erase, std::move(key), {}}; }
};

using Entry = std::pair<std::string, std::string>;
using Rows = std::vector<Entry>;

// Owns a materialized consistent view; remains usable after Store destruction.
class Iterator {
public:
    explicit Iterator(Rows rows) : rows_(std::move(rows)) {}
    bool valid() const noexcept { return position_ < rows_.size(); }
    void next() noexcept { if (valid()) ++position_; }
    void seek(std::string_view key);
    const std::string& key() const;
    const std::string& value() const;
    Rows::const_iterator begin() const { return rows_.begin(); }
    Rows::const_iterator end() const { return rows_.end(); }
private:
    Rows rows_;
    std::size_t position_ = 0;
};

struct Metrics {
    std::uint64_t writes = 0, batches = 0, reads = 0, scans = 0;
    std::uint64_t wal_syncs = 0, wal_bytes_written = 0, grouped_requests = 0;
    std::uint64_t cache_hits = 0, cache_misses = 0, disk_block_reads = 0;
    std::uint64_t bloom_checks = 0, bloom_negatives = 0;
    std::uint64_t flushes = 0, compactions = 0, versions_reclaimed = 0;
    std::uint64_t write_latency_ns = 0, read_latency_ns = 0;
    std::uint64_t sequence = 0, checkpoint = 0, sstable_bytes = 0;
    std::size_t sstables = 0, sparse_index_entries = 0, bloom_bytes = 0;
    std::size_t cache_bytes = 0, memtable_bytes = 0, active_snapshots = 0;
};

// A snapshot belongs to one open Store instance and pins a logical version.
class Snapshot {
public:
    std::uint64_t sequence() const noexcept { return sequence_; }
private:
    friend class Store;
    Snapshot(std::uint64_t seq, std::shared_ptr<const int> owner,
             std::shared_ptr<const std::uint64_t> pin)
        : sequence_(seq), owner_(std::move(owner)), pin_(std::move(pin)) {}
    std::uint64_t sequence_;
    std::shared_ptr<const int> owner_;
    std::shared_ptr<const std::uint64_t> pin_;
};

// One process per directory. Methods are thread-safe; failed writes may have
// committed. After an I/O write failure, close and reopen the Store.
class Store {
public:
    explicit Store(const std::filesystem::path& directory, Options options = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    void put(std::string_view key, std::string_view value);
    void erase(std::string_view key);
    void write_batch(const std::vector<Mutation>& mutations);
    std::optional<std::string> get(std::string_view key) const;
    std::optional<std::string> get(std::string_view key, const Snapshot& snapshot) const;
    Snapshot snapshot() const;
    void flush();
    void compact();
    // Half-open range [begin, end); nullopt means no upper bound.
    Rows scan(std::string_view begin = {}, std::optional<std::string_view> end = std::nullopt) const;
    Rows scan(std::string_view begin, std::optional<std::string_view> end, const Snapshot& snapshot) const;
    Iterator iterator() const;
    Iterator iterator(const Snapshot& snapshot) const;
    Metrics metrics() const;
    // Destination must not exist. Completed backups are directly openable Stores.
    void backup(const std::filesystem::path& destination);
    static void restore(const std::filesystem::path& backup, const std::filesystem::path& destination);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kv
