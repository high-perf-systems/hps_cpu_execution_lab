# Experiment 5: False Sharing and Cache Coherence Cost — Notes

## 1. Experimental Design

This experiment measures the performance cost of **false sharing** on the Apple M2
and validates that padding eliminates it. False sharing is the most common concurrency
performance bug that is invisible to correctness tools — two threads write independent
variables that happen to share a 128-byte cache line, causing the cache coherence
protocol to treat each write as a conflict.

Three cases are profiled in sequence, each isolating one variable:

**Case 1 — Single-threaded baseline:**
A single thread updates both `counter_A` and `counter_B` in a loop. Both counters sit
adjacent in memory on the same cache line. The thread never yields the line — it stays
in L1d for the entire run. This establishes the cost of the counter update itself,
with no cache coherence traffic.

**Case 2 — Multi-threaded with false sharing:**
Two threads run concurrently on separate P-cores. Thread A updates `counter_A` and
Thread B updates `counter_B`. The counters are still adjacent — same 128-byte cache
line. Every write by either thread invalidates the line in the other core's L1/L2
cache. To write again, the core must first re-acquire the line from a shared cache
level. This is false sharing: the threads are writing logically independent data, but
the coherence protocol cannot distinguish sub-line granularity.

**Case 3 — Multi-threaded with padding:**
Same two-thread setup, but `counter_A` is padded to occupy a full 128-byte cache line
before `counter_B` begins. The two counters now live on separate cache lines. Each
core holds its line in exclusive state and never invalidates the other. True parallelism
is achieved — both threads run at single-threaded speed simultaneously.

**Why simple `counter++` will not work:**

A naive increment loop like:

```cpp
for (int i = 0; i < N; i++) counter_A++;
```

has no dependency between iterations — the compiler can transform this into a single
`counter_A += N`, collapsing N iterations into one operation. The benchmark would
measure one store, not N counter updates, and completely misrepresent the hardware
behavior.

The fix is a **multiplicative recurrence (LCG step)**:

```cpp
counter_A = counter_A * 6364136223846793005ULL + 1442695040888963407ULL;
```

Each iteration's output feeds the next iteration's input as a strict RAW dependency.
The compiler cannot simplify or batch this chain. The CPU must execute exactly N
multiply-add operations in sequence per thread.

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
two P-cores for multi-threaded cases, `sudo nice -n -20`.

---

## 2. Hypotheses

### 2.1 Case 1 — Single-threaded Baseline

Both counters reside on the same cache line, which stays hot in L1d for the entire
run. The bottleneck is purely the arithmetic recurrence: a 64-bit multiply followed
by an add.

With `memory_order_relaxed`, the expected cost would be ~3–4 cycles per iteration —
the store-to-load forwarding path dominates, and the madd latency (~3 cycles) is the
bottleneck. However, as established during the measurement journey (section 3),
`memory_order_relaxed` completely hides false sharing via store-to-load forwarding.
The final implementation requires `memory_order_seq_cst` to expose coherence effects.

With `seq_cst`, the `stlr`/`ldar` barrier pair adds overhead even with no other
threads running — the store must commit to L1d before the next load can proceed.
Expected cost with seq_cst: **~12–15 cycles per iteration**. See section 3.3 for
why seq_cst was necessary.

No coherence traffic. No memory access beyond the initial L1 warm-up.

### 2.2 Case 2 — Multi-threaded with False Sharing

When Thread A writes `counter_A`, the M2 cache coherence protocol marks the cache
line as Modified in Core A's L1. Core B currently holds a Shared or Invalid copy.
On Core B's next write to `counter_B` (same line), the protocol must:

1. Detect that Core A holds the line in Modified state
2. Flush Core A's dirty line to a shared coherence point
3. Transfer the line to Core B so it can perform its write

On Apple Silicon, the shared coherence point between P-cores is the **SLC** — the
same 22.5-cycle cache measured in exp3. Every iteration of each thread incurs:

```
arithmetic cost     :  ~1 cycle     (the LCG step itself)
seq_cst overhead    :  ~8 cycles    (stlr + ldar barrier cost)
coherence fetch     : ~22.5 cycles  (SLC round-trip from exp3)
──────────────────────────────────────────────────────────
total per iteration : ~31.5 cycles
```

