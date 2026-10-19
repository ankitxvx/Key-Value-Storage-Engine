#pragma once
#include "kv/store.hpp"
#include "bloom.hpp"
#include "codec.hpp"

namespace kv::detail {
struct Block {
    std::string first_key;
    std::uint64_t offset, end;
};
struct Table {
    fs::path path;
    std::vector<Block> index;
    Bloom bloom;
    std::uint64_t max_sequence = 0, bytes = 0, records = 0;
    bool retired = false;
    ~Table();
};
using Tables = std::vector<std::shared_ptr<Table>>;

std::shared_ptr<Table> load_table(const fs::path& path, const Options& options);
std::shared_ptr<Table> write_table(const fs::path& path, const MemTable& data, const Options& options);
std::vector<Record> read_block(const Table& table, const Block& block);
} // namespace kv::detail
