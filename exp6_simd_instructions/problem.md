# Experiment 6: SIMD Instructions and Auto-Vectorization

## Problem

Modern CPUs can execute a single instruction across multiple data values simultaneously — a paradigm called **SIMD (Single Instruction, Multiple Data)**. On ARM, this capability is implemented through the **NEON** extension, which provides 32 registers of 128 bits each. A single 128-bit NEON register can hold four 32-bit floats (`float32x4_t`), two 64-bit doubles (`float64x2_t`), or a variety of integer widths. A single vector instruction like `fadd.4s` adds four float pairs in the same number of cycles as scalar `fadd` adds one — giving a theoretical **4× throughput multiplier for 32-bit floats** at no extra clock cost.

This experiment measures whether that theoretical multiplier materialises in practice, how effectively the compiler exploits it automatically, and whether hand-written NEON intrinsics can improve on compiler-generated code.

### The Benchmark Kernel

The workload is **reduction sum over a large float array**:

```cpp
float sum = 0.0f;
for (int i = 0; i < N; i++)
    sum += arr[i];
```

This operation is chosen because:

1. **It is compute-bound, not memory-bound** — for N large enough to spill from L1 but fitting in L2 (e.g., 1M floats = 4 MB, within M2's 4 MB L2), every element is loaded exactly once and then processed. The memory access pattern is a linear stride-1 walk, which prefetchers handle well. The bottleneck is arithmetic throughput, not bandwidth.

2. **It has a loop-carried dependency** — `sum` is read and written every iteration. Scalar code produces a strict dependency chain: iteration N cannot start until iteration N−1 completes the `fadd`. This limits scalar throughput to one fadd per latency cycle, not one per throughput cycle.

3. **Vectorization breaks the dependency** — the compiler can split the array into K independent partial sums (one per NEON lane per accumulator register), accumulate them in parallel, and reduce at the end. This eliminates the scalar bottleneck and exposes both SIMD width and instruction-level parallelism simultaneously.

4. **It is simple enough to write in intrinsics** — the hand-written NEON version can be studied instruction by instruction and compared directly against the compiler's output.

### The Apple M2 NEON Pipeline

On the M2 P-core:

- **NEON register width:** 128 bits (4 × float32 per register)
- **`fadd.4s` latency:** ~3 cycles (same as scalar `fadd`)
- **`fadd.4s` throughput:** 2 per cycle (M2 has dual NEON execution units)
- **`fmla.4s` (fused multiply-add) latency:** ~4 cycles, throughput: 2 per cycle

This means for vector floating-point addition:
- With one accumulator vector: throughput is limited to one `fadd.4s` per 3 cycles (latency-bound), processing 4 floats every 3 cycles = **1.33 floats/cycle**
- With 6 independent accumulator vectors: the pipeline stays full — one `fadd.4s` per 0.5 cycles (throughput-bound at 2/cycle), processing 4 floats every 0.5 cycles = **8 floats/cycle**
- Scalar with one accumulator: 1 float per 3 cycles = **0.33 floats/cycle**

The theoretical peak speedup from scalar (1 accumulator) to fully-pipelined SIMD (6+ accumulators) is approximately **24×** — 8× from SIMD width + ILP over 0.33 scalar floats/cycle.

In practice the limit is memory bandwidth and the number of accumulators the compiler generates.

## Core Question

> **On Apple M2, how close does the compiler's auto-vectorized reduction sum get to the hardware's theoretical throughput ceiling, and can hand-written NEON intrinsics with explicit multiple accumulators close the remaining gap?**

## Three Cases

### Case 1 — Scalar Baseline (`-fno-vectorize`)

The compiler is prohibited from generating vector instructions. The loop produces one `fadd` per iteration with a strict RAW dependency on the accumulator. Expected assembly:

```asm
LBB0_loop:
    ldr  s1, [x0], #4      ; load next float
    fadd s0, s0, s1        ; sum += *ptr (scalar, latency-bound at ~3 cycles)
    subs x2, x2, #1
    b.ne LBB0_loop
```

Bottleneck: `fadd` latency (3 cycles) on the `s0` dependency chain.  
Expected throughput: **~3 cycles per element = ~1.1 ns/element**.

### Case 2 — Auto-Vectorized (compiler default at `-O2`)

With vectorization enabled, the compiler recognises the reduction pattern and transforms the loop to use NEON registers. It will typically:

1. Process 4 (or 8 if it unrolls) floats per iteration using `fadd.4s`
2. Maintain multiple independent accumulator vectors to hide `fadd.4s` latency
3. Reduce the partial sums into a scalar result at the end with `faddp` / `addv`

The key unknown is **how many independent accumulator vectors** the compiler allocates. With one NEON accumulator the loop is still latency-bound — just operating on 4 elements at once (4× faster than scalar). With 6 accumulators, it reaches throughput-bound territory (up to 24× faster than scalar).

### Case 3 — Hand-Written ARM NEON

We write the inner loop explicitly in NEON intrinsics, controlling:
- The number of accumulator registers (targeting 6–8 for full pipeline saturation)
- The load pattern (using `vld1q_f32` for aligned 128-bit loads)
- The reduction sequence (using `vaddq_f32` with `vaddvq_f32` at the end)

```cpp
// Example: 4 independent accumulators × 4 floats = 16 floats in flight
float32x4_t acc0 = vdupq_n_f32(0.0f);
float32x4_t acc1 = vdupq_n_f32(0.0f);
float32x4_t acc2 = vdupq_n_f32(0.0f);
float32x4_t acc3 = vdupq_n_f32(0.0f);

for (int i = 0; i < N; i += 16) {
    acc0 = vaddq_f32(acc0, vld1q_f32(&arr[i]));
    acc1 = vaddq_f32(acc1, vld1q_f32(&arr[i + 4]));
    acc2 = vaddq_f32(acc2, vld1q_f32(&arr[i + 8]));
    acc3 = vaddq_f32(acc3, vld1q_f32(&arr[i + 12]));
}

float32x4_t sum4 = vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3));
float result = vaddvq_f32(sum4);
```

If the compiler in Case 2 already uses 6+ accumulators, Case 3 should match Case 2. If the compiler uses fewer, Case 3 should be measurably faster. In either scenario, we compare assembly to understand exactly what the compiler did.

## Assembly Analysis Plan

For Cases 2 and 3, we generate assembly with:

```bash
clang++ -O2 -S -std=c++17 src/auto_vectorized.cpp -o build/auto_vectorized.s
clang++ -O2 -S -std=c++17 src/neon_manual.cpp -o build/neon_manual.s
```

We inspect:
- How many vector registers are used as accumulators
- Whether the loop body uses `fadd.4s`, `fmla.4s`, or `faddp`
- Loop unroll factor (how many NEON ops per loop iteration)
- The reduction sequence (how partial sums are combined at the end)

If Case 3 outperforms Case 2, we identify the specific limitation in the compiler's output (too few accumulators, no unrolling, poor scheduling) and demonstrate the hand-written fix.

If Cases 2 and 3 match, we escalate: find a problem where the compiler provably fails to vectorize or generates worse vector code — common candidates are:

- Reductions with **aliasing uncertainty** (pointer arguments rather than arrays)
- **Conditional inner loops** where the compiler cannot prove branch-free execution
- **Horizontal reductions with dependencies** the compiler cannot safely reorder under FP non-associativity

## Floating-Point Associativity Note

By default, the compiler will not auto-vectorize a floating-point reduction because FP addition is not associative: `(a + b) + c ≠ a + (b + c)` in general due to rounding. Re-ordering the sum changes the result slightly.

To enable auto-vectorization of the reduction, we either:
- Add `-ffast-math` (allows the compiler to reorder FP operations freely), or
- Use a reduction pattern written in a way the compiler recognises as safe

We will use `-ffast-math` for the auto-vectorized case and document the flag. The hand-written NEON case explicitly reorders the accumulation and is inherently equivalent to using `-ffast-math` for this kernel.

The scalar baseline must also use `-ffast-math` to ensure fair comparison — without it, the compiler cannot even merge loads. We will confirm the flag's effect on the scalar assembly.

## Scope

- Single-threaded execution on one M2 P-core.
- Workload: 1M float elements (4 MB — fits in L2, avoids DRAM bandwidth as the limiting factor).
- Compilation: `-O2 -ffast-math` for Cases 2 and 3; `-O2 -fno-vectorize -ffast-math` for Case 1.
- Comparison metric: nanoseconds per element and cycles per element (using M2 P-core at 3.33 GHz).
- Out of scope: double precision, integer SIMD, masked operations, SVE.
