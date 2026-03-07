#include <iostream>
#include <random>
#include <chrono>
#include <vector>
#include <cstdint>
#include <iomanip>

using ull = unsigned long long;
using clock_type = std::chrono::steady_clock;

static inline void escape(void* p)
{
    asm volatile("" : : "g"(p) : "memory");
}

int main(int argc, char** argv)
{
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::mt19937 gen(seed);
    ull steps = 100'000'000;

    std::vector<ull> sizes = {8000, 150000,500'000, 1'500'000, 2'000'000, 2'500'000,3'000'000, 6'000'000, 25'000'000}; // L1 cache, L2 cache, mid SLC cache, SLC cache, DRAM, deep DRAM

 const int RUNS = 5;

for(ull sz : sizes)
{
    // build array once
    std::vector<ull> indices(sz);
    std::iota(indices.begin(), indices.end(), 0);
    std::shuffle(indices.begin(), indices.end(), gen);

    // warmup
    ull index = 0;
    for(ull i = 0; i < steps; i++) index = indices[index];
    escape(&index);

    // multiple timed runs
    std::vector<double> results;
    for(int r = 0; r < RUNS; r++)
    {
        index = 0;
        auto start = clock_type::now();
        for(ull i = 0; i < steps; i++) index = indices[index];
        auto end = clock_type::now();
        escape(&index);

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
        results.push_back((double)ns / steps);
    }

    // compute min and average
    double min_ns = *std::min_element(results.begin(), results.end());
    double avg_ns = std::accumulate(results.begin(), results.end(), 0.0) / RUNS;

    std::cout << "N = " << std::setw(10) << sz
              << "  min = " << std::fixed << std::setprecision(1) << min_ns * 3.33 << " cycles"
              << "  avg = " << avg_ns * 3.33 << " cycles\n";
}

    return 0;
}

