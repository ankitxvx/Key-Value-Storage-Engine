#include "kv/store.hpp"
#include <iostream>

int main(int argc, char** argv) {
    try {
        kv::Store store(argc > 1 ? argv[1] : "demo-data");
        store.put("language", "C++17");
        const auto snapshot = store.snapshot();
        store.put("language", "C++20");
        store.flush();
        std::cout << "Current: " << *store.get("language") << '\n'
                  << "Snapshot: " << *store.get("language", snapshot) << '\n';
        store.erase("language");
        std::cout << "Deleted: " << !store.get("language").has_value() << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