Both threads spend most of their time waiting for the cache line to be transferred,
not doing useful arithmetic. The two cores effectively take turns holding the line —
serialising what should be parallel work.

**Expected slowdown vs padded case:** ~23.5 cycles coherence / ~14 cycles baseline ≈ 2.3× above baseline, giving ~31.5 cycles total.

### 2.3 Case 3 — Multi-threaded with Padding

With 128 bytes of padding between counters, each thread owns an independent cache
line. Thread A's writes never touch Thread B's line. There is no invalidation,
no coherence transfer, no SLC round-trip. Each core's line stays in M (Modified)
state in its own L1d for the full run.

Each thread runs at single-threaded arithmetic speed. With seq_cst the expected
cost per iteration is ~14 cycles — matching Case 1 exactly, since the only overhead
is the seq_cst barrier, not coherence traffic.

Note: the original prediction assumed a 2× speedup over the single-threaded case
(both threads running in parallel at relaxed-atomic speed). With seq_cst this
prediction does not hold — the bottleneck is barrier latency, which is a serial
per-iteration cost that parallelism cannot reduce. The actual result is Case 3 ≈
Case 1, not Case 1 / 2.

**Expected result:** ~14 cycles, matching single-threaded baseline.

### 2.4 Predicted Latency Summary

| Case | Configuration | Predicted cycles/iter | Relative to Case 3 |
|---|---|---|---|
| 1 | Single-threaded, seq_cst | ~14 cycles | 1× (baseline) |
| 2 | Multi-threaded, false sharing | ~31 cycles | ~2.3× slower |
| 3 | Multi-threaded, padded | ~14 cycles | 1× (baseline) |

### 2.5 Expected Shape of Results

```
Cycles
per iter

   31  |          ████
       |          ████
       |          ████
   14  | ████     ████     ████
       | ████     ████     ████
       | ████     ████     ████
       +─────────────────────────
        Case 1   Case 2   Case 3
       (single) (false   (padded)
                sharing)
```

### 2.6 What Would Invalidate the Hypothesis

- If Case 2 matches Case 3, store-to-load forwarding is still hiding the coherence
  cost — the memory ordering is not strict enough.
- If Case 3 is significantly slower than Case 1, thread launch overhead or the
  barrier synchronisation cost is dominating the measurement.
- If Cases 2 and 3 differ by less than 2×, the counters may not actually be on the
  same cache line — the actual addresses must be printed and verified.

---

## 3. Results — Measurement Journey

Getting a clean false sharing signal required three distinct implementation attempts.
Each failure uncovered a fundamental hardware mechanism that had to be understood and
worked around before the coherence cost became visible.

### 3.1 Attempt 1 — Relaxed Atomics (Failed: store-to-load forwarding)

The original implementation used `std::atomic<uint64_t>` with `memory_order_relaxed`
for all loads and stores. This is the standard choice for independent per-thread
counters — it provides atomicity without unnecessary ordering constraints.

**Results:**

```
Case 1 — single-threaded baseline
  counter_a:  min = 1.85 ns  =  6.2 cycles

Case 2 — multi-threaded, false sharing
  Thread A:   min = 1.96 ns  =  6.5 cycles
  Thread B:   min = 1.97 ns  =  6.5 cycles

Case 3 — multi-threaded, padded
  Thread A:   min = 1.93 ns  =  6.4 cycles
  Thread B:   min = 1.94 ns  =  6.4 cycles
```

All three cases measured approximately 6.2–6.5 cycles. The expected multi-cycle
slowdown in Case 2 was completely absent.

**Root cause: store-to-load forwarding.**

Assembly inspection revealed that `memory_order_relaxed` on ARM64 compiles to plain
`ldr` / `str` instructions. The timed inner loop for all cases was:

```asm
LBB9_7:
    ldr  x9, [x23]               ; load counter
    madd x9, x9, x20, x21        ; x9 = x9 * LCG_MUL + LCG_INC
    str  x9, [x23]               ; store counter
    subs x8, x8, #1
    b.ne LBB9_7
```

The `str` in iteration N and the `ldr` at the start of iteration N+1 target the
**exact same address**. The CPU's store buffer recognises this and forwards the value
directly from the store buffer to the next load, bypassing the cache entirely.

Even when Thread B's write invalidates Thread A's L1d cache line between the store
and the next load, Thread A's `ldr` never reaches the cache — it is satisfied from
Thread A's own store buffer. The coherence invalidation happens in hardware but
Thread A pays no visible cost for it.

