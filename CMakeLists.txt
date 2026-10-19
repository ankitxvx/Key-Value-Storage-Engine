cmake_minimum_required(VERSION 3.16)
project(persistent_kv VERSION 1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
add_library(persistent_kv
    src/store.cpp
    src/engine.cpp
    src/write.cpp
    src/read.cpp
    src/background.cpp
    src/backup.cpp
    src/iterator.cpp
    src/file.cpp
    src/codec.cpp
    src/bloom.cpp
    src/sstable.cpp
    src/manifest.cpp
)
target_include_directories(persistent_kv PUBLIC include)
target_link_libraries(persistent_kv PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(persistent_kv PRIVATE /W4 /permissive-)
else()
  target_compile_options(persistent_kv PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(kv_demo examples/demo.cpp)
target_link_libraries(kv_demo PRIVATE persistent_kv)
enable_testing()
add_executable(kv_tests tests/store_test.cpp)
target_link_libraries(kv_tests PRIVATE persistent_kv)
add_test(NAME kv_tests COMMAND kv_tests)
add_executable(kv_advanced_tests tests/advanced_test.cpp)
target_link_libraries(kv_advanced_tests PRIVATE persistent_kv)
add_test(NAME kv_advanced_tests COMMAND kv_advanced_tests)
set_tests_properties(kv_tests kv_advanced_tests PROPERTIES TIMEOUT 120)
add_executable(kv_bench benchmarks/bench.cpp)
target_link_libraries(kv_bench PRIVATE persistent_kv)
