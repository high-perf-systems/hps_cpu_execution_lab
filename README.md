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

---

## Empirical Hardware Constants (Apple M2 Max)

These constants were measured directly in this lab and are used to reason about
performance across all experiments.

```
L1d load latency      :   3.8 cycles   (exp3)
L2 load latency       :  16.8 cycles   (exp3)
SLC load latency      :  22.5 cycles   (exp3)
DRAM load latency     :  ~128 cycles   (exp3, 48 MB working set)
Warm TLB miss cost    :   ~9 cycles    (exp4, page tables in L1/L2)
Cold TLB miss cost    :  ~200 cycles   (exp3/exp4 combined)
TLB capacity          :  512–1024 entries  (~8–16 MB coverage)
False sharing penalty :   ~85 cycles/iter  (exp5, seq_cst, unpadded vs padded)
Cache line size       :  128 bytes
Page size             :  16 KB
P-core frequency      :  3.33 GHz
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
# Standard build
/usr/bin/clang++ -O2 -std=c++17 -fno-vectorize exp<N>_<topic>/src/<file>.cpp \
    -o build/<binary>

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

---

## Tooling

| Tool | Purpose | Notes |
|---|---|---|
| `std::chrono::steady_clock` | Wall-clock timing | Warmup + 5 timed runs; report min and average |
| `llvm-mca` | Static pipeline simulation | Scheduler wait, port contention, ROB occupancy; install via `brew install llvm` |
| `objdump -d` / `-S` flag | Assembly inspection | Mandatory before trusting any timing number |
| `sudo nice -n -20` | Elevated process priority | Reduces OS scheduler preemption during long runs |
| `vm_stat` | Memory pressure diagnosis | Used in exp3 to detect macOS compressor interference |

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
  (e.g., same data, different stride; same threads, different padding).
- **Validate with assembly.** The compiler transforms source code in ways that can
  silently invalidate a benchmark. Always inspect the generated assembly before
  trusting timing numbers.
- **Report minimum, not average.** The minimum across multiple runs approximates
  the true hardware latency with minimal OS scheduler noise. The average includes
  interference; the minimum isolates signal.
- **Use warmup passes.** Cold caches and cold branch predictor state contaminate
  the first measurement. Warm up before timing.
- **Reason from first principles.** Every result is explained in terms of the
  hardware mechanisms involved — pipeline stages, cache levels, coherence protocols.
  Numbers without explanations are just numbers.

---

## Motivation

Modern CPUs are deeply complex systems. Out-of-order execution, branch speculation,
multiple execution ports, multi-level cache hierarchies, hardware prefetchers, and
compiler optimisations all interact in ways that are non-obvious from source code alone.

Textbooks explain the concepts. This lab builds the intuition by measuring them
directly on real hardware — forming a hypothesis, running the experiment, reading the
assembly, and reconciling the numbers with the mechanism. The loop is:

```
hypothesis → experiment → assembly inspection → measurement → interpretation → repeat
```

The most important lesson so far: what you write in C++ and what the CPU executes are
often fundamentally different things. The compiler, the hardware scheduler, the store
buffer, and the cache coherence protocol all reshape the program before a single
nanosecond is measured. Understanding those transformations is what this lab is for.
