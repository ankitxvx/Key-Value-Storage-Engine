#include "detail/engine.hpp"
#include <algorithm>

namespace kv {

void Store::Impl::work() noexcept {
    std::unique_lock<std::mutex> guard(mutex);
    try {
        while (true) {
            cv.wait(guard, [&] { return stopping || error || immutable ||
                (!memtable.empty() && (memory_bytes >= options.memtable_bytes || flush_target > checkpoint)) ||
                compact_requested > compact_completed ||
                (options.compaction_table_trigger && tables.size() >= options.compaction_table_trigger); });
            if (stopping || error) return;
            if (!immutable && !memtable.empty() &&
                (memory_bytes >= options.memtable_bytes || flush_target > checkpoint)) {
                immutable = std::make_shared<MemTable>(std::move(memtable));
                memtable.clear();
                immutable_bytes = memory_bytes; memory_bytes = 0;
                immutable_sequence = sequence;
            }
            if (immutable) {
                const auto data = immutable;
                const auto target = immutable_sequence;
                const auto path = allocate_table();
                guard.unlock();
                auto table = write_table(path, *data, options);
                guard.lock();
                healthy();
                auto updated = tables;
                updated.push_back(std::move(table));
                write_manifest(directory, describe(updated, target, next_id), options.fault_injector);
                inject(options.fault_injector, "flush.after_manifest");
                tables = std::move(updated);
                checkpoint = target;
                checkpoint_wal();
                immutable.reset(); immutable_bytes = 0;
                ++counters.flushes;
                cv.notify_all();
                continue;
            }
            const auto ticket = compact_requested;
            const auto inputs = tables;
            const auto floor = oldest_snapshot();
            const auto path = allocate_table();
            guard.unlock();
            inject(options.fault_injector, "compaction.merge");
            MemTable merged;
            std::uint64_t before = 0, after = 0;
            for (const auto& table : inputs) for (const auto& block : table->index)
                for (auto& r : read_block(*table, block)) { merged[r.key].push_back(std::move(r)); ++before; }
            for (auto it = merged.begin(); it != merged.end();) {
                auto& versions = it->second;
                std::sort(versions.begin(), versions.end(), [](const Record& a, const Record& b) { return a.sequence < b.sequence; });
                const auto above = std::upper_bound(versions.begin(), versions.end(), floor,
                    [](auto seq, const Record& r) { return seq < r.sequence; });
                if (above != versions.begin()) {
                    const auto base = above - 1;
                    // All older disk tables are inputs: a base tombstone can be dropped.
                    versions.erase(versions.begin(), base->tombstone ? above : base);
                }
                after += versions.size();
                if (versions.empty()) it = merged.erase(it); else ++it;
            }
            Tables replacement;
            if (!merged.empty()) replacement.push_back(write_table(path, merged, options));
            inject(options.fault_injector, "compaction.before_publish");
            guard.lock();
            healthy();
            write_manifest(directory, describe(replacement, checkpoint, next_id), options.fault_injector);
            inject(options.fault_injector, "compaction.after_manifest");
            tables = std::move(replacement);
            for (const auto& table : inputs) table->retired = true;
            cache.clear(); lru.clear(); cache_size = 0;
            ++counters.compactions;
            counters.versions_reclaimed += before - after;
            compact_completed = ticket;
            cv.notify_all();
        }
    } catch (...) {
        if (!guard.owns_lock()) guard.lock();
        if (!error) error = std::current_exception();
        cv.notify_all();
    }
}

void Store::Impl::flush() {
    std::unique_lock<std::mutex> guard(mutex);
    healthy();
    const auto target = sequence;
    flush_target = std::max(flush_target, target);
    cv.notify_all();
    cv.wait(guard, [&] { return error || checkpoint >= target; });
    healthy();
}

} // namespace kv