The 6.2-cycle measurement is **store-buffer-to-load forwarding latency + madd
latency**, not cache latency. This path is completely independent of what other cores
do to the shared cache line — which is why all three cases produced identical numbers.

### 3.2 Attempt 2 — Volatile (Failed: identical code generation to relaxed)

The hypothesis was that `volatile uint64_t` might prevent the compiler from using
the store buffer forwarding path. This was incorrect.

Assembly inspection showed that `volatile` produces byte-for-byte identical loop
bodies to `memory_order_relaxed` — plain `ldr` / `str`, no barriers, no
acquire/release instructions. The only difference was the physical register allocated
(an artefact of register allocation, not a semantic change):

```asm
; relaxed atomic             ; volatile
ldr  x9, [x23]               ldr  x9, [x24]   ← same instruction, different register
madd x9, x9, x20, x21        madd x9, x9, x21, x22
str  x9, [x23]               str  x9, [x24]
subs x8, x8, #1              subs x8, x8, #1
b.ne LBB9_7                  b.ne LBB9_7
```

`volatile` is a **compiler directive** — it prevents the compiler from caching values
in registers or eliminating accesses. It has no effect on hardware store buffer
behaviour. Store-to-load forwarding is a silicon optimisation that operates below the
instruction level and cannot be controlled from C++ source code.

On ARM64, both `volatile T` and `std::atomic<T>` with `memory_order_relaxed` compile
to plain `ldr` / `str` for naturally aligned 8-byte types. Volatile was a dead end.

### 3.3 Attempt 3 — Sequential Consistency (Successful)

The key insight from Attempts 1 and 2: store-to-load forwarding must be broken at the
**instruction** level. This requires an instruction that the hardware cannot satisfy
from the store buffer.

On ARM64, `memory_order_seq_cst` compiles to:
- `stlr` (store-release) for stores
- `ldar` (load-acquire) for loads

On Apple M2, `stlr` is implemented more strictly than the ARM architecture requires.
Apple's microarchitecture treats `stlr` as a full commit barrier — the store is flushed
from the store buffer to L1d before the core proceeds. When the next `ldar` executes,
the store buffer has no pending entry for that address, so the load must go to the
cache. If Thread B has invalidated the line in the interim, Thread A pays the full
coherence re-fetch penalty.

**Results:**

```
Case 1 — single-threaded baseline
  counter_a:  min = 4.37 ns  =  14.6 cycles   avg = 4.39 ns  =  14.6 cycles

Case 2 — multi-threaded, false sharing
  Thread A:   min = 29.92 ns  =  99.6 cycles  avg = 30.06 ns  = 100.1 cycles
  Thread B:   min = 30.03 ns  = 100.0 cycles  avg = 30.13 ns  = 100.3 cycles

Case 3 — multi-threaded, padded
  Thread A:   min = 4.50 ns  =  15.0 cycles   avg = 4.52 ns  =  15.1 cycles
  Thread B:   min = 4.51 ns  =  15.0 cycles   avg = 4.54 ns  =  15.1 cycles
```

False sharing is now clearly visible. Case 2 is **6.8× slower** than Case 3.
Case 3 perfectly matches Case 1 — the padded multi-threaded baseline is identical
to the single-threaded baseline, confirming that padding fully eliminates coherence
overhead and both threads run at full single-threaded speed in parallel.

### 3.4 Hypothesis Validation

| Prediction | Expected | Measured | Verdict |
|---|---|---|---|
| Case 1 baseline (seq_cst) | ~12–15 cycles | 14.6 cycles | ✓ Accurate |
| Case 2 total cost | ~31 cycles | ~100 cycles | ✗ Underestimated — full coherence round trip is more expensive than SLC latency alone (see 5.1) |
| Case 3 matches Case 1 | Yes | Yes (15.0 vs 14.6 cycles) | ✓ |
| Both Case 2 threads slow symmetrically | Yes | Yes (~100 cycles each) | ✓ |
| Padding eliminates all coherence overhead | Yes | Yes | ✓ |

---

## 4. Assembly Analysis

### 4.1 Hot Loop — seq_cst (the measurement that worked)

**All three cases — timed inner loop:**

