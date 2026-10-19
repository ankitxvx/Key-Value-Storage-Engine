#pragma once
#include "manifest.hpp"
#include <condition_variable>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <thread>

namespace kv {
// Private shared state. Each operation family is implemented in its own source.
using namespace detail;
struct Store::Impl {
    fs::path directory;
    Options options;
    std::unique_ptr<DirectoryLock> lock;
    std::unique_ptr<File> wal;
    mutable std::mutex mutex;
    std::condition_variable cv;
    MemTable memtable;
    std::shared_ptr<const MemTable> immutable;
    std::uint64_t immutable_sequence = 0;
    std::size_t immutable_bytes = 0;
    Tables tables;
    std::uint64_t sequence = 0, checkpoint = 0, next_id = 1, flush_target = 0;
    std::uint64_t compact_requested = 0, compact_completed = 0;
    std::size_t memory_bytes = 0;
    std::exception_ptr error;
    bool stopping = false, writer_active = false, checkpoint_pending = false;
    std::thread worker;
    std::shared_ptr<const int> identity = std::make_shared<const int>(0);
    mutable std::vector<std::weak_ptr<const std::uint64_t>> snapshots;
    mutable Metrics counters;
    struct Cached {
        std::pair<std::string, std::uint64_t> key;
        std::shared_ptr<const std::vector<Record>> records;
        std::size_t bytes;
    };
    mutable std::list<Cached> lru;
    mutable std::map<std::pair<std::string, std::uint64_t>, std::list<Cached>::iterator> cache;
    mutable std::size_t cache_size = 0;
    struct Request {
        const std::vector<Mutation>& mutations;
        bool done = false;
        std::exception_ptr error;
    };
    std::deque<Request*> writers;

    Impl(const fs::path& dir, Options opts);
    ~Impl();
    void healthy() const;
    void add(const Record& r);
    fs::path allocate_table();
    std::uint64_t oldest_snapshot();
    void submit(const std::vector<Mutation>& mutations);
    void checkpoint_wal();
    void work() noexcept;
    void flush();
    std::shared_ptr<const std::vector<Record>> block(const Table& table, const Block& part) const;
    static const Record* mem_read(const MemTable& data, std::string_view key, std::uint64_t visible);
    std::optional<std::string> read(std::string_view key, std::uint64_t visible) const;
    Rows scan(std::string_view begin, std::optional<std::string_view> end, std::uint64_t visible) const;
};
} // namespace kv
