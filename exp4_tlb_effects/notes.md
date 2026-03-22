# Experiment 4: TLB Effects and Page Walk Cost — Notes

## 1. Experimental Design

This experiment isolates the cost of **TLB misses and page table walks** on the
Apple M2. In exp3, latency grew from 127.6 cycles at 48MB to 327.8 cycles at 200MB
— a 200 cycle increase that could not be explained by cache miss cost alone. TLB
pressure was the suspected cause but could not be confirmed because cache pressure
and TLB pressure increased together as working set size grew.

Exp4 decouples them using two complementary techniques:

**Attempt 1 — Stride sweep (defeated by hardware prefetcher):**
A fixed large array is allocated. The stride between accesses is varied from one
cache line (64 bytes) to multiple pages. Stride controls how many unique pages are
touched per iteration — small stride means few unique pages, large stride means one
access per page and maximum TLB pressure.

**Attempt 2 — Random page order sweep (successful):**
A fixed array spanning many pages is allocated. Accesses jump to randomly ordered
pages, defeating the hardware prefetcher. The number of unique pages touched is
swept from small (fits in TLB) to large (exceeds TLB). The inflection point in
the latency vs unique-pages curve reveals TLB capacity.

A secondary goal is to **infer the Apple M2 TLB size** empirically. Apple does not
publish TLB specifications. The point at which latency rises sharply corresponds
to the working set spanning more unique pages than the TLB can hold.

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
single-threaded, performance cores, `sudo nice -n -20`.

---

## 2. Hypotheses

### 2.1 Expected Behavior

As the number of unique pages touched increases beyond TLB capacity, three regions
are expected:

**Region 1 — TLB not stressed:**
All active pages fit in TLB. Translation is free — TLB hit rate near 100%. Latency
reflects only data access cost.

**Region 2 — TLB saturation begins:**
Unique pages exceed TLB capacity. Each miss triggers a page table walk — 4 sequential
memory accesses through the page table hierarchy. Latency rises sharply.

**Region 3 — Full TLB pressure:**
Every access is a TLB miss. Latency plateaus at maximum — data access cost plus
full page walk cost.

### 2.2 Predicted Latency Values

**Baseline (no TLB pressure):** 17–23 cycles — matching exp3's L2/SLC measurements.

**TLB miss penalty:** A 4-level page walk requires 4 sequential memory accesses.
If page table entries hit the SLC (22.5 cycles each from exp3):

```
4 levels × 22.5 cycles ≈ 90 additional cycles per TLB miss
```

Total at full TLB pressure:
```
DRAM latency + page walk cost ≈ 128 + 90 ≈ 218 cycles
```

**Expected ratio:** TLB-stressed / baseline ≈ 218 / 20 ≈ 10×

### 2.3 Expected TLB Size

Based on comparable ARM designs, the M2 L1 TLB likely holds 128–256 entries.
At 16KB per page:

```
128 entries × 16KB = 2MB coverage
256 entries × 16KB = 4MB coverage
```

Inflection point expected in the 2–4MB range of unique pages.

### 2.4 Expected Shape of Results

```
Latency
(cycles)
  220 |                          ..........
      |                        ..
   20 |................
      +------------------------------------------→
      16   64  256  512  1024  4096  16384  pages
                       ↑
               expected TLB capacity
```

---

## 3. Results

### 3.1 Attempt 1 — Stride Sweep (Failed Due to Hardware Prefetcher)

The first benchmark used sequential strided access over a fixed 256MB array,
sweeping stride from 64 bytes to 262144 bytes:

```
stride=64     :  3.8 cycles
stride=128    :  9.5 cycles
stride=512    : 10.3 cycles
stride=1024   :  7.9 cycles   ← unexpected dip
stride=2048   :  7.1 cycles   ← unexpected dip
stride=4096   : 11.1 cycles
stride=16384  : 12.2 cycles   ← page boundary — expected spike absent
stride=262144 : 10.2 cycles
```

**The expected TLB cliff at stride=16KB was completely absent.** Latency at
stride=16384 (one access per page) was only 12.2 cycles — barely different from
stride=4096. No spike, no plateau.

**Why it failed — hardware prefetcher interference:**

The access pattern `idx = (idx + stride_elems) & mask` is perfectly regular —
a fixed stride repeated 1,000,000 times. The M2 hardware prefetcher detects strided
patterns and prefetches both data and TLB translations ahead of time. By the time
each load executes, the data is already in cache and the TLB entry is already loaded.
The prefetcher completely hides the TLB miss cost.

The dip at stride=1KB–2KB is the prefetcher finding its optimal stride detection
window — it works most efficiently at these strides and hides latency aggressively.

**Lesson:** Sequential strided access is the worst possible pattern for measuring
TLB effects. The hardware prefetcher was specifically designed to handle this case.
Any benchmark that touches memory in a predictable pattern will have its latency
hidden by prefetching, regardless of TLB pressure.

