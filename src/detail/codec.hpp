#pragma once
#include "file.hpp"
#include <map>
#include <vector>

namespace kv::detail {
constexpr std::uint32_t max_record = 64 * 1024 * 1024;
struct Record {
    std::uint64_t sequence;
    bool tombstone;
    std::string key;
    std::string value;
};

using MemTable = std::map<std::string, std::vector<Record>, std::less<>>;
void append_u32(std::string& out, std::uint32_t v);
void append_u64(std::string& out, std::uint64_t v);
std::uint32_t u32(const char* p);
std::uint64_t u64(const char* p);
std::uint32_t crc32(std::string_view data);
std::string encode(const Record& record);
std::string encode_batch(const std::vector<Record>& records);
bool read_record(File& file, Record& record, std::uint64_t& offset, bool wal,
                 std::vector<Record>* batch = nullptr);
} // namespace kv::detail
