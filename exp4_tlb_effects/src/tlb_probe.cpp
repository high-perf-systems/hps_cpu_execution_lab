#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <random>

using ull = unsigned long long;
using clock_type = std::chrono::steady_clock;

static inline void escape(void *p){
    asm volatile("" : : "g"(p) : "memory");
}

int main(){

// Build array — one ull per page, page-aligned access
const ull PAGE_SIZE_BYTES = 16384;
const ull ELEMS_PER_PAGE  = PAGE_SIZE_BYTES / sizeof(ull);  // 2048
unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
std::mt19937 gen(seed);

// Array spans many pages
const ull NUM_PAGES  = 32768;   // sweep up to this many pages
const ull ARRAY_SIZE = NUM_PAGES * ELEMS_PER_PAGE;  // one ull per page worth

std::vector<ull> arr(ARRAY_SIZE, 1);  // fill with 1s — non-trivial, non-zero

// Sweep unique pages from small (fits in TLB) to large (exceeds TLB)
const std::vector<ull> page_counts = {
    16, 32, 64, 128, 256,       // TLB likely fits all of these
    512, 1024, 2048,            // approaching TLB capacity
    4096, 8192, 16384, 32768    // exceeding TLB
};

const ull STEPS = 1'000'000;
const int RUNS  = 5;

for (ull num_pages : page_counts)
{

    // Build random permutation of page indices — defeats prefetcher
    std::vector<ull> page_order(num_pages);
    std::iota(page_order.begin(), page_order.end(), 0);
    std::shuffle(page_order.begin(), page_order.end(), gen);

    // warmup
    ull sum = 0;
    for (ull s = 0; s < STEPS; s++) {
        ull page_idx  = page_order[s % num_pages];
        ull elem_idx  = page_idx * ELEMS_PER_PAGE;  // first element of that page
        sum += arr[elem_idx];
    }
    escape(&sum);

    // timed runs
    std::vector<double> results;
    for (int r = 0; r < RUNS; r++) {
        sum = 0;
        auto start = clock_type::now();
        for (ull s = 0; s < STEPS; s++) {
            ull page_idx = page_order[s % num_pages];
            ull elem_idx = page_idx * ELEMS_PER_PAGE;
            sum += arr[elem_idx];
        }
        auto end = clock_type::now();
        escape(&sum);

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      end - start).count();
        results.push_back((double)ns / STEPS);
    }

    double min_ns = *std::min_element(results.begin(), results.end());
    double avg_ns = std::accumulate(results.begin(), results.end(), 0.0) / RUNS;

    ull working_set_mb = (num_pages * PAGE_SIZE_BYTES) / (1024 * 1024);

    std::cout << std::setw(10) << num_pages   << " pages"
              << std::setw(8)  << working_set_mb << " MB"
              << std::setw(16) << std::fixed << std::setprecision(1)
              << min_ns * 3.33 << " cycles(min)"
              << std::setw(16) << avg_ns * 3.33  << " cycles(avg)\n";
}
return 0;
}
