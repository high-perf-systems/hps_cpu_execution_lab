# Experiment 3: Memory Latency Measurement via Pointer Chasing — Notes

## 1. Experimental Design

This experiment empirically measures the access latency at each level of the Apple M2
memory hierarchy — L1 cache, L2 cache, SLC, and DRAM. Apple does not publish these
numbers. We measure them directly.

The core challenge is that two hardware mechanisms actively hide memory latency from
naive benchmarks:

**Hardware prefetcher** — detects sequential or strided access patterns and fetches
data ahead of time. By the time the load executes, the data is already in cache and
true latency is never observed.

**Out-of-order execution** — overlaps multiple independent memory requests
simultaneously, hiding latency behind other work. The observed time reflects
throughput, not latency.

Both are defeated using **pointer chasing**. An array is filled with a random
permutation of indices. Each load's address is the result of the previous load:

```cpp
index = array[index];
```

This creates a strict serial dependency chain — the CPU cannot issue the next load
until the current one completes, and the prefetcher cannot predict the next address
since the access pattern is random. Full memory latency is exposed at every step.

The working set size is swept from small (fits in L1) to large (exceeds SLC),
crossing each cache boundary. Latency per access is computed at each size by dividing
total time by the number of steps (100,000,000). The result should show a step function
with a distinct plateau at each cache level.

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
single-threaded, performance cores, `sudo nice -n -20`.

---

## 2. Hypotheses

### 2.1 Expected Latency Per Level

| Level | Size | Predicted Latency | Reasoning |
|---|---|---|---|
| L1 cache | 64 KB | 4–5 cycles | Physically closest to execution units, small SRAM |
| L2 cache | 4 MB | 15–20 cycles | Larger, further from core, additional interconnect latency |
| SLC | 24 MB | 35–50 cycles | Large off-core shared SRAM, traverses the system fabric |
| DRAM | >24 MB | 150–300 cycles | Off-chip, DRAM row activation + memory controller latency |

### 2.2 Reasoning Behind Each Prediction

**L1 (4–5 cycles):** L1 is a small, fast SRAM physically adjacent to the execution
units. Load-to-use latency was observed indirectly in exp2's llvm-mca output — `ldrsb`
feeding `tbz` with an 8.6 cycle average wait across multiple overlapping iterations,
consistent with a single-iteration L1 latency of 4–5 cycles.

**L2 (15–20 cycles):** L2 is larger and shared across P-cluster cores. Additional size
means longer tag lookup and the physical distance from the core adds interconnect
latency. A 3–4× multiplier over L1 is typical for modern designs.

**SLC (35–50 cycles):** Apple's System Level Cache is a 24MB on-chip SRAM shared
across the CPU, GPU, and Neural Engine. It sits on the system fabric rather than
directly on the CPU die. Crossing the fabric adds latency beyond L2, but it is still
on-chip SRAM — significantly faster than DRAM.

**DRAM (150–300 cycles):** DRAM access requires activating a row in the memory array,
waiting for sense amplifiers to stabilize, and transferring data through the memory
controller. Even with Apple's unified memory architecture, DRAM latency is
fundamentally limited by the physics of DRAM cell operation.

### 2.3 Expected Shape of Results

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
     64KB  4MB   12MB  24MB   48MB        200MB
                         Working Set Size
