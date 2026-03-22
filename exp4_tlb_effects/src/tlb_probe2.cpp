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
const int RUNS  = 1;
// Eviction array — access this between measurements to flush page table cache
const ull EVICT_SIZE = 64 * 1024 * 1024 / sizeof(ull);  // 64MB
std::vector<ull> evict(EVICT_SIZE, 0);
for (ull num_pages : page_counts)
{
    // Evict previous page table entries from cache
    ull dummy = 0;
    for (ull i = 0; i < EVICT_SIZE; i += ELEMS_PER_PAGE)
        dummy += evict[i];
    escape(&dummy);

    // Fresh random page order — never seen before
    std::vector<ull> page_order(num_pages);
    std::iota(page_order.begin(), page_order.end(), 0);
    std::shuffle(page_order.begin(), page_order.end(), gen);

    // ONE cold run — each page visited exactly once
    ull sum = 0;
    auto start = clock_type::now();
    for (ull s = 0; s < num_pages; s++) {
        ull page_idx = page_order[s];
        ull elem_idx = page_idx * ELEMS_PER_PAGE;
        sum += arr[elem_idx];
    }
    auto end = clock_type::now();
    escape(&sum);

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  end - start).count();
    double ns_per_access = (double)ns / num_pages;

    std::cout << std::setw(10) << num_pages << " pages"
              << std::setw(16) << std::fixed << std::setprecision(1)
              << ns_per_access * 3.33 << " cycles\n";
}
return 0;
}