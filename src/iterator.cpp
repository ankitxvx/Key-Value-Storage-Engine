#include "kv/store.hpp"
#include <algorithm>
#include <stdexcept>

namespace kv {

void Iterator::seek(std::string_view key) {
    position_ = static_cast<std::size_t>(std::lower_bound(rows_.begin(), rows_.end(), key,
        [](const Entry& entry, auto k) { return entry.first < k; }) - rows_.begin());
}
const std::string& Iterator::key() const {
    if (!valid()) throw std::out_of_range("Iterator is exhausted");
    return rows_[position_].first;
}
const std::string& Iterator::value() const {
    if (!valid()) throw std::out_of_range("Iterator is exhausted");
    return rows_[position_].second;
}

} // namespace kv
