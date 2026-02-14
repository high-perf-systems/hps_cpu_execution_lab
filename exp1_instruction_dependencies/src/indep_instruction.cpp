#include <chrono>
#include <iostream>
#include <cstdint>

using clock_type = std::chrono::steady_clock;

static inline void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

int main(int argc, char** argv) {

    size_t N = (argc > 1) ? std::stoull(argv[1]) : 100000000;

    uint64_t a = 1, b = 2, c = 3, d = 4;

    // Warm-up
    for (size_t i = 0; i < N; i++) {
        a++; b++; c++; d++;
        a++; b++; c++; d++;
        a++; b++; c++; d++;
        a++; b++; c++; d++;
        a++; b++; c++; d++;
    }

    escape(&a);

    a = b = c = d = 1;

    auto start = clock_type::now();

    for (size_t i = 0; i < N; i++) {
        a += (i&1);
        b += (i&1);
        c += (i&1);
        d += (i&1);
    }

    auto end = clock_type::now();

    escape(&a);

    auto t = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Independent time: " << t.count() << " us\n";
    std::cout << "Result: " << (a + b + c + d) << "\n";
}
