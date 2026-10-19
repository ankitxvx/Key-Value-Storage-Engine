#include "detail/engine.hpp"

namespace kv {

Store::Store(const fs::path& directory, Options options) : impl_(std::make_unique<Impl>(directory, std::move(options))) {}
Store::~Store() = default;
void Store::put(std::string_view key, std::string_view value) { write_batch({Mutation::put(std::string(key), std::string(value))}); }
void Store::erase(std::string_view key) { write_batch({Mutation::erase(std::string(key))}); }
void Store::write_batch(const std::vector<Mutation>& mutations) { impl_->submit(mutations); }
std::optional<std::string> Store::get(std::string_view key) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->read(key, impl_->sequence);
}
std::optional<std::string> Store::get(std::string_view key, const Snapshot& snapshot) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (snapshot.owner_ != impl_->identity) throw std::invalid_argument("Snapshot belongs to another Store");
    return impl_->read(key, snapshot.sequence_);
}
Snapshot Store::snapshot() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->healthy();
    auto pin = std::make_shared<const std::uint64_t>(impl_->sequence);
    impl_->snapshots.push_back(pin);
    return Snapshot(impl_->sequence, impl_->identity, std::move(pin));
}
void Store::flush() { impl_->flush(); }
void Store::compact() {
    impl_->flush();
    std::unique_lock<std::mutex> guard(impl_->mutex);
    impl_->healthy();
    const auto ticket = ++impl_->compact_requested;
    impl_->cv.notify_all();
    impl_->cv.wait(guard, [&] { return impl_->error || impl_->compact_completed >= ticket; });
    impl_->healthy();
}
Rows Store::scan(std::string_view begin, std::optional<std::string_view> end) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->scan(begin, end, impl_->sequence);
}
Rows Store::scan(std::string_view begin, std::optional<std::string_view> end, const Snapshot& snapshot) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (snapshot.owner_ != impl_->identity) throw std::invalid_argument("Snapshot belongs to another Store");
    return impl_->scan(begin, end, snapshot.sequence_);
}
Iterator Store::iterator() const { return Iterator(scan()); }
Iterator Store::iterator(const Snapshot& snapshot) const { return Iterator(scan({}, std::nullopt, snapshot)); }
Metrics Store::metrics() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto result = impl_->counters;
    result.sequence = impl_->sequence; result.checkpoint = impl_->checkpoint;
    result.sstables = impl_->tables.size(); result.memtable_bytes = impl_->memory_bytes + impl_->immutable_bytes;
    result.cache_bytes = impl_->cache_size;
    result.pending_writers = impl_->writers.size();
    impl_->oldest_snapshot(); result.active_snapshots = impl_->snapshots.size();
    for (const auto& table : impl_->tables) {
        result.sstable_bytes += table->bytes; result.sparse_index_entries += table->index.size();
        result.bloom_bytes += table->bloom.bits.size();
    }
    return result;
}

} // namespace kv
