// Experiment 6 — Case 4b: hand-written ARM NEON conditional sum (branch-free)
//
// Kernel: sum of positive elements, NO branch.
//   vmaxq_f32(v, zero) clamps each lane to 0 if it was negative.
//   Adding the clamped value is equivalent to the scalar:
//       if (arr[i] > 0.0f) s += arr[i];
//   because arr[i] is either +1.5f or -0.7f, so max(arr[i], 0) is either
//   1.5f or 0.0f — exactly what the conditional branch would have accumulated.
//
// Why this beats scalar:
//   The scalar kernel stalls ~12-15 cycles per mispredict on M2. With ~50%
//   mispredict rate on Xorshift32 data, that is ~6 extra cycles per element ON
//   TOP of the load + compare + add cost. vmaxq_f32 removes the conditional
//   entirely — the CPU sees a straight stream of load + max + add with no
//   control-flow hazards. 8 independent accumulators keep 8 NEON pipelines busy.
//
// vmaxq_f32 explanation:
//   float32x4_t vmaxq_f32(float32x4_t a, float32x4_t b)
//   → emits:  fmax.4s  v_dst, v_a, v_b
//   → result: each of the 4 lanes = max(a_lane, b_lane), independently.
//   With b = {0,0,0,0}, negative lanes become 0, positive lanes pass through.
//   Latency on M2: 2 cycles (vs 3 for fadd). Throughput: 2 per cycle.
//   It costs almost nothing — the bottleneck remains L2 bandwidth.
//
// Data: same Xorshift32 seed (0xDEADBEEF) as cond_sum_scalar.cpp so both
//   kernels process identical data and results are directly comparable.
//
// Build:
//   /usr/bin/clang++ -O2 -std=c++17 \
//       src/cond_sum_neon.cpp -o build/cond_sum_neon
//
// Assembly:
//   /usr/bin/clang++ -O2 -S \
//       src/cond_sum_neon.cpp -o build/cond_sum_neon.s

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

static uint32_t xorshift32(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int main() {
    std::vector<float> arr(N);
    {
        uint32_t rng = 0xDEADBEEF;   // same seed as cond_sum_scalar.cpp
        for (int i = 0; i < N; i++)
            arr[i] = (xorshift32(rng) & 1) ? 1.5f : -0.7f;
    }
    escape(arr.data());

    // Warmup — bring array into L2 cache
    {
        float32x4_t w    = vdupq_n_f32(0.0f);
        float32x4_t zero = vdupq_n_f32(0.0f);
        for (int i = 0; i < N; i += 4)
            w = vaddq_f32(w, vmaxq_f32(vld1q_f32(&arr[i]), zero));
        float wf = vaddvq_f32(w);
        escape(&wf);
    }

    double min_ns = 1e18, sum_ns = 0.0;
    float  result = 0.0f;

    for (int r = 0; r < RUNS; r++) {
        // 8 independent accumulators × 4 floats = 32 floats per iteration.
        // Each iteration loads one full 128-byte M2 cache line.
        float32x4_t acc0 = vdupq_n_f32(0.0f);
        float32x4_t acc1 = vdupq_n_f32(0.0f);
        float32x4_t acc2 = vdupq_n_f32(0.0f);
        float32x4_t acc3 = vdupq_n_f32(0.0f);
        float32x4_t acc4 = vdupq_n_f32(0.0f);
        float32x4_t acc5 = vdupq_n_f32(0.0f);
        float32x4_t acc6 = vdupq_n_f32(0.0f);
        float32x4_t acc7 = vdupq_n_f32(0.0f);

        // A single zero vector shared across all iterations.
        // vdupq_n_f32(0.0f) is hoisted out of the loop by the compiler
        // because it is loop-invariant.
        const float32x4_t zero = vdupq_n_f32(0.0f);

        auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < N; i += 32) {
            // Load 32 floats (128 bytes = 1 cache line), clamp negatives to 0,
            // then accumulate. vmaxq_f32 replaces the branch:
            //   scalar: if (arr[i] > 0) s += arr[i]
            //   vector: acc0 += max(arr[i..i+3], 0)   (lane-wise, no branch)
            float32x4_t v0 = vmaxq_f32(vld1q_f32(&arr[i     ]), zero);
            float32x4_t v1 = vmaxq_f32(vld1q_f32(&arr[i +  4]), zero);
            float32x4_t v2 = vmaxq_f32(vld1q_f32(&arr[i +  8]), zero);
            float32x4_t v3 = vmaxq_f32(vld1q_f32(&arr[i + 12]), zero);
            float32x4_t v4 = vmaxq_f32(vld1q_f32(&arr[i + 16]), zero);
            float32x4_t v5 = vmaxq_f32(vld1q_f32(&arr[i + 20]), zero);
            float32x4_t v6 = vmaxq_f32(vld1q_f32(&arr[i + 24]), zero);
            float32x4_t v7 = vmaxq_f32(vld1q_f32(&arr[i + 28]), zero);

            acc0 = vaddq_f32(acc0, v0);
            acc1 = vaddq_f32(acc1, v1);
            acc2 = vaddq_f32(acc2, v2);
            acc3 = vaddq_f32(acc3, v3);
            acc4 = vaddq_f32(acc4, v4);
            acc5 = vaddq_f32(acc5, v5);
            acc6 = vaddq_f32(acc6, v6);
            acc7 = vaddq_f32(acc7, v7);
        }

        auto t1 = std::chrono::steady_clock::now();

        // Tree reduction: 8 vectors → 4 → 2 → 1 → scalar
        float32x4_t s01   = vaddq_f32(acc0, acc1);
        float32x4_t s23   = vaddq_f32(acc2, acc3);
        float32x4_t s45   = vaddq_f32(acc4, acc5);
        float32x4_t s67   = vaddq_f32(acc6, acc7);
        float32x4_t s0123 = vaddq_f32(s01,  s23);
        float32x4_t s4567 = vaddq_f32(s45,  s67);
        float32x4_t total = vaddq_f32(s0123, s4567);
        float s = vaddvq_f32(total);

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
