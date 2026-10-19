#include "detail/file.hpp"
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif


namespace kv::detail {

File::File(const fs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode;
    while (*mode) wide_mode.push_back(static_cast<wchar_t>(*mode++));
    file_ = _wfopen(path.c_str(), wide_mode.c_str());
#else
    file_ = std::fopen(path.c_str(), mode);
#endif
    if (!file_) fail("Cannot open " + path.string() + ": " + std::strerror(errno));
}

File::~File() { if (file_) std::fclose(file_); }

std::size_t File::read(char* dst, std::size_t n) {
    const auto count = std::fread(dst, 1, n, file_);
    if (std::ferror(file_)) fail("File read failed");
    return count;
}

void File::write(std::string_view bytes) {
    if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size())
        fail("File write failed");
}

void File::sync() {
    if (std::fflush(file_) != 0) fail("File flush failed");
#ifdef _WIN32
    if (_commit(_fileno(file_)) != 0) fail("File sync failed");
#else
    if (::fsync(fileno(file_)) != 0) fail("File sync failed");
#endif
}

void File::sync_data() {
#ifdef __linux__
    if (std::fflush(file_) != 0) fail("File flush failed");
    if (::fdatasync(fileno(file_)) != 0) fail("File data sync failed");
#else
    sync();
#endif
}

void File::seek(std::uint64_t position) {
#ifdef _WIN32
    if (_fseeki64(file_, static_cast<__int64>(position), SEEK_SET) != 0)
#else
    if (fseeko(file_, static_cast<off_t>(position), SEEK_SET) != 0)
#endif
        fail("File seek failed");
}

void File::truncate(std::uint64_t size) {
    if (std::fflush(file_) != 0) fail("File flush failed");
#ifdef _WIN32
    if (_chsize_s(_fileno(file_), size) != 0) fail("File truncate failed");
#else
    if (ftruncate(fileno(file_), static_cast<off_t>(size)) != 0)
        fail("File truncate failed");
#endif
    seek(size);
    sync();
}

DirectoryLock::DirectoryLock(const fs::path& path) {
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) fail("Database is locked or inaccessible");
#else
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) fail("Cannot open database lock");
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd_);
        fail("Database is locked");
    }
#endif
}

DirectoryLock::~DirectoryLock() {
#ifdef _WIN32
    CloseHandle(handle_);
#else
    ::close(fd_);
#endif
}

void sync_directory(const fs::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) fail("Cannot open directory for sync");
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) fail("Directory sync failed");
#else
    (void)path; // Publication uses MoveFileExW with WRITE_THROUGH below.
#endif
}

void publish(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING))
        fail("SSTable publication failed");
#else
    fs::rename(from, to);
    sync_directory(to.parent_path());
#endif
}

void publish_directory(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) fail("Backup publication failed");
#else
    fs::rename(from, to);
#endif
    sync_directory(to.parent_path());
}

} // namespace kv::detail