```asm
LBB0_9:                          ; =>This Inner Loop Header: Depth=2
    ldar x9, [x23]               ; load-acquire: store buffer already drained
                                 ; Cases 1 & 3: reads from L1d (~5 cycles, line in M state)
                                 ; Case 2:      reads from SLC (~22 cycles, line invalidated
                                 ;              by other core between stlr and ldar)
    madd x9, x9, x21, x22        ; x9 = x9 * LCG_MUL + LCG_INC
    stlr x9, [x23]               ; store-release: commits to L1d before proceeding
                                 ; Case 2: triggers Invalidate to other core
    subs x8, x8, #1
    b.ne LBB0_9
```

The loop body is identical across all three cases — four instructions, no
vectorisation, no unrolling. The difference in Case 2 performance is entirely
a hardware coherence effect, not a code difference.

### 4.2 Instruction Behaviour and the Store Buffer

| Instruction | relaxed | seq_cst |
|---|---|---|
| Load | `ldr` — forwarded from own store buffer (~4 cycles) | `ldar` — store buffer drained by prior `stlr`; Cases 1 & 3: reads from L1d (~5 cycles); Case 2: reads from SLC (~22 cycles, line invalidated by other core) |
| Store | `str` — enters store buffer, core moves on immediately | `stlr` — Apple M2: committed to L1d before core proceeds (~5 cycles on critical path); Case 2: sends Invalidate to other core's L1d |

The critical path per iteration with `stlr` / `ldar`:

```
stlr x9, [x23]   →  store committed to L1d              ~5 cycles
                     (Thread B may invalidate the line here, Cases 2 only)
ldar x9, [x23]   →  L1d hit  (Cases 1 & 3)              ~5 cycles
                     OR SLC miss (Case 2, line invalidated) ~22 cycles
madd x9, ...     →  multiply-add                         ~3 cycles
```

In Case 2, Thread B is continuously writing to the same cache line. Thread A's `stlr`
commits the line to its L1d in Modified state. Thread B's concurrent `stlr` sends an
Invalidate. Thread A's `ldar` now misses L1d and pays the full SLC re-fetch cost —
on the critical path, every single iteration.

---

## 5. Interpretation

### 5.1 Decomposing the Measured Costs

The seq_cst results include two separable costs:

**Cost 1 — seq_cst instruction overhead (paid in all three cases):**

```
relaxed baseline (Case 1):    ~6.2 cycles
seq_cst baseline (Case 1):    14.6 cycles
─────────────────────────────────────────
seq_cst overhead per iter:    +8.4 cycles
```

With `relaxed`, store-to-load forwarding costs ~6 cycles total (forwarding latency
~4 cycles + madd ~3 cycles, minus overlap). With `seq_cst`, `stlr` stalls the core
until the store hits L1d (~5 cycles) and `ldar` then reads from L1d (~5 cycles),
adding ~8 cycles per iteration. This cost is entirely single-threaded and has no
connection to inter-thread coherence.

**Cost 2 — False sharing (present only in Case 2):**

```
seq_cst padded (Case 3):        15.0 cycles   ← matches Case 1 baseline ✓
seq_cst false sharing (Case 2): 100.0 cycles
────────────────────────────────────────────
Pure false sharing penalty:     +85 cycles over padded baseline
```

Since Cases 2 and 3 both pay the same seq_cst overhead, the 85-cycle gap between
them is pure cache coherence cost. This is larger than a single SLC round-trip (22.5
cycles) because the coherence protocol involves multiple steps: Thread A's `stlr`
sends an Invalidate, the Invalidate is acknowledged by Thread B, Thread B's `stlr`
completes, the line is transferred via the SLC to Thread A, and Thread A's `ldar`
can finally proceed. This multi-step handshake compounds on both sides of the
ownership transfer, producing a ~85-cycle total per-iteration cost.

**Complete cost breakdown:**

```
                              Cycles   Attribution
──────────────────────────────────────────────────────────────
Case 1  (seq_cst, 1 thread):    14.6   seq_cst instruction overhead
Case 3  (seq_cst, padded):      15.0   seq_cst overhead, no coherence (matches Case 1 ✓)
Case 2  (seq_cst, false share): 100.0  seq_cst overhead + ~85 cycles coherence ping-pong
──────────────────────────────────────────────────────────────
False sharing penalty:          +85 cycles = 5.7× slowdown over padded baseline
Total measured slowdown Case 2 vs Case 3: 6.8×
```