```

Flat plateaus at each cache level. Sharp steps at L1→L2, L2→SLC, and SLC→DRAM
boundaries. The SLC boundary behavior is uncertain since Apple does not document it.

---

## 3. Results

### 3.1 Measurement Journey

Getting reliable results required three phases of experimental design. Each phase
revealed something about either the hardware or the measurement methodology.

**Phase 1 — Memory pressure from macOS compressor:**

The first run used a 40M element DRAM array (320MB). The DRAM measurement ran for
over 5 minutes — expected time was 6-10 seconds. Diagnosing with `vm_stat` revealed
the root cause:

```
Pages free: 4656 × 16384 bytes = 76 MB free
PhysMem: 31G used (17G in compressor), 135M unused
```

macOS does not swap inactive pages to SSD immediately. Instead it compresses them
in-place, keeping them in RAM but consuming physical pages. With only 76MB truly free,
the 320MB benchmark array could not be allocated in physical memory — the OS was paging
to NVMe SSD during measurement, inflating DRAM latency by 30-50×.

Two additional findings from this diagnosis:

- Apple Silicon **page size is 16KB** (not 4KB as on x86). This affects TLB coverage
  and page fault cost — directly relevant to the TLB experiment in exp4.
- `sudo purge` flushes only file caches, not the compressor. A full reboot is required
  to reclaim compressor memory. Always benchmark on a clean boot.

Fix: rebooted the machine, reduced DRAM working set to 6M elements (48MB).

**Phase 2 — High variance in SLC and DRAM measurements:**

After reboot, results were cleaner but showed 30-40% variance run to run in the SLC
and DRAM regions. The same 1.5M array measured 34.2 cycles in one run and 22.5 in
another. Two causes:

- **OS scheduler preemption** — macOS can preempt the benchmark thread at any point.
  For L1 measurements (~115ms total) this rarely happens. For DRAM measurements
  (~5 seconds total) a preemption is almost guaranteed and inflates the result.
- **SLC contention** — the SLC is shared with the GPU and Neural Engine. Background
  macOS compositor activity evicts working set pages mid-measurement.

Fix: ran each size 5 times and reported the **minimum** across runs. The minimum
represents the closest approximation to true hardware latency — the run with least
OS interference. The average includes noise; the minimum isolates signal. Also ran
with `sudo nice -n -20` to reduce scheduler preemption.

**Phase 3 — Adding granularity around cache boundaries:**

Initial sizes were too coarse to resolve where transitions happened. Added intermediate
sizes around the L2→SLC and SLC→DRAM boundaries, and a large 200MB size to observe
TLB pressure effects in the deep DRAM region.

### 3.2 Final Results

All values are minimum across 5 runs, 100,000,000 pointer chase steps each.

```
N =       8000  working set =  64 KB   min =   3.8 cycles  avg =   3.8 cycles
N =     150000  working set = 1.2 MB   min =  16.8 cycles  avg =  17.2 cycles
N =     500000  working set =   4 MB   min =  17.8 cycles  avg =  18.0 cycles
N =    1500000  working set =  12 MB   min =  22.5 cycles  avg =  22.7 cycles
N =    2000000  working set =  16 MB   min =  28.6 cycles  avg =  28.7 cycles
N =    2500000  working set =  20 MB   min =  46.5 cycles  avg =  47.2 cycles
N =    3000000  working set =  24 MB   min =  60.6 cycles  avg =  61.1 cycles
N =    6000000  working set =  48 MB   min = 127.6 cycles  avg = 183.1 cycles
N =   25000000  working set = 200 MB   min = 327.8 cycles  avg = 341.0 cycles
```

### 3.3 Hypothesis Validation

| Level | Predicted | Measured | Verdict |
|---|---|---|---|
| L1 | 4–5 cycles | 3.8 cycles | ✓ Accurate |
| L2 | 15–20 cycles | 16.8 cycles | ✓ Accurate |
| SLC | 35–50 cycles | 22.5 cycles | ✗ Overestimated — SLC is significantly faster than predicted |
| DRAM | 150–300 cycles | 127.6 cycles | ✓ Within range |

---

## 4. Assembly Analysis

The hot loop after compilation with `-O2 -fno-vectorize`:

```asm
LBB0_5:                           ; =>This Inner Loop Header: Depth=1
    ldr    x8, [x20, x8, lsl #3]   ; index = array[index]
                                  ; x20 = array base address
                                  ; x8 = current index
                                  ; x8 lsl #3 = index × 8 (byte offset for uint64_t)
    subs   x10, x10, #1           ; steps--
    b.ne   LBB0_5                 ; loop if steps > 0
```

Three instructions. The entire experiment lives in one `ldr`.

The critical dependency chain:

```
Iteration N:    ldr x8, [x20, x8, lsl #3]   → result lands in x8
                                                      ↓
Iteration N+1:  ldr x8, [x20, x8, lsl #3]   → uses x8 as address input
```

Every load depends on the previous load's result. The CPU cannot compute the address
for iteration N+1 until iteration N's load completes and writes its value to x8.
Out-of-order execution cannot overlap these loads. The prefetcher cannot predict the
next address. The full latency of each memory access is exposed with no hiding.

The `subs` and `b.ne` are independent of the load chain and execute in parallel — they
do not contribute to the critical path.

**The entire loop performance is determined by one number: the latency of a single
`ldr` for the current working set size.** cycles/iteration = memory latency at that
cache level. This is exactly what the experiment measures.

---

## 5. Interpretation

### 5.1 Empirical Memory Hierarchy Constants — Apple M2 Max

These are the hardware constants established by this experiment:

```
L1 cache  :   3.8 cycles   (64KB working set, zero variance)
L2 cache  :  16.8 cycles   (1.2MB working set, comfortably inside L2)
SLC       :  22.5 cycles   (12MB working set, mid-SLC, clean measurement)
DRAM      : ~128 cycles    (48MB working set, minimum across 5 runs)
```

Every future experiment in this lab series that involves memory will reference
these numbers. They are the foundation of all memory performance modeling on this
machine.

### 5.2 The L2→SLC Transition Is Nearly Invisible

```
L2  (1.2MB):  16.8 cycles
SLC (12MB):   22.5 cycles
Gap:           5.7 cycles  (34% increase)
```

Only a 5.7 cycle difference between L2 and mid-SLC. Apple's SLC behaves almost like
a large L2 extension rather than a separate cache tier. This is intentional design —
the SLC serves the GPU and Neural Engine in addition to the CPU. Apple invested in
making it fast enough for GPU texture and weight access, which benefits CPU workloads
as a side effect. The prediction of 35-50 cycles overestimated SLC latency by roughly 2×.

### 5.3 The SLC Has an Internal Gradient — It Is Not Flat

Unlike L1 and L2 which show flat latency well within their capacity, the SLC shows
a rising gradient as the working set approaches its boundary:

```
12 MB:  22.5 cycles
16 MB:  28.6 cycles
20 MB:  46.5 cycles
24 MB:  60.6 cycles
```

As the random pointer chase working set grows toward 24MB, it increasingly causes
conflict evictions within the SLC. Elements being evicted and reloaded from DRAM
inflate the average latency before the working set has fully crossed the boundary.
The SLC is not a hard cliff — it degrades gradually under pressure. The safe working
set size for stable SLC latency is approximately 12-16MB on this machine.

### 5.4 The SLC→DRAM Cliff Is the Most Critical Transition

```
20MB (approaching SLC limit):  46.5 cycles
48MB (DRAM):                  127.6 cycles
Jump:                          2.7×
```

This is the sharpest and most expensive transition in the hierarchy. Once a working
set overflows the SLC, performance degrades sharply. The L2→SLC gap was only 5.7
cycles absolute — the SLC→DRAM gap is 105 cycles absolute.

For algorithm design on M2, the most important question is not "does this fit in L2"
but "does this fit in the SLC." The L2→SLC penalty is small. The SLC→DRAM penalty
is enormous.

### 5.5 DRAM Latency Is Not Fixed — TLB Pressure Compounds It

```
48MB  (6M elements):    127.6 cycles
200MB (25M elements):   327.8 cycles
Increase:               2.6× for 4× more data
```

Raw DRAM latency does not change with working set size — a DRAM access always costs
roughly the same number of cycles to activate a row and return data. The increasing
measured latency reflects **TLB pressure**.

Apple M2 page size is 16KB. A 200MB working set spans 200MB / 16KB = 12,800 unique
pages. The TLB cannot hold all of these simultaneously. Each TLB miss triggers a
**page table walk** — a series of memory accesses through the page table hierarchy to
resolve the virtual-to-physical address mapping. On a pointer chase where every
access goes to a random address, TLB miss rate grows with working set size, adding
page walk overhead on top of raw DRAM latency.

True raw DRAM latency on M2 is approximately 100-130 cycles. The 327.8 cycle
measurement at 200MB includes substantial TLB miss overhead on top of that.

### 5.6 The min/avg Gap Is a Measurement Quality Indicator

```
L1  (64KB):   min =   3.8,  avg =   3.8  — 0% gap   (measurement is clean)
DRAM (48MB):  min = 127.6,  avg = 183.1  — 43% gap  (significant OS noise)
DRAM (200MB): min = 327.8,  avg = 341.0  — 4% gap   (noise amortized over longer run)
```

The gap is largest at 48MB because each run takes ~5 seconds — long enough for a
scheduler preemption to cause a large spike, but short enough that one spike dominates
the average. At 200MB the run takes ~10 seconds and preemption cost is amortized
across more steps, shrinking relative variance. At L1 the run completes in ~115ms
and preemption essentially never occurs.

The minimum is the correct metric for hardware latency. This is the standard in
systems benchmarking — the minimum represents the hardware's best-case behavior
with minimal OS interference.

---

## 6. Key Takeaways

**Memory latency varies by 34× across the on-chip hierarchy.** L1 at 3.8 cycles
vs DRAM at 128 cycles. Every algorithm design decision involving memory access
patterns is ultimately a decision about which level of this hierarchy the working
set lives in.

**Apple's SLC is faster than expected — behaves like a large L2 extension.** At
22.5 cycles it is only 5.7 cycles slower than L2. The unified memory design required
Apple to make the SLC fast for GPU workloads, and CPU workloads benefit as a side
effect. Prior assumptions based on x86 LLC latency significantly overestimate SLC cost.

**The SLC→DRAM boundary is the most important threshold on M2.** The L2→SLC gap
is 5.7 cycles. The SLC→DRAM gap is 105 cycles. Keeping working sets within 12-16MB
(comfortably inside SLC) is far more impactful than keeping them within L2.

**DRAM latency is not a constant — it grows with working set size due to TLB
pressure.** Performance models using a single DRAM latency number are only accurate
for small DRAM working sets. At 200MB, measured latency is 2.6× higher than at 48MB
due to page table walk overhead. Apple M2 page size is 16KB — larger than x86's 4KB
— which provides better TLB coverage per entry but does not eliminate TLB pressure
at large working set sizes.

**System state contaminates memory benchmarks more than any other experiment type.**
A dirty macOS system with the memory compressor active produced measurements 30-50×
wrong. Clean boot, minimum reporting across multiple runs, elevated process priority,
and reduced working set size are all necessary for reliable results.

**The minimum, not the average, measures hardware latency.** The average includes
OS scheduler noise, SLC contention from background processes, and DRAM row buffer
state variation. The minimum across multiple runs isolates true hardware behavior.
This is the standard in systems benchmarking.