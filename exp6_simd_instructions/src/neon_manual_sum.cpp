// Experiment 6 — Case 3: hand-written ARM NEON reduction sum
//
// Explicitly controls every aspect of the vectorized loop:
//   - 8 independent accumulator vectors (vs compiler's 4 in Case 2)
//   - 32 floats per loop iteration = one full 128-byte M2 cache line
//   - vld1q_f32 for 128-bit aligned loads
//   - tree reduction to collapse 8 vectors to one scalar
//
// The prediction: since Case 2 was L2-bandwidth bound (not accumulator-count
// bound), doubling the accumulators should NOT improve throughput — we should
// measure roughly the same cycles/elem as Case 2. This confirms the compiler
// already reached the hardware ceiling for this kernel.
//
// Build:
//   /usr/bin/clang++ -O2 -ffast-math -std=c++17 \
//       src/neon_manual_sum.cpp -o build/neon_manual_sum
//
// Assembly:
//   /usr/bin/clang++ -O2 -ffast-math -S \
//       src/neon_manual_sum.cpp -o build/neon_manual_sum.s

#include <algorithm>
#include <arm_neon.h>
#include <chrono>
#include <iostream>
#include <vector>

static constexpr int    N    = 1 << 20;   // 1M floats = 4 MB
static constexpr int    RUNS = 5;
static constexpr double GHZ  = 3.33;

static inline void escape(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

int main() {
    std::vector<float> arr(N);
    for (int i = 0; i < N; i++)
        arr[i] = 1.0f + (float)(i & 7) * 0.001f;
    escape(arr.data());

    // Warmup — bring array into L2 cache
    {
        float32x4_t w = vdupq_n_f32(0.0f);
        for (int i = 0; i < N; i += 4)
            w = vaddq_f32(w, vld1q_f32(&arr[i]));
        float wf = vaddvq_f32(w);
        escape(&wf);
    }

    double min_ns = 1e18, sum_ns = 0.0;
    float  result = 0.0f;

    for (int r = 0; r < RUNS; r++) {
        // 8 independent accumulators × 4 floats each = 32 floats in flight.
        // Each loop iteration processes exactly 128 bytes — one full M2 cache line.
        // Having 8 independent chains means 8 × 3-cycle latency windows = 24
        // slots, well beyond the 6 needed to saturate 2 NEON units at 3-cycle
        // latency. Compute is definitely not the bottleneck here.
        float32x4_t acc0 = vdupq_n_f32(0.0f);
        float32x4_t acc1 = vdupq_n_f32(0.0f);
        float32x4_t acc2 = vdupq_n_f32(0.0f);
        float32x4_t acc3 = vdupq_n_f32(0.0f);
        float32x4_t acc4 = vdupq_n_f32(0.0f);
        float32x4_t acc5 = vdupq_n_f32(0.0f);
        float32x4_t acc6 = vdupq_n_f32(0.0f);
        float32x4_t acc7 = vdupq_n_f32(0.0f);

        auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < N; i += 32) {
            // 8 vld1q_f32 loads = 8 × 16 bytes = 128 bytes = 1 cache line
            acc0 = vaddq_f32(acc0, vld1q_f32(&arr[i     ]));
            acc1 = vaddq_f32(acc1, vld1q_f32(&arr[i +  4]));
            acc2 = vaddq_f32(acc2, vld1q_f32(&arr[i +  8]));
            acc3 = vaddq_f32(acc3, vld1q_f32(&arr[i + 12]));
            acc4 = vaddq_f32(acc4, vld1q_f32(&arr[i + 16]));
            acc5 = vaddq_f32(acc5, vld1q_f32(&arr[i + 20]));
            acc6 = vaddq_f32(acc6, vld1q_f32(&arr[i + 24]));
            acc7 = vaddq_f32(acc7, vld1q_f32(&arr[i + 28]));
        }

        auto t1 = std::chrono::steady_clock::now();

        // Tree reduction: 8 vectors → 4 → 2 → 1 vector → 1 scalar.
        // Each level is fully parallel (no serial dependency chain).
        float32x4_t s01   = vaddq_f32(acc0, acc1);
        float32x4_t s23   = vaddq_f32(acc2, acc3);
        float32x4_t s45   = vaddq_f32(acc4, acc5);
        float32x4_t s67   = vaddq_f32(acc6, acc7);
        float32x4_t s0123 = vaddq_f32(s01,  s23);
        float32x4_t s4567 = vaddq_f32(s45,  s67);
        float32x4_t total = vaddq_f32(s0123, s4567);
        float s = vaddvq_f32(total);   // horizontal sum of 4 lanes

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