### 5.2 The seq_cst Overhead Is a Measurement Compromise, Not a Flaw

The seq_cst overhead inflates the single-threaded baseline from 6 to 14.6 cycles.
The false sharing cost is therefore not what you would observe in real application
code using relaxed atomics or plain variables for per-thread counters. The experiment
required seq_cst as an engineering tool to defeat store-to-load forwarding and make
the coherence penalty observable.

This does not invalidate the result. The number to cite for false sharing cost is the
**gap between Case 2 and Case 3 — 85 cycles** — not the raw Case 2 value, because
both cases pay the same seq_cst overhead and the delta isolates the coherence effect
alone. The symmetric slowdown of both Thread A and Thread B in Case 2 (~100 cycles
each) confirms the ping-pong interpretation: neither thread can make progress faster
than the other because both compete for exclusive ownership of the same cache line.

### 5.3 Why Both Threads Slow Down Symmetrically

In Case 2, cache line ownership can only reside on one core at a time. When Thread A's
`stlr` commits the write, the line is in Modified state in Thread A's L1d. Thread B's
next `stlr` triggers an Invalidate to Thread A, then takes the line into Modified state
in Thread B's L1d. Thread A's next `ldar` is now a miss — it must fetch the line back
via the SLC. Since both threads run at the same iteration rate, this ownership transfer
occurs on every iteration in both directions. Both threads pay the same average
re-fetch cost, producing identical ~100-cycle measurements.

In Case 3, each thread permanently owns a distinct cache line. Thread A's line stays
in Modified state in Thread A's L1d for the entire run. No Invalidates are sent.
Both threads execute their loops simultaneously and independently — true parallelism.

### 5.4 The Measured Result Proves False Sharing Exists on Apple M2

The combination of results forms a proof by elimination:

1. **Case 3 matches Case 1 (15.0 vs 14.6 cycles):** The two-thread setup itself adds
   no overhead. Any slowdown in Case 2 is not due to threading infrastructure.

2. **Case 2 is 6.8× slower than Case 3:** The only difference between Cases 2 and 3
   is the position of `counter_b` in memory — 8 bytes after `counter_a` vs 128 bytes
   after. No code logic changes. The sole cause of the 85-cycle penalty is cache
   line layout.

3. **Layout is confirmed at runtime:** The code prints actual addresses and cache line
   indices. Case 2 prints `same 128-byte cache line: YES`. Case 3 prints `same
   128-byte cache line: NO`. The hardware behaves exactly as the MESI model predicts.

### 5.5 Apple M2's `stlr` Is Stricter Than the ARM Specification Requires

The ARM architecture specification permits `ldar` to satisfy its load from the same
core's store buffer when the address matches a pending store (same-address forwarding).
If Apple had implemented this permissively, seq_cst would still have hidden the false
sharing: Thread A's `ldar` would have forwarded from Thread A's own `stlr` and never
touched the cache.

The fact that false sharing is visible with seq_cst proves that Apple's M2
implementation of `stlr` drains the store buffer entry to L1d before the core can
issue the next instruction. This is stricter than architecturally required and reflects
Apple's design choice to expose a TSO-like memory model in practice, simplifying
software porting from x86. This behaviour is M2-specific — on an ARM core that
implements `stlr` permissively, seq_cst alone might not suffice to expose false sharing.

### 5.6 The Architecturally Guaranteed Measurement Is `fetch_add`

seq_cst was sufficient to expose false sharing on this machine. The portable,
architecture-guaranteed approach is `fetch_add`:

```cpp
ctr.fetch_add(step, std::memory_order_relaxed);
```

On ARMv8.1+, this compiles to `ldadd` — a single instruction that atomically reads
and writes at the cache coherence point. The read and write are fused; there is no
separate store buffer entry that a subsequent load could forward from. Exclusive
ownership of the cache line is required on every iteration regardless of
microarchitecture. `fetch_add` is also the more realistic workload — shared counters
in real multi-threaded programs are typically implemented with atomic RMW operations.

### 5.7 Connection to Exp3 Memory Latency Constants

The 85-cycle false sharing penalty is directly grounded in exp3's measurements:

```
SLC latency (exp3):  22.5 cycles
False sharing delta: ~85 cycles  ≈  multiple SLC round-trips per iteration
```

