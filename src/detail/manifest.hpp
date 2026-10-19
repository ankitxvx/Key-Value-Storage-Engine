#pragma once
#include "sstable.hpp"

namespace kv::detail {
struct Manifest {
    std::uint64_t checkpoint = 0, next_id = 1;
    std::vector<std::string> names;
};
bool table_name(std::string_view name);
void write_manifest(const fs::path& directory, const Manifest& manifest, const Fault& fault = {});
Manifest read_manifest(const fs::path& directory);
Manifest describe(const Tables& tables, std::uint64_t checkpoint, std::uint64_t next_id);
} // namespace kv::detail
