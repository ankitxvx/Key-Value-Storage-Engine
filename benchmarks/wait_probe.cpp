#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <numeric>
#include <vector>

// Isolate the exact wait primitive used by the previous collection policy.
int main() {
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mutex);
    std::vector<double> samples;
    for (int i = 0; i < 512; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        cv.wait_for(lock, std::chrono::microseconds(200), [] { return false; });
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "requested_us,samples,mean_us,p50_us,p99_us,max_us\n200,512,"
              << std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size() << ','
              << samples[255] << ',' << samples[506] << ',' << samples.back() << '\n';
}
