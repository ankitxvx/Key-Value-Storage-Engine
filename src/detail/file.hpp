#pragma once
#include "common.hpp"
#include <cstdio>
#include <cstdint>

namespace kv::detail {
// Native file durability and exclusive directory ownership live behind this API.
class File {
public:
    File(const fs::path& path, const char* mode);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    std::size_t read(char* dst, std::size_t n);
    void write(std::string_view bytes);
    void sync();
    void sync_data();
    void seek(std::uint64_t position);
    void truncate(std::uint64_t size);
private:
    std::FILE* file_ = nullptr;
};
class DirectoryLock {
public:
    explicit DirectoryLock(const fs::path& path);
    ~DirectoryLock();
    DirectoryLock(const DirectoryLock&) = delete;
    DirectoryLock& operator=(const DirectoryLock&) = delete;
private:
#ifdef _WIN32
    void* handle_;
#else
    int fd_;
#endif
};
void sync_directory(const fs::path& path);
void publish(const fs::path& from, const fs::path& to);
void publish_directory(const fs::path& from, const fs::path& to);
} // namespace kv::detail