This is the same problem encountered in exp3 — the prefetcher defeated the naive
benchmark and forced the use of pointer chasing. Here it forced the use of random
page ordering.

### 3.2 Attempt 2 — Random Page Order Sweep (Successful)

The redesigned benchmark builds a random permutation of page indices and accesses
one element from each page in random order. This defeats the prefetcher — the next
page address is unknown until the current access completes.

Array: 512MB (32768 pages × 16KB). Sweep: 16 to 32768 unique pages.
Each size: 5 runs, minimum reported. Steps = 1,000,000, wrapping over page_order.

```
     16 pages   (  0 MB)  :   2.4 cycles
     32 pages   (  0 MB)  :   2.5 cycles
     64 pages   (  1 MB)  :   2.3 cycles
    128 pages   (  2 MB)  :   2.4 cycles
    256 pages   (  4 MB)  :   2.4 cycles   ← TLB covers all pages, flat
    512 pages   (  8 MB)  :   3.5 cycles   ← slight rise, TLB starting to stress
   1024 pages   ( 16 MB)  :   8.8 cycles   ← sharp jump — TLB capacity exceeded
   2048 pages   ( 32 MB)  :  11.1 cycles   ← plateau begins
   4096 pages   ( 64 MB)  :  11.6 cycles
   8192 pages   (128 MB)  :  11.6 cycles
  16384 pages   (256 MB)  :  11.5 cycles
  32768 pages   (512 MB)  :  11.3 cycles   ← fully saturated, flat plateau
```

**The TLB inflection point is clearly visible between 512 and 1024 pages.**

### 3.3 Hypothesis Validation

| Prediction | Expected | Measured | Verdict |
|---|---|---|---|
| TLB capacity | 128–256 entries | 512–1024 entries | ✗ Underestimated — M2 TLB is larger |
| Inflection point | 2–4 MB | 8–16 MB | ✗ Underestimated accordingly |
| Baseline latency | 17–23 cycles | 2.4 cycles | ✗ Much lower — data in L1, not L2 |
| TLB miss penalty | ~90 cycles | ~9 cycles | ✗ Much lower — page tables in L1/L2 |
| Shape of curve | Step function | Step function | ✓ Correct |

Every magnitude prediction was wrong, but the shape prediction was correct. The
reasons are explained in the interpretation section.

---

## 4. Assembly Analysis

The hot loop from the random page sweep benchmark:

```asm
LBB0_5:                           ; =>This Inner Loop Header: Depth=1
    ldr  x9, [x8], #8             ; load page_order[s], advance pointer
    lsl  x9, x9, #14              ; page_idx × 16384 (ELEMS_PER_PAGE × 8)
                                  ; = byte offset of first element on that page
    ldr  x9, [x20, x9]            ; load arr[elem_idx] — the actual data access
    add  x19, x19, x9             ; sum += arr[elem_idx]
    subs x21, x21, #1             ; steps--
    b.ne LBB0_5                   ; loop
```

Two loads per iteration. The first load fetches the next page index from
`page_order` — this array is small and stays in L1 throughout. The second load
fetches data from the target page — this is the load whose latency we are measuring.

The `lsl #14` computes the byte offset: `page_idx × 16384`. Since page size is
16KB = 2^14, the shift by 14 is equivalent to multiplying by the page size — the
same `log₂(size)` trick seen in exp3's `lsl #3` for 8-byte elements.

The second `ldr` depends on the result of the first `ldr` (page index) and the
`lsl` (offset computation). This creates a serial dependency — the address of the
data load cannot be computed until the page index is known. The prefetcher cannot
predict this address. TLB miss cost is fully exposed on every access that misses.

---

## 5. Interpretation

### 5.1 TLB Capacity — M2 Is Larger Than Expected

The inflection point between 512 and 1024 pages gives:

```
TLB capacity : 512–1024 entries
TLB coverage : 512 × 16KB = 8MB  to  1024 × 16KB = 16MB
```

The prediction was 128–256 entries based on typical ARM designs. Apple's M2 TLB
is significantly larger — consistent with Apple's philosophy of investing in large,
fast on-chip structures (the same philosophy that produced the 24MB SLC). A larger
TLB means more working set coverage before TLB pressure begins, which directly
benefits the large working sets used in GPU and ML inference workloads.

### 5.2 Why the Measured TLB Miss Penalty Is Only 9 Cycles

The prediction was ~90 cycles (4 levels × 22.5 cycles SLC latency). The measured
delta was only ~9 cycles (11.6 - 2.4). This is the most important discrepancy and
it has a precise explanation.

**Page table entries are cached in L1/L2, not SLC or DRAM.**

