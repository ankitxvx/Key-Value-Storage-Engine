#include "detail/bloom.hpp"

namespace kv::detail {

std::uint64_t Bloom::hash(std::string_view key) {
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

void Bloom::add(std::string_view key) {
    if (bits.empty()) return;
    auto h = hash(key);
    const auto delta = ((h >> 33) | (h << 31)) | 1;
    for (int i = 0; i < 7; ++i, h += delta) {
        const auto bit = h % (bits.size() * 8);
        bits[bit / 8] |= static_cast<unsigned char>(1U << (bit % 8));
    }
}

bool Bloom::may_contain(std::string_view key) const {
    if (bits.empty()) return true;
    auto h = hash(key);
    const auto delta = ((h >> 33) | (h << 31)) | 1;
    for (int i = 0; i < 7; ++i, h += delta) {
        const auto bit = h % (bits.size() * 8);
        if (!(bits[bit / 8] & (1U << (bit % 8)))) return false;
    }
    return true;
}

} // namespace kv::detail
