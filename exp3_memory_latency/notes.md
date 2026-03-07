# Experiment 3: Memory Latency Measurement via Pointer Chasing — Notes

## 1. Experimental Design

This experiment empirically measures the access latency at each level of the Apple M2
memory hierarchy — L1 cache, L2 cache, SLC, and DRAM. Apple does not publish these
numbers. We measure them directly.

The core challenge is that two hardware mechanisms actively hide memory latency from
naive benchmarks:

**Hardware prefetcher** — detects sequential or strided access patterns and fetches
data ahead of time. By the time the load executes, the data is already in cache and
true latency is never exposed.

**Out-of-order execution** — overlaps multiple independent memory requests
simultaneously, hiding latency behind other work.

Both are defeated using **pointer chasing**. An array is filled with a random
permutation of indices. Each load's address is the result of the previous load:

```cpp
index = array[index];
```

This creates a strict serial dependency chain — the CPU cannot issue the next load
until the current one completes, and the prefetcher cannot predict the next address
since the pattern is random. Full memory latency is exposed at every step.

The working set size is swept from small (fits in L1) to large (exceeds DRAM page),
crossing each cache boundary. Average latency per access is computed at each size.
The result should show a clear step function with a distinct plateau at each cache level.

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
single-threaded, performance cores.

---

## 2. Hypotheses

### 2.1 Expected Latency Per Level

| Level | Size | Predicted Latency | Reasoning |
|---|---|---|---|
| L1 cache | 64 KB | 4–5 cycles | Physically closest to execution units, small SRAM, single-cycle access path |
| L2 cache | 4 MB | 15–20 cycles | Larger, further from core, additional interconnect latency |
| SLC | 24 MB | 35–50 cycles | Large off-core shared SRAM, traverses the system fabric |
| DRAM | 32 GB | 150–300 cycles | Off-chip, DRAM row activation + memory controller + bus latency |

### 2.2 Reasoning Behind Each Prediction

**L1 (4–5 cycles):** L1 is a small, fast SRAM physically adjacent to the execution
units. The pipeline is designed so that a load-to-use dependency incurs roughly 4
cycles — observed directly in exp2's llvm-mca output where `ldrsb` fed `tbz` with
an 8.6 cycle average wait reflecting multiple overlapping iterations. Single iteration
L1 latency should be 4–5 cycles.

**L2 (15–20 cycles):** L2 is larger and shared across the P-cluster cores. Additional
size means longer tag lookup and the physical distance from the core adds interconnect
latency. A 3–4× multiplier over L1 is typical for modern designs.

**SLC (35–50 cycles):** Apple's System Level Cache is a 24MB on-chip SRAM shared
across CPU, GPU, and Neural Engine. It sits on the system fabric rather than directly
on the CPU die. Crossing the fabric adds latency beyond L2 but it is still on-chip
SRAM — significantly faster than DRAM.

**DRAM (150–300 cycles):** DRAM access requires activating a row in the memory array,
waiting for sense amplifiers to stabilize, transferring data through the memory
controller, and bringing it across the memory bus. Even with Apple's unified memory
architecture, DRAM latency is fundamentally limited by the physics of DRAM operation
and will be 30–60× slower than L1.

### 2.3 Expected Shape of Results

The latency vs working set size plot should show a clear step function:

```
Latency
(cycles)
  300 |                                    .........
      |                                  ..
  100 |                        ..........
      |                      ..
   20 |           ...........
      |         ..
    5 |.........
      +------------------------------------------→
     1KB  64KB  512KB  4MB   24MB   64MB   256MB
                                    Working Set Size
```

Flat regions correspond to cache levels. Steps occur at L1→L2, L2→SLC, and
SLC→DRAM boundaries. The SLC may produce a less distinct step since its behavior
on Apple Silicon is not publicly documented — anomalies will be noted where observed.

---

## 3. Results

*To be filled after running benchmarks.*

---

## 4. Assembly Analysis

*To be filled after inspecting generated assembly.*

---

## 5. Interpretation

*To be filled after results.*

---

## 6. Key Takeaways

*To be filled at conclusion of experiment.*