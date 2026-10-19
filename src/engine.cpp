#include "detail/engine.hpp"
#include <algorithm>
#include <limits>
#include <set>

namespace kv {

Store::Impl::Impl(const fs::path& dir, Options opts) : directory(fs::absolute(dir)), options(std::move(opts)) {
    if (!options.memtable_bytes || !options.block_bytes || options.bloom_bits_per_key > 64 ||
        options.compaction_table_trigger == 1)
        throw std::invalid_argument("Invalid storage options");
    fs::create_directories(directory);
    sync_directory(directory.parent_path());
    lock = std::make_unique<DirectoryLock>(directory / "LOCK");
    const bool has_manifest = fs::exists(directory / "MANIFEST");
    Manifest manifest;
    if (has_manifest) {
        manifest = read_manifest(directory);
        checkpoint = manifest.checkpoint;
        next_id = manifest.next_id;
        for (const auto& name : manifest.names) {
            auto table = load_table(directory / name, options);
            if (table->max_sequence > checkpoint) fail("Table exceeds manifest checkpoint");
            tables.push_back(std::move(table));
        }
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (!table_name(name)) continue;
        const auto id = std::stoull(name.substr(6, name.size() - 10));
        if (id == std::numeric_limits<std::uint64_t>::max()) fail("Table generation exhausted");
        next_id = std::max(next_id, static_cast<std::uint64_t>(id + 1));
        if (!has_manifest) tables.push_back(load_table(entry.path(), options));
    }
    if (!has_manifest) {
        std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) { return a->max_sequence < b->max_sequence; });
        for (const auto& table : tables) checkpoint = std::max(checkpoint, table->max_sequence);
        write_manifest(directory, describe(tables, checkpoint, next_id));
    }
    sequence = checkpoint;
    const auto wal_path = directory / "WAL";
    if (!fs::exists(wal_path)) {
        File created(wal_path, "wb"); created.sync(); sync_directory(directory);
    }
    wal = std::make_unique<File>(wal_path, "r+b");
    std::uint64_t valid_end = 0, previous_seq = 0;
    Record r;
    std::vector<Record> batch;
    while (read_record(*wal, r, valid_end, true, &batch)) {
        if (batch.front().sequence <= previous_seq) fail("Nonmonotonic WAL sequence");
        previous_seq = r.sequence;
        if (r.sequence <= checkpoint) continue;
        if (batch.front().sequence <= checkpoint) fail("Checkpoint splits an atomic batch");
        for (const auto& item : batch) {
            if (item.sequence != sequence + 1) fail("Gap in WAL sequence");
            add(item);
            sequence = item.sequence;
        }
    }
    wal->truncate(valid_end);
    // Only the manifest grants membership. Orphan tables cannot resurrect data.
    std::set<fs::path> live;
    for (const auto& table : tables) live.insert(table->path);
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (table_name(entry.path().filename().string()) && !live.count(entry.path())) {
            std::error_code ignored; fs::remove(entry.path(), ignored);
        }
    }
    worker = std::thread([this] { work(); });
}

Store::Impl::~Impl() {
    { std::lock_guard<std::mutex> guard(mutex); stopping = true; cv.notify_all(); }
    if (worker.joinable()) worker.join();
}

void Store::Impl::healthy() const { if (error) std::rethrow_exception(error); }

void Store::Impl::add(const Record& r) {
    memtable[r.key].push_back(r);
    memory_bytes += 25 + r.key.size() + r.value.size();
}

fs::path Store::Impl::allocate_table() {
    if (next_id == std::numeric_limits<std::uint64_t>::max()) fail("Table generation exhausted");
    return directory / ("table-" + std::to_string(next_id++) + ".sst");
}

std::uint64_t Store::Impl::oldest_snapshot() {
    auto oldest = sequence;
    snapshots.erase(std::remove_if(snapshots.begin(), snapshots.end(), [&](const auto& weak) {
        if (const auto pin = weak.lock()) { oldest = std::min(oldest, *pin); return false; }
        return true;
    }), snapshots.end());
    return oldest;
}

} // namespace kv
