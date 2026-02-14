#include <chrono>
#include <iostream>
#include <cstdint>

using clock_type = std::chrono::steady_clock;

// Prevent compiler from optimizing away values
// arm assmebly memory barrier trick
static inline void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

int main(int argc, char** argv) {

    size_t N = (argc > 1) ? std::stoull(argv[1]) : 100000000;

    uint64_t x = 1;

    // Warm-up
    for (size_t i = 0; i < N; i++) {
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
    }

    escape(&x);

    x = 1;

    auto start = clock_type::now();

    for (size_t i = 0; i < N; i++) {
        x += (i&1);
    }

    auto end = clock_type::now();

    escape(&x);

    auto t = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Dependent time: " << t.count() << " us\n";
    std::cout << "Result: " << x << "\n";
}
