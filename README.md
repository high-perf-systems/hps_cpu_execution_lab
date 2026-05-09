# hps_cpu_execution_lab

A hands-on experimental lab for building intuition about **CPU microarchitecture and
execution behavior** through carefully designed, measurement-driven experiments.

The goal is not algorithmic optimization — it is understanding **why the CPU behaves
the way it does**: how instructions flow through the pipeline, how caches and coherence
protocols interact with code, and how the compiler reshapes source code before any
of that happens. Every conclusion here is backed by a timing measurement, an assembly
listing, or both.

---

## Target Hardware

All experiments run on **Apple M2 Max** (arm64).

| Property | Value |
|---|---|
| P-core frequency | ~3.33 GHz |
| E-core frequency | ~1.5 GHz |
| Cache line size | 128 bytes |
| L1d per P-core | 64 KB |
| L2 per P-cluster | 4 MB |
| SLC (shared last-level) | ~24 MB |
| Page size | 16 KB |
| DRAM | Unified memory (on-package) |

Apple does not publish latency numbers for most of these levels. Where the specs are
absent, this lab measures them directly.

---

## Experiments

| # | Topic | Key Finding |
|---|---|---|
| [1 — ILP](exp1_instruction_dependencies/) | Dependent vs independent instruction chains | M2 achieves 4.27 IPC with 4 independent chains vs 2.60 IPC for a dependent chain; loop-carried dependencies dominate over port pressure |
| [2 — Branch Prediction](exp2_branch_prediction/) | Predictable vs random branches | 8.4× slowdown from ~50% misprediction rate; the compiler eliminated branches in 3 successive attempts via if-conversion before a valid design was found |
| [3 — Memory Latency](exp3_memory_latency/) | Pointer-chasing across the memory hierarchy | L1: 3.8 cy, L2: 16.8 cy, SLC: 22.5 cy, DRAM: ~128 cy; SLC is only 5.7 cycles slower than L2 — behaves like a large L2 extension |
| [4 — TLB Effects](exp4_tlb_effects/) | Isolating TLB miss cost from cache miss cost | M2 TLB holds 512–1024 entries (8–16 MB coverage); warm TLB miss costs ~9 cycles (page tables in L1/L2); cold miss costs ~200 cycles |
| [5 — False Sharing](exp5_false_sharing/) | Cache line contention between threads | 85-cycle coherence penalty per iteration when two threads write to the same cache line; fully eliminated by padding to separate lines |
| [6 — SIMD Vectorisation](exp6_simd_vectorisation/) | Auto-vectorisation vs manual ARM Neon intrinsics | 9.5× speedup over scalar for float32 reduction; compiler auto-vectorisation matched manual Neon (within 0.07%); conditional sum delivered 47× speedup via combined branch elimination + vectorisation |

---

## Empirical Hardware Constants (Apple M2 Max)

These constants were measured directly in this lab and are used to reason about
performance across all experiments.

```
L1d load latency          :   3.8 cycles        (exp3)
L2 load latency           :  16.8 cycles        (exp3)
SLC load latency          :  22.5 cycles        (exp3)
DRAM load latency         :  ~128 cycles        (exp3, 48 MB working set)
Warm TLB miss cost        :   ~9 cycles         (exp4, page tables in L1/L2)
Cold TLB miss cost        :  ~200 cycles        (exp3/exp4 combined)
TLB capacity              :  512–1024 entries   (~8–16 MB coverage)
False sharing penalty     :  ~85 cycles/iter    (exp5, seq_cst, unpadded vs padded)
L2 streaming bandwidth    :  ~31 GB/s           (exp6, single P-core, float32 reduction)
SIMD vectorisation gain   :  ~9.5×              (exp6, scalar vs auto-vec float32 sum)
Branch mispredict penalty :  ~15 cycles         (exp2, exp6 Case 4a decomposition)
Cache line size           :  128 bytes
Page size                 :  16 KB
P-core frequency          :  3.33 GHz
```

---

## Repository Structure

Each experiment directory follows the same layout:

```
expN_<topic>/
    problem.md   — research question, hypothesis, scope
    notes.md     — measurement journey, assembly analysis, interpretation, takeaways
    src/         — minimal C++ source files, one variable changed at a time
```

Other directories:

```
build/           — compiled binaries and .s assembly output (not committed)
design/          — project constraints, philosophy, out-of-scope decisions
```

---

## Building and Running

There is no Makefile. Each experiment is compiled individually using Apple's clang.

```bash
# Standard build (scalar, no vectorisation)
/usr/bin/clang++ -O2 -std=c++17 -fno-vectorize \
    exp<N>_<topic>/src/<file>.cpp -o build/<binary>

# Auto-vectorised build (exp6, Cases 2 and 3)
/usr/bin/clang++ -O2 -std=c++17 -ffast-math \
    exp6_simd_vectorisation/src/simd_bench.cpp -o build/simd_bench

# Multi-threaded (exp5)
/usr/bin/clang++ -O2 -std=c++20 -fno-vectorize -fno-unroll-loops -pthread \
    exp5_false_sharing/src/false_sharing.cpp -o build/false_sharing

# Generate assembly to verify compiler output
/usr/bin/clang++ -O2 -std=c++17 -fno-vectorize -S \
    exp<N>_<topic>/src/<file>.cpp -o build/<file>.s

# Run with elevated priority (reduces scheduler noise, increases P-core probability)
sudo nice -n -20 ./build/<binary>
```

