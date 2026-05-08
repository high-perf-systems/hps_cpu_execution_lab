// Experiment 6 — Case 2: auto-vectorized reduction sum
//
// Identical source to scalar_check.cpp. The only difference is the compile
// flags: -fno-vectorize is removed. The compiler is now free to emit NEON
// vector instructions.
//
// -ffast-math is required: without it the compiler cannot vectorize a
// floating-point reduction. IEEE 754 treats FP addition as non-associative,
// so the compiler is forbidden by default from reordering the accumulation
// (which is what vectorization requires — splitting one serial sum into N
// independent partial sums). -ffast-math grants that permission.
//
// Build:
//   /usr/bin/clang++ -O2 -ffast-math -std=c++17 \
//       src/auto_vec_sum.cpp -o build/auto_vec_sum
//
// Assembly:
//   /usr/bin/clang++ -O2 -ffast-math -S \
//       src/auto_vec_sum.cpp -o build/auto_vec_sum.s

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

static constexpr int    N    = 1 << 20;   // 1M floats = 4 MB
static constexpr int    RUNS = 5;
static constexpr double GHZ  = 3.33;      // M2 Max P-core

static inline void escape(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

int main() {
    std::vector<float> arr(N);
    for (int i = 0; i < N; i++)
        arr[i] = 1.0f + (float)(i & 7) * 0.001f;
    escape(arr.data());

    // Warmup
    float warmup = 0.0f;
    for (int i = 0; i < N; i++) warmup += arr[i];
    escape(&warmup);

    double min_ns = 1e18, sum_ns = 0.0;
    float  result = 0.0f;

    for (int r = 0; r < RUNS; r++) {
        float s = 0.0f;

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; i++)
            s += arr[i];
        auto t1 = std::chrono::steady_clock::now();

        escape(&s);
        result = s;

        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
        min_ns = std::min(min_ns, ns);
        sum_ns += ns;
    }

    std::cout << "Result (anti-elim check): " << result << "\n";
    std::cout << "N = " << N << "  RUNS = " << RUNS << "\n";
    std::cout << "min = " << min_ns        << " ns/elem"
              << "  =  " << (min_ns * GHZ) << " cycles/elem\n";
    std::cout << "avg = " << (sum_ns / RUNS)        << " ns/elem"
              << "  =  " << (sum_ns / RUNS * GHZ) << " cycles/elem\n";
    return 0;
}