Each iteration involves Thread A handing off the 128-byte cache line to Thread B
through the SLC, and Thread B handing it back. Exp3 measured the cost of one thread
missing the cache; exp5 measured the cost of two threads fighting over a single line.
Together they form a complete picture of coherence overhead on M2.

### 5.8 Practical Implications for Real Code

False sharing most commonly appears in three patterns:

**Per-thread counters or accumulators** — statistics collection, histogram bins,
progress counters. Fix: use thread-local storage (`thread_local`) or pad each counter
to a full cache line.

**Producer-consumer queues** — the head and tail pointers of a lock-free queue often
sit adjacent in the same struct. One thread writes the head, another writes the tail.
Fix: place head and tail on separate cache lines.

**Object fields accessed by different threads** — a struct where Thread A writes one
field and Thread B writes another. Fix: reorganise the struct or add explicit padding
between hot fields.

The standard portable C++ fix uses the C++17 cache line size constant:

```cpp
#include <new>

struct alignas(std::hardware_destructive_interference_size) PerThreadCounter {
    uint64_t value;
};
```

`std::hardware_destructive_interference_size` is defined in `<new>` since C++17 and
returns the cache line size for the target platform. On Apple M2 this evaluates to
128. Using this constant instead of hardcoding 128 makes the padding portable across
architectures where the cache line size differs.

---

## 6. Key Takeaways

**False sharing is real and measurable on Apple M2 — 85 cycles of coherence overhead
per iteration in the unpadded case.** Two threads updating independent variables packed
into the same 128-byte cache line suffer a 5.7× slowdown over the padded baseline.
This penalty accumulates silently: the code is logically correct, both threads update
different variables, no data race exists. The only symptom is performance degradation.

**Cache line padding is the correct mitigation and works perfectly on M2.** Case 3
measures 15.0 cycles — matching the 14.6-cycle single-threaded baseline within
measurement noise. Placing independent per-thread variables on separate 128-byte cache
lines eliminates all coherence traffic. Both threads run at full single-threaded speed
simultaneously, achieving true parallelism at no extra cost.

**Store-to-load forwarding is a hardware optimisation that completely hides false
sharing from naive benchmarks.** When a load immediately follows a store to the same
address in the same instruction stream, the CPU forwards the stored value from the
store buffer without touching the cache. The cache line may be in Invalid state but
the load never reaches the cache to discover this. This is a hardware property,
invisible to memory ordering annotations and `volatile`.

**`volatile` does not break store-to-load forwarding — it is a compiler directive,
not a hardware barrier.** On ARM64, `volatile uint64_t` and `std::atomic<uint64_t>`
with `memory_order_relaxed` compile to identical `ldr` / `str` instructions for
naturally aligned 8-byte types. Store-to-load forwarding is a silicon feature that
cannot be disabled from C++ source code.

**`memory_order_seq_cst` introduced ~8.4 cycles of per-iteration overhead in the
single-threaded baseline.** The baseline rose from ~6 cycles (relaxed) to 14.6 cycles
(seq_cst). `stlr` stalls the core until the store hits L1d. `ldar` then reads from
L1d rather than the store buffer. The measurement is a compromise: seq_cst was
necessary to expose the coherence cost, but it adds its own overhead. The clean false
sharing number is the **Case 2 minus Case 3 delta: 85 cycles** — both pay identical
seq_cst overhead and the difference isolates pure coherence cost.

**Apple M2's `stlr` is implemented more strictly than the ARM architecture requires.**
The ARM spec permits `ldar` to forward from the same core's pending store buffer for
same-address accesses. Apple's M2 flushes the store buffer entry to L1d at `stlr`,
making the subsequent `ldar` go to cache. This is why seq_cst was sufficient to expose
false sharing on this machine. On an ARM core that implements `stlr` permissively,
the guaranteed portable approach is `fetch_add`, which compiles to the fused `ldadd`
instruction — a single atomic read-modify-write where store buffer forwarding is
structurally impossible.

**The false sharing penalty of ~85 cycles is grounded in exp3's SLC latency of 22.5
cycles.** Once the store buffer forwarding path is removed, each iteration requires
fetching the 128-byte cache line from the SLC after the other core invalidates it.
Exp3 measured the cost of one thread missing the cache; exp5 measured the cost of
two threads contending over a single line. These two experiments together establish
the complete coherence cost model for this machine.