**Why `/usr/bin/clang++` and not `clang++`?** Homebrew installs LLVM with a newer
clang that may conflict with the macOS SDK. Apple's clang at `/usr/bin/clang++` always
resolves to the correct SDK and sysroot. Use Homebrew LLVM only for `llvm-mca` (which
Apple's clang does not ship).

**Why `-O2` and not `-O3`?** `-O3` enables aggressive loop unrolling and other
transformations that obscure the hardware effects being studied. `-O2` gives realistic
optimisation without hiding the signal.

**Why `-ffast-math` for vectorisation experiments?** IEEE 754 mandates that
floating-point addition is non-associative — the compiler cannot reorder additions
without explicit permission. `-ffast-math` grants that permission, allowing the
vectoriser to split a serial reduction into independent partial sums. Without it,
the compiler refuses to auto-vectorise reduction kernels. See exp6 notes for details.

---

## Tooling

| Tool | Purpose | Notes |
|---|---|---|
| `std::chrono::steady_clock` | Wall-clock timing | Warmup + 5 timed runs; report min and average |
| `llvm-mca` | Static pipeline simulation | Scheduler wait, port contention, ROB occupancy; install via `brew install llvm` |
| `objdump -d` / `-S` flag | Assembly inspection | Mandatory before trusting any timing number |
| `sudo nice -n -20` | Elevated process priority | Reduces OS scheduler preemption during long runs |
| `vm_stat` | Memory pressure diagnosis | Used in exp3 to detect macOS compressor interference |
| `<arm_neon.h>` | ARM Neon intrinsics | Used in exp6 for manual SIMD; `float32x4_t`, `vaddq_f32`, `vmaxq_f32`, `vdupq_n_f32` |

**Converting cycles:** `cycles = time_ns × 3.33` (P-core frequency).

**Preventing dead-code elimination:** Use an inline asm barrier to force the compiler
to treat results as observable:

```cpp
static inline void escape(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}
```

---

## Methodology

Every experiment in this lab follows the same discipline:

- **One variable at a time.** Change exactly one thing between paired benchmarks
  (e.g., same data, different stride; same threads, different padding; same kernel,
  different compiler flags).
- **Validate with assembly.** The compiler transforms source code in ways that can
  silently invalidate a benchmark. Always inspect the generated assembly before
  trusting timing numbers. Several experiments required multiple design iterations
  after assembly inspection revealed the benchmark was not measuring the intended effect.
- **Report minimum, not average.** The minimum across multiple runs approximates
  the true hardware latency with minimal OS scheduler noise. The average includes
  interference; the minimum isolates signal.
- **Use warmup passes.** Cold caches and cold branch predictor state contaminate
  the first measurement. Warm up before timing.
- **Reason from first principles.** Every result is explained in terms of the
  hardware mechanisms involved — pipeline stages, cache levels, coherence protocols,
  memory ordering. Numbers without explanations are just numbers.
- **Hypothesis before code.** Every experiment documents predicted values and
  mechanisms before any benchmark is written. Gaps between prediction and measurement
  are always explained — they are where the real learning happens.

---

## Selected Findings Across the Series

The experiments are designed as a connected series. Each one builds on the hardware
constants and mechanisms established by the previous ones.

**The bottleneck always shifts.** In scalar code the bottleneck is the loop-carried
accumulator dependency (exp1). Adding SIMD breaks that dependency and shifts the
bottleneck to L2 bandwidth (exp6). Adding more SIMD accumulators does nothing once
bandwidth-bound — the bottleneck has already moved.

**The compiler is not transparent.** In three separate experiments the benchmark
produced results that were invalid because the compiler had silently eliminated or
transformed the computation being measured — via if-conversion (exp2), algebraic loop
collapsing (exp5), and refusal to auto-vectorise under IEEE 754 constraints (exp6).
Assembly inspection was the only reliable diagnostic.

**Hardware optimisations hide costs that matter.** Store-to-load forwarding hid false
sharing in exp5 until `memory_order_seq_cst` was used. The hardware prefetcher hid TLB
miss cost in the strided benchmark of exp4 until random page ordering was used. The M2's
atomic pipelining hid coherence overhead under relaxed atomics. Every experiment required
defeating at least one hardware hiding mechanism to expose the true cost.

**Apple M2's SLC is the most surprising finding.** At 22.5 cycles — only 5.7 cycles
slower than L2 — the SLC behaves like a large L2 extension rather than a traditional
last-level cache. This directly explains why M2 performs well on large working sets
that would thrash the L2 on competing architectures.

**Branch misprediction and SIMD are orthogonal and multiplicative.** The conditional
sum experiment (exp6, Case 4) demonstrated that branch elimination (~5×) and
vectorisation (~10×) are independent effects that compound: 5 × 10 = 47×, which
matches the measured 46.9× exactly.

**The `-fno-vectorize` flag ran through every experiment until exp6.** Every prior
experiment deliberately disabled SIMD to isolate the effect being studied. Exp6 finally
removed that restriction and measured what was being left on the table: 9.5× for
compute-simple kernels, with the compiler matching hand-written Neon intrinsics within
measurement noise when auto-vectorisation was possible.

---

## Motivation

Modern CPUs are deeply complex systems. Out-of-order execution, branch speculation,
multiple execution ports, multi-level cache hierarchies, hardware prefetchers, SIMD
units, cache coherence protocols, and compiler optimisations all interact in ways
that are non-obvious from source code alone.

Textbooks explain the concepts. This lab builds the intuition by measuring them
directly on real hardware — forming a hypothesis, running the experiment, reading the
assembly, and reconciling the numbers with the mechanism. The loop is:

```
hypothesis → experiment → assembly inspection → measurement → interpretation → repeat
```

The most important lesson across six experiments: what you write in C++ and what the
CPU executes are often fundamentally different things. The compiler, the hardware
scheduler, the store buffer, the SIMD unit, and the cache coherence protocol all
reshape the program before a single nanosecond is measured. Understanding those
transformations — and designing experiments that defeat the hardware's tendency to
hide its own costs — is what this lab is for.