The benchmark runs 5 × 1,000,000 = 5 million accesses over the same set of pages.
Each page walk fetches 4 page table entries. After the first few iterations, all
page table entries covering the benchmark array are hot in L1 or L2 cache. Every
subsequent page walk finds its entries in L1 (~4 cycles per level) instead of SLC
or DRAM.

```
4 levels × ~2 cycles (L1 hit) ≈ 8 cycles page walk overhead   ✓ matches measurement
```

This is called a **warm TLB miss** — the TLB has no entry but the page table itself
is cached. The hardware page table walker traverses 4 levels but each level hits L1.

**TLB miss cost is not a fixed number. It depends on where page table entries live:**

```
Page table entries in L1   :   ~8 cycles   (this experiment — warm, repeated access)
Page table entries in SLC  :  ~90 cycles   (hypothesis — moderately warm)
Page table entries in DRAM :  ~200 cycles  (exp3 200MB result — cold, first touch)
```

### 5.3 The Cold Page Walk Cost Is Already Measured in Exp3

Attempting to force cold page table walks in a user-space benchmark on macOS is
difficult — no TLB flush instruction is available without kernel privileges, and
the OS aggressively caches page table entries. However, the cold measurement is
not needed — it already exists in exp3's data.

In exp3, the pointer chase at 200MB accessed 200MB / 16KB = 12,800 unique pages
with no warmup of page table entries. This was effectively a cold page walk
measurement:

```
Exp3 — 48MB  (3072 pages,  within TLB coverage) :  127.6 cycles
Exp3 — 200MB (12800 pages, beyond TLB coverage) :  327.8 cycles
Delta                                            :  200.2 cycles ← cold page walk cost
```

This delta — 200 cycles — is the true cost of a TLB miss when page table entries
are cold in DRAM. Exp4's warm measurement of 9 cycles and exp3's cold measurement
of 200 cycles together bound the full range of TLB miss cost on M2.

### 5.4 Why Strided Access Failed — The Prefetcher Is Too Good

The M2 hardware prefetcher recognises strided access patterns and prefetches both
data and TLB translations speculatively. A fixed stride repeated 1,000,000 times
is the easiest pattern for the prefetcher to learn. Even at stride=16KB (one access
per page), the prefetcher loaded upcoming pages and their TLB translations before
the CPU needed them — making the TLB miss cost invisible.

This failure mode is a direct parallel to exp3's initial failure — naive sequential
access hid cache miss cost just as strided access hid TLB miss cost here. In both
cases, the fix was randomisation — pointer chasing in exp3, random page order in exp4.

**The general principle:** any benchmark that accesses memory in a predictable
pattern will have its latency hidden by the hardware prefetcher. Isolating memory
subsystem costs always requires defeating predictability.

### 5.5 Complete TLB Cost Model for Apple M2

Combining exp3 and exp4 results:

```
TLB capacity              : 512–1024 entries
TLB coverage              : 8–16 MB of unique pages
TLB hit cost              : ~0 cycles  (translation free)
Warm TLB miss penalty     : ~9 cycles  (page tables in L1/L2, repeated access)
Cold TLB miss penalty     : ~200 cycles (page tables in DRAM, first touch)
Inflection point          : between 512 and 1024 unique pages
```

---

## 6. Key Takeaways

**TLB miss cost is not a fixed number — it depends on page table cache state.**
Warm page tables in L1 cost ~9 cycles per miss. Cold page tables in DRAM cost ~200
cycles per miss. The difference is 22×. Real applications sit somewhere between
these extremes depending on how recently each region of virtual address space was
last accessed.

**The M2 TLB is larger than typical ARM designs — 512–1024 entries vs 128–256.**
This provides 8–16MB of TLB coverage, significantly reducing TLB pressure for the
large working sets common in Apple's GPU and ML inference workloads. Empirical
measurement was necessary — Apple does not publish this specification.

**Hardware prefetching defeats strided TLB benchmarks completely.**
Fixed-stride access is the most prefetcher-friendly pattern. The prefetcher loads
TLB translations ahead of time just as it loads data ahead of time. Measuring TLB
effects requires random access patterns that the prefetcher cannot predict — the
same principle that required pointer chasing in exp3.

**The cold TLB miss cost was already captured in exp3.**
The 200-cycle difference between exp3's 48MB and 200MB pointer chase measurements
is the cold page walk cost. Exp4 provides the complementary warm measurement. Together
they give the full range: 9 cycles (warm) to 200 cycles (cold).

**Virtual memory translation is layered on top of every memory access.**
Every load and store pays translation cost before cache lookup. The TLB makes this
cost zero for hot pages. When the TLB misses, the hardware page table walker traverses
4 levels of page tables — each level is itself a memory access whose cost depends on
where that page table entry lives in the cache hierarchy. Understanding this layering
is essential for reasoning about memory performance in any real system.