#include "detail/sstable.hpp"
#include <algorithm>
#include <cstring>

namespace kv::detail {

constexpr char table_magic[] = "KVST0001";
Table::~Table() {
    if (retired) { std::error_code ignored; fs::remove(path, ignored); }
}
std::shared_ptr<Table> load_table(const fs::path& path, const Options& options) {
    File file(path, "rb");
    char header[16];
    if (file.read(header, sizeof(header)) != sizeof(header) ||
        std::memcmp(header, table_magic, 8) != 0) fail("Invalid SSTable header");
    const auto count = u64(header + 8);
    auto table = std::make_shared<Table>();
    table->path = path;
    table->records = count;
    std::uint64_t offset = 16;
    std::string previous_key;
    std::uint64_t previous_seq = 0;
    std::vector<std::string> keys;
    for (std::uint64_t i = 0; i < count; ++i) {
        Record r;
        const auto start = offset;
        if (!read_record(file, r, offset, false)) fail("Incomplete SSTable");
        if (i && (r.key < previous_key || (r.key == previous_key && r.sequence >= previous_seq)))
            fail("Unsorted SSTable");
        if (!i || r.key != previous_key) {
            keys.push_back(r.key);
            if (table->index.empty() || start - table->index.back().offset >= options.block_bytes) {
                if (!table->index.empty()) table->index.back().end = start;
                table->index.push_back({r.key, start, 0});
            }
        }
        table->max_sequence = std::max(table->max_sequence, r.sequence);
        previous_key = r.key;
        previous_seq = r.sequence;
    }
    if (!table->index.empty()) table->index.back().end = offset;
    char extra;
    if (file.read(&extra, 1)) fail("Trailing SSTable data");
    table->bytes = offset;
    if (options.bloom_bits_per_key && !keys.empty()) {
        table->bloom.bits.resize(std::max<std::size_t>(8, (keys.size() * options.bloom_bits_per_key + 7) / 8));
        for (const auto& key : keys) table->bloom.add(key);
    }
    return table;
}

std::shared_ptr<Table> write_table(const fs::path& path, const MemTable& data, const Options& options) {
    const fs::path temp = path.string() + ".tmp";
    {
        File output(temp, "wb");
        std::uint64_t count = 0;
        for (const auto& pair : data) count += pair.second.size();
        std::string header(table_magic, 8);
        append_u64(header, count);
        inject(options.fault_injector, "table.write");
        output.write(header);
        for (const auto& pair : data)
            for (auto it = pair.second.rbegin(); it != pair.second.rend(); ++it) {
                inject(options.fault_injector, "table.record");
                output.write(encode(*it));
            }
        inject(options.fault_injector, "table.sync");
        output.sync();
    }
    inject(options.fault_injector, "table.publish");
    publish(temp, path);
    return load_table(path, options);
}

std::vector<Record> read_block(const Table& table, const Block& block) {
    File file(table.path, "rb");
    file.seek(block.offset);
    auto offset = block.offset;
    std::vector<Record> result;
    while (offset < block.end) {
        Record r;
        if (!read_record(file, r, offset, false) || offset > block.end) fail("Invalid SSTable block");
        result.push_back(std::move(r));
    }
    return result;
}

} // namespace kv::detail
