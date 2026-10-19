#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kv::detail {
namespace fs = std::filesystem;
using Fault = std::function<void(std::string_view)>;
[[noreturn]] inline void fail(const std::string& message) { throw std::runtime_error(message); }
inline void inject(const Fault& fault, std::string_view point) { if (fault) fault(point); }
inline std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
}
} // namespace kv::detail
