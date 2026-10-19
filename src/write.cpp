#include "detail/engine.hpp"
#include <algorithm>
#include <limits>

namespace kv {

void Store::Impl::submit(const std::vector<Mutation>& mutations) {
    if (mutations.empty()) return;
    std::size_t total = mutations.size() == 1 ? 0 : 17;
    for (const auto& m : mutations) {
        if (m.type != Mutation::Type::Put && m.type != Mutation::Type::Erase)
            throw std::invalid_argument("Invalid mutation type");
        if (m.type == Mutation::Type::Erase && !m.value.empty()) throw std::invalid_argument("Erase has a value");
        if (m.key.size() > max_record - 17 || m.value.size() > max_record - 17 - m.key.size())
            throw std::length_error("Record exceeds 64 MiB");
        const auto size = 25 + m.key.size() + m.value.size();
        if (mutations.size() > 1 && size > max_record - total) throw std::length_error("Batch exceeds 64 MiB");
        total += size;
    }
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> guard(mutex);
    healthy();
    Request request{mutations, false, {}};
    writers.push_back(&request);
    // FIFO after admission. Release the mutex during WAL IO so new callers can
    // actually join this queue rather than compete with the last writer for it.
    cv.wait(guard, [&] { return request.done || (!writer_active && (error ||
        (!checkpoint_pending && writers.front() == &request &&
         (!immutable || memory_bytes < options.memtable_bytes)))); });
    if (!request.done && !error) {
        writer_active = true;
        const auto count = options.group_commit ? std::min<std::size_t>(64, writers.size()) : 1;
        try {
            healthy();
            std::vector<std::vector<Record>> groups;
            std::vector<std::string> frames;
            std::uint64_t written_bytes = 0;
            auto next = sequence;
            for (std::size_t i = 0; i < count; ++i) {
                groups.emplace_back();
                for (const auto& m : writers[i]->mutations) {
                    if (next == std::numeric_limits<std::uint64_t>::max()) fail("Sequence exhausted");
                    groups.back().push_back({++next, m.type == Mutation::Type::Erase, m.key, m.value});
                }
                frames.push_back(encode_batch(groups.back()));
                written_bytes += frames.back().size();
            }
            // Only this leader owns WAL IO. Checkpoint publication explicitly
            // waits for writer_active to clear before replacing the WAL handle.
            guard.unlock();
            for (const auto& frame : frames) {
                inject(options.fault_injector, "wal.write");
                wal->write(frame);
                inject(options.fault_injector, "wal.after_write");
            }
            inject(options.fault_injector, "wal.sync");
            wal->sync_data();
            inject(options.fault_injector, "wal.after_sync");
            guard.lock();
            healthy();
            counters.wal_bytes_written += written_bytes;
            ++counters.wal_syncs;
            if (count > 1) counters.grouped_requests += count;
            for (const auto& group : groups) for (const auto& r : group) add(r);
            sequence = next;
            counters.writes += next - (groups.front().front().sequence - 1);
            counters.batches += count;
        } catch (...) {
            if (!guard.owns_lock()) guard.lock();
            if (!error) error = std::current_exception();
        }
        for (std::size_t i = 0; i < count; ++i) {
            writers.front()->error = error;
            writers.front()->done = true;
            writers.pop_front();
        }
        writer_active = false;
        cv.notify_all();
    }
    // A background failure can wake queued writers that were not in a group.
    if (!request.done) {
        writers.erase(std::find(writers.begin(), writers.end(), &request));
        request.error = error;
        cv.notify_all();
    }
    if (request.error) std::rethrow_exception(request.error);
    counters.write_latency_ns += elapsed_ns(start);
}

void Store::Impl::checkpoint_wal() {
    // Atomic replacement preserves all committed active versions. Their old
    // batch envelopes can be flattened because the whole replacement is synced.
    std::vector<const Record*> records;
    for (const auto& pair : memtable) for (const auto& r : pair.second) records.push_back(&r);
    std::sort(records.begin(), records.end(), [](auto a, auto b) { return a->sequence < b->sequence; });
    {
        File output(directory / "WAL.tmp", "wb");
        inject(options.fault_injector, "wal.checkpoint");
        for (const auto* r : records) output.write(encode(*r));
        output.sync();
    }
    wal.reset();
    publish(directory / "WAL.tmp", directory / "WAL");
    wal = std::make_unique<File>(directory / "WAL", "r+b");
    wal->seek(fs::file_size(directory / "WAL"));
}

} // namespace kv
