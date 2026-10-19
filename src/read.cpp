#include "detail/engine.hpp"
#include <algorithm>

namespace kv {

std::shared_ptr<const std::vector<Record>> Store::Impl::block(const Table& table, const Block& part) const {
    const auto key = std::make_pair(table.path.filename().string(), part.offset);
    const auto found = cache.find(key);
    if (found != cache.end()) {
        ++counters.cache_hits;
        lru.splice(lru.begin(), lru, found->second);
        return found->second->records;
    }
    ++counters.cache_misses; ++counters.disk_block_reads;
    auto records = std::make_shared<const std::vector<Record>>(read_block(table, part));
    std::size_t size = sizeof(Cached) + key.first.size();
    for (const auto& r : *records) size += sizeof(Record) + r.key.size() + r.value.size();
    if (size <= options.cache_bytes) {
        while (cache_size > options.cache_bytes - size && !lru.empty()) {
            cache_size -= lru.back().bytes; cache.erase(lru.back().key); lru.pop_back();
        }
        lru.push_front({key, records, size}); cache[key] = lru.begin(); cache_size += size;
    }
    return records;
}

const Record* Store::Impl::mem_read(const MemTable& data, std::string_view key, std::uint64_t visible) {
    const auto it = data.find(key);
    if (it == data.end()) return nullptr;
    const auto& versions = it->second;
    auto r = std::upper_bound(versions.begin(), versions.end(), visible,
        [](auto seq, const Record& record) { return seq < record.sequence; });
    return r == versions.begin() ? nullptr : &*--r;
}

std::optional<std::string> Store::Impl::read(std::string_view key, std::uint64_t visible) const {
    healthy();
    const auto start = std::chrono::steady_clock::now();
    ++counters.reads;
    std::optional<std::string> result;
    const Record* r = mem_read(memtable, key, visible);
    if (!r && immutable) r = mem_read(*immutable, key, visible);
    if (r) { if (!r->tombstone) result = r->value; }
    else for (auto table = tables.rbegin(); table != tables.rend(); ++table) {
        ++counters.bloom_checks;
        if (!(*table)->bloom.may_contain(key)) { ++counters.bloom_negatives; continue; }
        const auto& index = (*table)->index;
        auto part = std::upper_bound(index.begin(), index.end(), key,
            [](auto k, const Block& b) { return k < b.first_key; });
        if (part == index.begin()) continue;
        const auto records = block(**table, *--part);
        auto candidate = std::lower_bound(records->begin(), records->end(), key,
            [](const Record& item, auto k) { return item.key < k; });
        const auto key_end = std::upper_bound(candidate, records->end(), key,
            [](auto k, const Record& item) { return k < item.key; });
        candidate = std::lower_bound(candidate, key_end, visible,
            [](const Record& item, auto seq) { return item.sequence > seq; });
        if (candidate != key_end) {
            if (!candidate->tombstone) result = candidate->value;
            break;
        }
    }
    counters.read_latency_ns += elapsed_ns(start);
    return result;
}

Rows Store::Impl::scan(std::string_view begin, std::optional<std::string_view> end, std::uint64_t visible) const {
    healthy(); ++counters.scans;
    if (end && begin > *end) throw std::invalid_argument("Reversed scan bounds");
    std::map<std::string, Record, std::less<>> selected;
    const auto visit = [&](const Record& r) {
        if (r.sequence > visible || r.key < begin || (end && r.key >= *end)) return;
        auto it = selected.find(r.key);
        if (it == selected.end() || it->second.sequence < r.sequence) selected[r.key] = r;
    };
    const auto visit_mem = [&](const MemTable& data) {
        for (auto it = data.lower_bound(begin); it != data.end() && (!end || it->first < *end); ++it)
            for (const auto& r : it->second) visit(r);
    };
    visit_mem(memtable); if (immutable) visit_mem(*immutable);
    for (const auto& table : tables) for (std::size_t i = 0; i < table->index.size(); ++i) {
        const auto& part = table->index[i];
        if (end && part.first_key >= *end) break;
        if (i + 1 < table->index.size() && table->index[i + 1].first_key <= begin) continue;
        for (const auto& r : *block(*table, part)) visit(r);
    }
    Rows rows;
    for (auto& pair : selected) if (!pair.second.tombstone)
        rows.emplace_back(pair.first, std::move(pair.second.value));
    return rows;
}

} // namespace kv
