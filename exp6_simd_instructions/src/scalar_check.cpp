// Experiment 6 — scalar check
//
// Verifies that the floating-point reduction sum is NOT algebraically
// eliminated by the compiler (unlike integer loops which can become sum = N*(N+1)/2).
//
// Build (scalar, no vectorize):
//   clang++ -O2 -fno-vectorize -ffast-math -std=c++17 \
//       src/scalar_check.cpp -o build/scalar_check
//
// Assembly:
//   clang++ -O2 -fno-vectorize -ffast-math -S \
//       src/scalar_check.cpp -o build/scalar_check.s

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

static constexpr int    N    = 1 << 20;   // 1M floats = 4 MB
static constexpr int    RUNS = 5;
static constexpr double GHZ  = 3.33;      // M2 Max P-core

// Prevents the compiler from treating the result as dead code.
// Without this, the loop has no visible side effects at -O2 and
// the entire computation can be silently deleted.
static inline void escape(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

int main() {
    // Fill the array at runtime with values that depend on the loop index.
    // This prevents the compiler from computing the sum as a compile-time constant.
    // Using (i & 7) * 0.001f gives 8 distinct values cycling across the array
    // — non-trivial enough to block constant folding, simple enough to stay
    // latency-dominated once in cache.
    std::vector<float> arr(N);
    for (int i = 0; i < N; i++)
        arr[i] = 1.0f + (float)(i & 7) * 0.001f;
    escape(arr.data());  // treat the entire array as observable output

    // Warmup — brings the array into L2 cache and exercises branch predictors.
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

        escape(&s);   // s must be observable — cannot be dead-code-eliminated
        result = s;

        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
        min_ns = std::min(min_ns, ns);
        sum_ns += ns;
    }

    // Print sum to confirm the loop ran (a constant-folded result would equal
    // exactly N * average_value, detectable by comparing against expected).
    std::cout << "Result (anti-elim check): " << result << "\n";
    std::cout << "N = " << N << "  RUNS = " << RUNS << "\n";
    std::cout << "min = " << min_ns       << " ns/elem"
              << "  =  " << (min_ns * GHZ) << " cycles/elem\n";
    std::cout << "avg = " << (sum_ns / RUNS)       << " ns/elem"
              << "  =  " << (sum_ns / RUNS * GHZ) << " cycles/elem\n";
    return 0;
}
