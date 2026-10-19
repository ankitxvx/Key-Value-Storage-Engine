#include "kv/store.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void throws(F action, const char* message) {
    bool caught = false;
    try { action(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}
std::string bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void replace(const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(bool(out), "test file write failed");
}
void run_crash_child(const fs::path& executable, const fs::path& dir) {
#ifdef _WIN32
    std::wstring command = L"\"" + executable.wstring() + L"\" --crash-child \"" + dir.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "spawn failed");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    check(code == 0, "crash child failed");
#else
    const auto child = fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), "--crash-child", dir.c_str(), nullptr);
        std::_Exit(2);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "crash child failed");
#endif
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--crash-child") {
        kv::Store db(argv[2]);
        db.put("disk", "old");
        db.flush();
        db.put("disk", "new");
        db.put("gone", "value");
        db.erase("gone");
        db.put("wal", "durable");
        std::_Exit(0); // Bypass all destructors and C stdio cleanup.
    }
    const auto root = fs::temp_directory_path() /
        ("kv-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root);
        {
            kv::Store db(root / "basic", {90});
            const auto empty = db.snapshot();
            check(!db.get("missing"), "missing key");
            db.put("key", "one");
            const auto first = db.snapshot();
            db.put("key", "two");
            db.put("other", "three"); // automatic flush
            const auto second = db.snapshot();
            db.erase("key");
            db.flush();
            check(!db.get("key"), "tombstone must shadow disk value");
            check(!db.get("key", empty), "empty snapshot");
            check(db.get("key", first) == "one", "first version after flush");
            check(db.get("key", second) == "two", "second version after flush");
            db.put("key", "reborn");
            check(db.get("key") == "reborn", "resurrection");
            db.put("", "");
            check(db.get("").has_value() && db.get("")->empty(), "empty key/value");
            const std::string binary("a\0b", 3);
            db.put(binary, binary);
            db.flush();
            check(db.get(binary) == binary, "binary key/value");
            throws([&] { kv::Store duplicate(root / "basic"); }, "directory lock");
            kv::Store other(root / "other");
            throws([&] { other.get("key", first); }, "foreign snapshot");
        }
        {
            kv::Store db(root / "basic");
            check(db.get("key") == "reborn", "SSTable reopen");
            check(db.get(std::string("a\0b", 3)) == std::string("a\0b", 3), "binary reopen");
        }
        run_crash_child(fs::absolute(argv[0]), root / "crash");
        {
            kv::Store db(root / "crash");
            check(db.get("disk") == "new" && db.get("wal") == "durable", "crash recovery");
            check(!db.get("gone"), "recover delete");
        }
        std::string full_wal;
        std::size_t first_end = 0;
        {
            kv::Store db(root / "torn");
            db.put("stable", "yes");
            first_end = bytes(root / "torn" / "WAL").size();
            db.put("tail", "last");
            full_wal = bytes(root / "torn" / "WAL");
        }
        // Every possible partial prefix of the last frame must be discarded.
        for (auto end = first_end; end < full_wal.size(); ++end) {
            replace(root / "torn" / "WAL", full_wal.substr(0, end));
            {
                kv::Store db(root / "torn");
                check(db.get("stable") == "yes" && !db.get("tail"), "torn WAL recovery");
                db.put("after", "valid");
            }
            kv::Store reopened(root / "torn");
            check(reopened.get("after") == "valid", "append after torn tail");
        }
        {
            auto corrupt = full_wal;
            corrupt.back() ^= 1;
            replace(root / "torn" / "WAL", corrupt);
            throws([&] { kv::Store db(root / "torn"); }, "WAL corruption must be rejected");
        }
        {
            std::string stale;
            {
                kv::Store db(root / "checkpoint");
                db.put("key", "old");
                db.put("key", "new");
                stale = bytes(root / "checkpoint" / "WAL");
                db.flush();
            }
            // Emulate a crash after table publication but before WAL reset.
            replace(root / "checkpoint" / "WAL", stale);
            replace(root / "checkpoint" / "table-999.tmp", "unfinished table");
            kv::Store db(root / "checkpoint");
            check(db.get("key") == "new", "published table plus stale WAL");
            db.put("key", "newest");
            db.flush();
        }
        {
            std::map<std::string, std::string> model;
            std::mt19937 random(42);
            for (int round = 0; round < 4; ++round) {
                kv::Store db(root / "random", {250});
                for (int i = 0; i < 70; ++i) {
                    const auto key = std::to_string(random() % 20);
                    const auto before = model;
                    const auto snapshot = db.snapshot();
                    if (random() % 3 == 0) { db.erase(key); model.erase(key); }
                    else { const auto value = std::to_string(random()); db.put(key, value); model[key] = value; }
                    for (int k = 0; k < 20; ++k) {
                        const auto name = std::to_string(k);
                        const auto expected = model.count(name) ? std::optional<std::string>(model.at(name)) : std::nullopt;
                        const auto old = before.count(name) ? std::optional<std::string>(before.at(name)) : std::nullopt;
                        check(db.get(name) == expected, "random model mismatch");
                        check(db.get(name, snapshot) == old, "random snapshot mismatch");
                    }
                }
            }
        }
        {
            kv::Store db(root / "threads", {300});
            db.put("shared", "initial");
            const auto snapshot = db.snapshot();
            std::atomic<bool> okay{true};
            std::vector<std::thread> workers;
            for (int t = 0; t < 4; ++t) workers.emplace_back([&, t] {
                try {
                    for (int i = 0; i < 20; ++i) {
                        db.put("shared", std::to_string(t));
                        db.put(std::to_string(t), std::to_string(i));
                        if (db.get("shared", snapshot) != "initial") okay = false;
                    }
                } catch (...) { okay = false; }
            });
            for (auto& worker : workers) worker.join();
            check(okay, "concurrent snapshot reads");
            for (int t = 0; t < 4; ++t) check(db.get(std::to_string(t)) == "19", "concurrent writer lost update");
        }
        {
            const auto dir = root / "bad-table";
            { kv::Store db(dir); db.put("x", "y"); db.flush(); }
            for (const auto& file : fs::directory_iterator(dir)) {
                if (file.path().extension() == ".sst") {
                    auto data = bytes(file.path());
                    data.resize(data.size() - 1);
                    replace(file.path(), data);
                }
            }
            throws([&] { kv::Store db(dir); }, "truncated SSTable must be rejected");
        }
        fs::remove_all(root);
        std::cout << "All storage engine tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\nTest data: " << root << '\n';
        return 1;
    }
}
