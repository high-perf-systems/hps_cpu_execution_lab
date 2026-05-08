// Experiment 6 — conditional sum, scalar baseline
//
// Kernel: sum of positive elements
//   for (int i = 0; i < N; i++)
//       if (arr[i] > 0.0f) s += arr[i];
//
// Data: Xorshift32 pseudo-random sign, fixed magnitude.
//   ~50% of elements are +1.5f, ~50% are -0.7f.
//   The branch is genuinely unpredictable: M2's predictor cannot learn the
//   pattern, so ~50% of branches mispredict. Each mispredict costs ~12-15
//   cycles on M2. This is the honest worst-case for a branchy scalar loop.
//
// Why no -ffast-math:
//   Without it the compiler cannot reorder FP additions. The conditional
//   accumulation requires both non-associativity (can't split into partial
//   sums) AND a branch (can't prove the branch-free max() equivalence).
//   The compiler generates scalar code. We verify this in the assembly.
//
// Build (forced scalar, our baseline):
//   /usr/bin/clang++ -O2 -fno-vectorize -std=c++17 \
//       src/cond_sum_scalar.cpp -o build/cond_sum_scalar
//
// Build (compiler free to vectorize -- expected: still scalar):
//   /usr/bin/clang++ -O2 -std=c++17 \
//       src/cond_sum_scalar.cpp -o build/cond_sum_compiler
//
// Assembly (forced scalar):
//   /usr/bin/clang++ -O2 -fno-vectorize -S \
//       src/cond_sum_scalar.cpp -o build/cond_sum_scalar.s

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

static constexpr int    N    = 1 << 20;   // 1M floats = 4 MB
static constexpr int    RUNS = 5;
static constexpr double GHZ  = 3.33;

static inline void escape(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

// Xorshift32 — fast, non-repeating for 2^32-1 steps, unpredictable to branch
// predictors. Same seed as cond_sum_neon.cpp so both measure the same data.
static uint32_t xorshift32(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int main() {
    std::vector<float> arr(N);
    {
        uint32_t rng = 0xDEADBEEF;
        for (int i = 0; i < N; i++)
            arr[i] = (xorshift32(rng) & 1) ? 1.5f : -0.7f;
    }
    escape(arr.data());

    // Warmup
    {
        float w = 0.0f;
        for (int i = 0; i < N; i++)
            if (arr[i] > 0.0f) w += arr[i];
        escape(&w);
    }

    double min_ns = 1e18, sum_ns = 0.0;
    float  result = 0.0f;

    for (int r = 0; r < RUNS; r++) {
        float s = 0.0f;

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; i++)
            if (arr[i] > 0.0f) s += arr[i];
        auto t1 = std::chrono::steady_clock::now();

        escape(&s);
        result = s;

        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
        min_ns = std::min(min_ns, ns);
        sum_ns += ns;
    }

    // Expected: ~N/2 * 1.5 = 786432. Both scalar and NEON should be close
    // (small FP rounding differences from different accumulation order).
    std::cout << "Result: " << result << "\n";
    std::cout << "N = " << N << "  RUNS = " << RUNS << "\n";
    std::cout << "min = " << min_ns        << " ns/elem"
              << "  =  " << (min_ns * GHZ) << " cycles/elem\n";
    std::cout << "avg = " << (sum_ns / RUNS)        << " ns/elem"
              << "  =  " << (sum_ns / RUNS * GHZ) << " cycles/elem\n";
    return 0;
}
