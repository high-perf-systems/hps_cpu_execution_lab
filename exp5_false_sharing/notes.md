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
with no cache coherence traffic. Expected cost: ~1 cycle per iteration.

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
measure one store, not N counter updates, and completely misrepresent the hardware behavior.

The fix is a **multiplicative recurrence (LCG step)**:

```cpp
counter_A = counter_A * 6364136223846793005ULL + 1442695040888963407ULL;
```

Each iteration's output feeds the next iteration's input as a strict RAW dependency.
The compiler cannot simplify or batch this chain. The CPU must execute exactly N
multiply-add operations in sequence per thread. The counter value changes every
iteration, so there are no dead-store elimination opportunities either.

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
two P-cores for multi-threaded cases, `sudo nice -n -20`.

---

## 2. Hypotheses

### 2.1 Case 1 — Single-threaded Baseline

Both counters reside on the same cache line, which stays hot in L1d for the entire
run. The bottleneck is purely the arithmetic recurrence: a 64-bit multiply followed
by an add. On the M2, a 64-bit multiply has a throughput of 1 per cycle and a
latency of 3 cycles. Since each iteration depends on the previous (RAW chain), the
throughput is limited by latency — approximately 3–4 cycles per iteration.

However, the two counter chains are **independent of each other**. If the loop
interleaves updates to `counter_A` and `counter_B` within the same thread, the
out-of-order engine can execute both chains in parallel, effectively halving the
observed cycles per pair of updates. Expected result: ~1–2 cycles per update.

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
arithmetic cost     :  ~1 cycle  (the LCG step itself)
coherence fetch     : ~22.5 cycles  (SLC round-trip from exp3)
──────────────────────────────────
total per iteration : ~23.5 cycles
```

Both threads are spending most of their time waiting for the cache line to be
transferred, not doing useful arithmetic. The two cores effectively take turns
holding the line — serialising what should be parallel work.

**Expected slowdown vs single-threaded:** ~23.5×

### 2.3 Case 3 — Multi-threaded with Padding

With 128 bytes of padding between counters, each thread owns an independent cache
line. Thread A's writes never touch Thread B's line. There is no invalidation,
no coherence transfer, no SLC round-trip. Each core's line stays in M (Modified)
state in its own L1d for the full run.

Each thread runs at single-threaded arithmetic speed: ~1 cycle per update. Both
threads run simultaneously on separate cores. Since the work is evenly split and
there is zero coherence overhead, the wall-clock time for N updates per thread
should match the single-threaded time for N updates on one counter — a perfect
2× throughput improvement relative to the single-threaded case (which does the same
N updates sequentially on a single core).

```
padded multi-threaded time  ≈  single-threaded time / 2
```

**Expected speedup vs single-threaded:** ~2×
**Expected speedup vs false sharing:**   ~46×

### 2.4 Predicted Latency Summary

| Case | Configuration | Predicted cycles/iter | Relative to single-thread |
|---|---|---|---|
| 1 | Single-threaded, same cache line | ~1 cycle | 1× (baseline) |
| 2 | Multi-threaded, false sharing | ~23.5 cycles | 23.5× slower |
| 3 | Multi-threaded, padded | ~0.5 cycles (2 threads) | 2× faster |

The 0.5 cycles entry for case 3 reflects that the wall-clock time per iteration halves
because two threads are executing simultaneously — each thread does 1 cycle of work
per iteration, but both are happening in parallel.

### 2.5 Expected Shape of Results

```
Time to
complete N iters
per thread

  23.5× |          ████
        |          ████
        |          ████
        |          ████
        |          ████
        |          ████
   1.0× | ████     ████
        | ████     ████
   0.5× | ████     ████     ████
        +─────────────────────────
         Case 1   Case 2   Case 3
        (single) (false   (padded)
                 sharing)
```

The false sharing case should be dramatically slower than both others. The padded
multi-threaded case should be faster than single-threaded, confirming that true
parallelism was achieved and that padding is sufficient to eliminate all coherence
overhead.

### 2.6 What Would Invalidate the Hypothesis

- If case 2 is only 2–5× slower (not ~23.5×), the coherence protocol may be
  using a faster transfer path (e.g., direct core-to-core via L2 interconnect
  rather than SLC). This would indicate the shared coherence point on M2 is closer
  than the SLC, or that the hardware is prefetching the coherence transfer.

- If case 3 is not 2× faster than case 1, thread launch overhead, core
  synchronization, or cache warm-up effects are dominating. The measurement must
  exclude thread creation time and measure only the steady-state inner loop.

- If case 2 and case 3 produce similar times, the counters may have been placed
  on separate cache lines even without explicit padding — due to struct alignment,
  allocator behavior, or compiler-inserted padding. The actual addresses of both
  counters must be printed and verified to be within the same 128-byte-aligned block.

---

## 3. Results

*(to be filled after implementation)*

---

## 4. Assembly Analysis

*(to be filled after implementation)*

---

## 5. Interpretation

*(to be filled after implementation)*

---

## 6. Key Takeaways

*(to be filled after implementation)*
