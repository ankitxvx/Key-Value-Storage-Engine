#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace kv::detail {
struct Bloom {
    std::vector<unsigned char> bits;
    static std::uint64_t hash(std::string_view key);
    void add(std::string_view key);
    bool may_contain(std::string_view key) const;
};
} // namespace kv::detail
