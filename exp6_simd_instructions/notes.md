# Experiment 6: SIMD Instructions and Auto-Vectorization -- Notes

## 1. Experimental Design

### 1.1 Cases 1–3: Plain Float32 Reduction Sum

This experiment measures the performance impact of SIMD vectorization on a floating-point
reduction sum over a large array. Three configurations are profiled in sequence, changing
exactly one variable between adjacent cases.

**Workload:** Sum of 1M `float32` values (4 MB array). The array is filled at runtime
with values cycling through `1.000 ... 1.007` (pattern `1.0f + (i & 7) * 0.001f`).
Runtime initialization prevents the compiler from computing the sum as a compile-time
constant. The `escape()` inline asm barrier on the result prevents dead-code elimination.

**Why this kernel:**
The reduction sum `sum += arr[i]` has a loop-carried dependency through the accumulator.
In scalar code this forces the CPU to serialize every add. In vectorized code the compiler
can split the work across independent accumulator vectors, breaking the serial chain and
exposing both SIMD width and instruction-level parallelism simultaneously. The kernel is
compute-simple (just adds) so any gap between scalar and vector is purely a hardware
parallelism story, not an algorithmic one.

**Why `-ffast-math` is required for Cases 2 and 3:**
IEEE 754 mandates that floating-point addition be treated as non-associative: the compiler
cannot change the order of additions without changing the result. Vectorizing a reduction
requires splitting one serial sum into N independent partial sums -- that reordering is
only legal with `-ffast-math`. Without it, the compiler refuses to vectorize. All three
cases use `-ffast-math` for a fair comparison; Case 1 adds `-fno-vectorize` to disable
the vectorizer specifically.

**Case 1 -- Scalar baseline (`-O2 -fno-vectorize -ffast-math`):**
Compiler emits one scalar `fadd` per element. The loop-carried RAW dependency on the
accumulator creates a strict serial dependency chain. Expected bottleneck: `fadd` latency.

**Case 2 -- Auto-vectorized (`-O2 -ffast-math`):**
Compiler is free to emit NEON vector instructions. With `-ffast-math` granted, it splits
the sum into multiple independent accumulator vectors. Key question: how many accumulators
does it choose, and does that saturate the M2's dual NEON pipelines?

**Case 3 -- Hand-written ARM NEON (`-O2 -ffast-math` + `<arm_neon.h>` intrinsics):**
Eight independent `float32x4_t` accumulator vectors are written explicitly. This processes
32 floats per loop iteration -- exactly one full 128-byte M2 cache line. If Case 3 matches
Case 2, the compiler was already at the hardware ceiling. If Case 3 is faster, we identify
what the compiler missed.

### 1.2 Case 4: Conditional Sum (Branch-Mispredict vs NEON Branch-Free)

A second sub-experiment isolates one additional effect: **branch misprediction**, and
demonstrates the specific scenario where hand-written NEON intrinsics outperform the
compiler even at the same `-O2` optimization level.

**Workload:** `if (arr[i] > 0.0f) sum += arr[i]` over 1M elements. Array is filled with
a Xorshift32 PRNG (seed `0xDEADBEEF`) producing ~50% `+1.5f` and ~50% `-0.7f` values in
a pattern that is genuinely unpredictable to hardware branch predictors.

**Why the compiler cannot auto-vectorize this:**
Without `-ffast-math` the compiler must preserve exact FP accumulation order (IEEE 754
non-associativity). The conditional branch also makes it impossible to prove that
`if (x>0) s+=x` is equivalent to `s += max(x, 0)` -- `max(-0.0f, 0.0f)` and NaN behavior
differ between the two forms. The compiler refuses to make that substitution. Confirmed
by assembly: both `cond_sum_scalar.s` (forced scalar) and `cond_sum_compiler.s` (compiler
free to vectorize) produce byte-for-byte identical timed loops.

**Case 4a -- Scalar branchy (`-O2 -fno-vectorize -std=c++17`):**
`ldr / fcmp / b.le / fadd` per element. With ~50% mispredict rate, every other branch
triggers a ~12–15 cycle pipeline flush. This is the honest worst case for a branchy loop.

**Case 4b -- Hand-written NEON (`-O2 -std=c++17` + intrinsics):**
Uses `vmaxq_f32(v, zero)` to clamp negative lanes to 0.0f before accumulation -- no
branch instruction. Eight independent `float32x4_t` accumulators, 32 floats per iteration,
128 bytes per loop = same structure as Case 3. The programmer explicitly asserts that the
data is free of NaN and negative-zero edge cases, making the `max` substitution safe where
the compiler could not assume it.

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, single P-core.

---

## 2. Hypotheses

### 2.1 M2 NEON Pipeline Parameters

```
fadd.4s latency            : ~3 cycles
fadd.4s throughput         : 2 per cycle  (dual NEON execution units)
fmax.4s latency            : ~2 cycles
fmax.4s throughput         : 2 per cycle
Minimum accumulators to
  saturate both units      : latency x throughput = 3 x 2 = 6 independent vectors
Scalar fadd latency        : ~3 cycles (same as vector, just 1 float instead of 4)
Branch mispredict penalty  : ~12-15 cycles on M2
```

### 2.2 Case 1 -- Scalar Baseline

One `ldr s, [ptr]` + one `fadd s, s, s` per element. The `fadd` must read the result
of the previous `fadd` (loop-carried RAW dependency). Throughput = 1 fadd per latency
period. However the array (4 MB) does not fit in L1d (64 KB), so loads come from L2.
With the hardware prefetcher handling stride-1 access, effective load latency is ~4 cycles.

The `fadd` cannot start until the load result is ready -- so the bottleneck is:

```
max(fadd_latency=3, effective_ldr_latency~4) = 4 cycles/element
```

**Predicted: ~3-4 cycles/element (~0.9-1.2 ns/element).**

### 2.3 Case 2 -- Auto-Vectorized

With 4 independent accumulator vectors the compiler processes 16 floats per loop
iteration. The compute ceiling with 4 accumulators and 3-cycle latency chains:

```
4 accumulators, issue 2/cycle:
  cycles 1-2: issue acc0, acc1, acc2, acc3
  wait 1 cycle (acc0 not ready until cycle 4)
  cycles 4-5: next round
  -> one group of 16 floats every 3 cycles = 5.3 floats/cycle = 0.19 cycles/elem
```

The array is 4 MB, L2 is 4 MB -- the array barely fits. The hardware prefetcher keeps
data flowing but L2 bandwidth becomes the ceiling:

```
64 bytes per iteration / hardware load bandwidth -> may be L2-bound
```

**Predicted: 4-10x faster than scalar, likely L2 bandwidth limited.**

### 2.4 Case 3 -- Hand-Written NEON (8 Accumulators)

Eight independent accumulator vectors process 32 floats = 128 bytes per iteration.
Since 128 bytes = one M2 cache line, this is the most natural alignment with the memory
hierarchy. With 8 independent chains the compute units are saturated well beyond the
minimum 6. If L2 bandwidth is the true bottleneck, Case 3 should match Case 2 exactly
-- doubling accumulators doubles floats per iteration but also doubles data per iteration,
leaving cycles-per-element unchanged.

**Predicted: matches Case 2 (~0.43 cycles/elem) -- L2 bandwidth bound, not compute bound.**

### 2.5 Case 4 -- Conditional Sum

**Case 4a (scalar branchy):**
Each element requires a branch. With ~50% mispredict rate (Xorshift32 pattern is
cryptographically random to hardware predictors):

```
base cost (ldr + fcmp + branch + fadd) : ~4-5 cycles/elem (same as Case 1 scalar)
mispredict penalty per element         : 50% x 12-15 cycles = 6-7.5 extra cycles
extra store to stack (str s0 per hit)  : ~0.5 cycles/elem on positive elements
Total predicted                        : ~10-13 cycles/elem minimum
```

**Predicted: ~10-20 cycles/elem for Case 4a (mispredict-dominated).**

**Case 4b (NEON branch-free):**
`vmaxq_f32` eliminates the branch entirely. The resulting loop (load + fmax + fadd,
8 independent chains, 128 bytes/iter) is structurally identical to Case 3. Expected
to hit the same L2 bandwidth ceiling.

**Predicted: ~0.43 cycles/elem for Case 4b (L2 bandwidth bound, same as Cases 2/3).**

### 2.6 Predicted Summary

| Case | Configuration | Predicted cycles/elem |
|---|---|---|
| 1 | Scalar, 1 accumulator, no branch | ~3-4 |
| 2 | Auto-vec, 4 accumulators | ~0.2-0.5 (likely L2 bound) |
| 3 | Manual NEON, 8 accumulators | matches Case 2 |
| 4a | Scalar, conditional branch, ~50% mispredict | ~10-20 |
| 4b | Manual NEON, vmaxq_f32, no branch | ~0.43 (L2 bound) |

---

## 3. Results

### 3.1 Case 1 -- Scalar Baseline

```
Result (anti-elim check): 1.04908e+06
N = 1048576  RUNS = 5
min = 1.23862 ns/elem  =  4.12462 cycles/elem
avg = 1.99896 ns/elem  =  6.65653 cycles/elem
```

The result is non-trivial (not a compile-time constant), confirming the loop executed.
The min (4.12 cycles) measures the steady-state cost; the avg (6.66 cycles) is inflated
by the first run where some data is not yet warm in L2. The scalar loop exposes this cold-start penalty fully because each load is on the critical path.

### 3.2 Case 2 -- Auto-Vectorized

```
Result (anti-elim check): 1.05255e+06
N = 1048576  RUNS = 5
min = 0.129819 ns/elem  =  0.432297 cycles/elem
avg = 0.129882 ns/elem  =  0.432508 cycles/elem
```

Note the result differs slightly from Case 1 (1.05255e6 vs 1.04908e6). This is expected:
`-ffast-math` allows FP addition reordering, which changes accumulation order and
introduces different rounding errors. Both results are numerically correct sums; the
difference is in the last few ULPs. The vectorised loops with 4-8 independent accumulators absorb the cold-start latency in the out-of-order window — the penalty is hidden behind the independent accumulator chains

### 3.3 Case 3 -- Hand-Written NEON (8 Accumulators)

```
Result (anti-elim check): 1.05243e+06
N = 1048576  RUNS = 5
min = 0.129899 ns/elem  =  0.432564 cycles/elem
avg = 0.130224 ns/elem  =  0.433647 cycles/elem
```

### 3.4 Case 4a -- Scalar Conditional Sum (Branchy)

```
Result: 786898
N = 1048576  RUNS = 5
min = 5.91664 ns/elem  =  19.7024 cycles/elem
avg = 6.94731 ns/elem  =  23.1345 cycles/elem
```

Result is ~786898, close to the expected N/2 × 1.5 = 786432 (small deviation from
Xorshift32 not being exactly 50/50 on this seed/length). The 19.7 cycles/elem minimum
is far above the 4.12 cycles/elem of Case 1 (branchless scalar). The extra ~15.6 cycles
is almost entirely branch misprediction overhead.

Note: `cond_sum_compiler` (same source, without `-fno-vectorize`) measured identically
at 19.4 cycles/elem, and its assembly was byte-for-byte the same -- confirming the
compiler refused to vectorize in both variants for the reasons described in Section 1.2.

### 3.5 Case 4b -- NEON Conditional Sum (Branch-Free via `vmaxq_f32`)

```
Result: 786898
N = 1048576  RUNS = 5
min = 0.125806 ns/elem  =  0.418933 cycles/elem
avg = 0.134412 ns/elem  =  0.447593 cycles/elem
```

Result matches Case 4a exactly (786898) -- correct, since both process the same data
(same Xorshift32 seed) and the `vmaxq_f32` substitution is numerically identical for
finite non-NaN non-negative-zero values. The difference from the theoretical 786432 is
from the PRNG not being exactly 50/50.

### 3.6 Summary Table

| Case | min ns/elem | min cycles/elem | vs Case 1 | vs Case 4a |
|---|---|---|---|---|
| 1 -- Scalar (branchless) | 1.239 | 4.124 | 1x (baseline) | -- |
| 2 -- Auto-vec | 0.1298 | 0.4323 | **9.54x** | -- |
| 3 -- NEON manual | 0.1299 | 0.4326 | **9.53x** | -- |
| 4a -- Scalar branchy | 5.917 | 19.70 | **0.21x (4.8x slower than Case 1)** | 1x (baseline) |
| 4b -- NEON branch-free | 0.1258 | 0.4189 | **9.84x** | **47x faster than 4a** |

**The 47x speedup of Case 4b over 4a is the compound of two independent effects:**
vectorization (~10x) and branch mispredict elimination (~5x).

---

## 4. Assembly Analysis

### 4.1 Case 1 -- Scalar Inner Loop

```asm
LBB0_5:
    ldr  s0, [x19, x8]        ; load arr[i] into scalar float register s0
    fadd s8, s8, s0            ; s8 += s0  -- 1 float, loop-carried dep on s8
    add  x8, x8, #4            ; advance byte pointer by 4 (one float)
    cmp  x8, #1024, lsl #12    ; compare against 4194304 (N * 4 bytes)
    b.ne LBB0_5
```

**One `ldr` + one `fadd` per element. No unrolling. One accumulator.**

The dependency graph per iteration:
```
ldr s0               -> produces s0 (new data, ~4 cycle load latency from L2)
fadd s8, s8, s0      -> reads s8 (from prev iter, 3 cycle latency) AND s0 (from ldr)
                     -> fadd cannot issue until BOTH inputs are ready
                     -> bottleneck = max(3, ~4) = ~4 cycles/iter
```

### 4.2 Case 2 -- Auto-Vectorized Inner Loop

```asm
; Accumulators zeroed before the loop:
movi.2d  v7, #0000000000000000   ; acc0 = [0, 0, 0, 0]
movi.2d  v0, #0000000000000000   ; acc1 = [0, 0, 0, 0]
movi.2d  v1, #0000000000000000   ; acc2 = [0, 0, 0, 0]
movi.2d  v2, #0000000000000000   ; acc3 = [0, 0, 0, 0]

LBB0_5:
    add     x9, x19, x8
    ldp     q3, q4, [x9]         ; load 32 bytes: q3=[arr[i..i+3]], q4=[arr[i+4..i+7]]
    ldp     q5, q6, [x9, #32]    ; load 32 bytes: q5=[arr[i+8..i+11]], q6=[arr[i+12..i+15]]

    fadd.4s  v7, v7, v3          ; acc0 += chunk0  (4 floats simultaneously)
    fadd.4s  v0, v0, v4          ; acc1 += chunk1  (independent of acc0)
    fadd.4s  v1, v1, v5          ; acc2 += chunk2  (independent of acc0, acc1)
    fadd.4s  v2, v2, v6          ; acc3 += chunk3  (independent of all above)

    add     x8, x8, #64          ; advance 64 bytes = 16 floats x 4 bytes
    cmp     x8, #1024, lsl #12
    b.ne    LBB0_5

; Post-loop reduction: merge 4 vectors into 1 scalar
    fadd.4s  v0, v0, v7          ; [acc1+acc0] per lane
    fadd.4s  v0, v1, v0          ; [acc2+acc1+acc0] per lane
    fadd.4s  v0, v2, v0          ; [acc3+acc2+acc1+acc0] per lane
    faddp.4s v0, v0, v0          ; pairwise: [lane0+lane1, lane2+lane3, ...]
    faddp.2s s0, v0              ; final scalar: (lane0+lane1) + (lane2+lane3)
```

**Key facts:**
- Accumulator count: **4 vectors** (v7, v0, v1, v2)
- Floats per iteration: **16** (2 x ldp loading 8 floats each)
- Loop iterations: N/16 = **65,536**
- All 4 `fadd.4s` are mutually independent -- no cross-accumulator RAW dependencies

### 4.3 Case 3 -- Hand-Written NEON Inner Loop

```asm
; Accumulators zeroed: v2, v3, v4, v5, v6, v7, v16, v17

LBB0_6:
    ldp  q0, q1, [x8, #-64]     ; load 32 bytes (8 floats)
    fadd.4s  v2, v0, v2          ; acc0 += chunk0
    fadd.4s  v3, v1, v3          ; acc1 += chunk1

    ldp  q0, q1, [x8, #-32]     ; load next 32 bytes
    fadd.4s  v4, v0, v4          ; acc2
    fadd.4s  v5, v1, v5          ; acc3

    ldp  q0, q1, [x8]            ; load next 32 bytes
    fadd.4s  v6, v0, v6          ; acc4
    fadd.4s  v7, v1, v7          ; acc5

    ldp  q0, q1, [x8, #32]      ; load last 32 bytes
    fadd.4s  v16, v0, v16        ; acc6
    fadd.4s  v17, v1, v17        ; acc7

    add  x8, x8, #128            ; advance 128 bytes = 1 full M2 cache line
    b.lo LBB0_6
```

**Key facts:**
- Accumulator count: **8 vectors** (v2-v7, v16-v17)
- Floats per iteration: **32** (4 x ldp loading 8 floats each)
- Loop iterations: N/32 = **32,768**
- Advances by 128 bytes per iteration = exactly one M2 cache line

**The register spill -- a compiler artefact:**

The 8 accumulators (v2-v7, v16-v17) are all **caller-saved** NEON registers. The
`steady_clock::now()` call after the loop is a black-box function that clobbers
caller-saved registers. The compiler therefore spills all 8 accumulators to the
stack before the clock call and reloads them after:

```asm
stp  q3,  q2,  [sp, #96]    ; spill to stack (4 x stp = 128 bytes written)
stp  q5,  q4,  [sp, #64]
stp  q7,  q6,  [sp, #32]
stp  q17, q16, [sp]
bl   steady_clock::now()     ; t1 captured here
ldp  q1,  q0,  [sp]         ; reload (4 x ldp = 128 bytes read)
...reduction...
```

This overhead runs **once per timed run**, adding ~8-16 cycles across 1M elements =
0.000008-0.000016 cycles/elem. Completely invisible in the measurement.

### 4.4 Case 4a -- Scalar Conditional Sum (Timed Loop)

```asm
; 5 identical copies of this loop (RUNS=5 outer loop unrolled by compiler)
LBB0_6:                       ; inner loop header
    ldr  s1, [x19, x8]        ; load arr[i]
    fcmp s1, #0.0              ; compare with zero
    b.le LBB0_5                ; branch: SKIP if arr[i] <= 0  <- ~50% mispredict rate

; positive path (taken ~half the time):
    fadd s0, s1, s0            ; s += arr[i]
    str  s0, [sp, #60]         ; store s back to stack (every positive element!)
    b    LBB0_5

LBB0_5:
    add  x8, x8, #4
    cmp  x8, #4194304
    b.eq LBB0_8               ; exit loop
```

**Critical observations:**

1. **Branch is present and genuinely unpredictable.** `fcmp + b.le` is in every iteration.
   The Xorshift32 pattern looks random to the branch predictor → ~50% mispredict rate.

2. **`str s0, [sp, #60]` is inside the hot path.** Every positive element triggers a
   store to the stack slot. This happens because `escape(&s)` from a previous RUNS
   iteration caused the compiler to treat `s`'s stack slot as potentially aliased. It
   keeps s in sync between register and memory on every update. This adds an extra
   memory write on ~50% of all iterations, on top of the mispredict penalty.

3. **RUNS=5 outer loop is unrolled into 5 separate copies** (LBB0_6, LBB0_10, LBB0_14,
   LBB0_18, LBB0_22). Each copy is identical. This is the compiler avoiding branch
   overhead on the outer loop at the cost of code size.

4. **`cond_sum_compiler.s` is byte-for-byte identical.** Removing `-fno-vectorize` did
   nothing. Both files have the same `ldr/fcmp/b.le/fadd/str` timed loop. The compiler
   was blocked by IEEE 754, not by the flag.

### 4.5 Case 4b -- NEON Conditional Sum (Timed Loop)

```asm
; v16 = {0.0f, 0.0f, 0.0f, 0.0f}  (zero vector, loop-invariant, hoisted out)
; v17-v24 = 8 accumulators, zeroed at top of each RUNS iteration

LBB0_6:                            ; inner loop header (RUNS loop is NOT unrolled)
    ldp  q0, q1, [x8, #-64]        ; load 32 bytes (floats 0-7)
    fmax.4s  v0, v0, v16            ; max(lane, 0) -- negative lanes become 0.0f
    fmax.4s  v1, v1, v16            ; NO BRANCH: fmax is a pure compute instruction
    ldp  q2, q3, [x8, #-32]        ; load floats 8-15
    fmax.4s  v2, v2, v16
    fmax.4s  v3, v3, v16
    ldp  q4, q5, [x8]              ; load floats 16-23
    fmax.4s  v4, v4, v16
    fmax.4s  v5, v5, v16
    ldp  q6, q7, [x8, #32]         ; load floats 24-31
    fmax.4s  v6, v6, v16
    fmax.4s  v7, v7, v16
    fadd.4s  v17, v17, v0           ; accumulate 8 independent chains
    fadd.4s  v18, v18, v1
    fadd.4s  v19, v19, v2
    fadd.4s  v20, v20, v3
    fadd.4s  v21, v21, v4
    fadd.4s  v22, v22, v5
    fadd.4s  v23, v23, v6
    fadd.4s  v24, v24, v7
    add  x27, x27, #32
    add  x8, x8, #128              ; advance exactly one 128-byte M2 cache line
    cmp  x27, x24
    b.lo LBB0_6
```

**Key observations:**

1. **Zero branch instructions in the timed loop body.** No `fcmp`, no `b.le`, no
   conditional of any kind. The only branch is `b.lo LBB0_6` -- the loop-back itself,
   which is always-taken until the last iteration and predicted perfectly.

2. **`fmax.4s v0, v0, v16` is the entire conditional.** This single instruction
   processes 4 floats simultaneously. Where a lane was negative, it becomes 0.0f.
   Where it was positive, it passes through unchanged. Adding 0.0f to the accumulator
   is a no-op numerically -- this is exactly equivalent to `if (x>0) s+=x` for our data.

3. **`fmax.4s` has 2-cycle latency, 2 per cycle throughput.** It is cheaper than
   `fadd.4s` (3-cycle latency). The compute budget per 32 floats is dominated by
   `fadd.4s`, not `fmax.4s`.

4. **No `str` inside the loop.** Unlike Case 4a, there is no per-element stack writeback.
   The accumulators live in registers throughout the entire timed loop.

5. **RUNS outer loop is NOT unrolled** (compare to Case 4a which unrolled to 5 copies).
   The NEON version keeps 9 live NEON registers (v16-v24), so the compiler decided
   against unrolling to avoid even higher register pressure.

6. **Register spill before `steady_clock::now()`** (same as Case 3): v17-v24 are
   caller-saved, so the compiler spills them to the stack before calling `now()` and
   reloads them for the reduction. This spill is outside the timed window relative to
   t0, but inside relative to t1. Since it's 8 vector stores (128 bytes) amortized over
   1M elements, it contributes ~0.0001 cycles/elem -- invisible.

### 4.6 Instruction Vocabulary Reference

| Intrinsic | Assembly | Meaning |
|---|---|---|
| `float32x4_t` | `v0..v31` | 128-bit NEON register, 4 x float32 |
| `vdupq_n_f32(0)` | `movi.2d v0, #0` | zero all 4 lanes |
| `vld1q_f32(ptr)` | `ldr q0, [x0]` | load 4 floats (16 bytes) from pointer |
| `vaddq_f32(a, b)` | `fadd.4s v0, v0, v1` | add 4 float pairs simultaneously |
| `vmaxq_f32(a, b)` | `fmax.4s v0, v0, v1` | lane-wise max(a, b) -- no branch |
| `vaddvq_f32(v)` | `faddp.4s + faddp.2s` | sum all 4 lanes to one scalar |
| -- | `ldp q0, q1, [x]` | load PAIR of 128-bit registers (32 bytes at once) |

---

## 5. Interpretation

### 5.1 Decomposing the Case 1 Bottleneck

The scalar loop has two inputs to every `fadd`: the accumulator (loop-carried, 3-cycle
dependency) and the loaded value (from L2, ~4 cycles with prefetching). The `fadd`
cannot issue until BOTH are ready. The critical path is therefore:

```
scalar bottleneck = max(fadd_latency, effective_ldr_latency)
                  = max(3,  ~4)
                  = 4 cycles/element   (matches measured 4.12 cycles)
```

Note: the hardware prefetcher handles stride-1 access well, so effective load latency
from L2 is not the full 12-cycle L2 latency -- but it is still ~4 cycles to get the
value into the NEON pipeline, which just barely exceeds `fadd` latency and shifts
the bottleneck from compute to load.

### 5.2 Speedup Decomposition: Scalar to Auto-Vectorized (Cases 1→2)

```
Observed speedup: 4.124 / 0.4323 = 9.54x
```

The speedup comes from two orthogonal sources:

**Source 1 -- SIMD width (4x):**
`fadd.4s` processes 4 floats in the same latency as scalar `fadd` processes 1.

**Source 2 -- Breaking the serial dependency chain:**
The scalar accumulator is one register that every iteration reads and writes. The 4
vector accumulators are independent -- no iteration of acc0 depends on acc1. This
eliminates the bottleneck interaction between load latency and accumulator latency.
In vector mode, the four load streams and four fadd chains run in parallel; the load
latency is absorbed into the pipeline rather than sitting on the critical path.

```
If purely SIMD-width limited (one vector accumulator):
  cycles/elem = fadd.4s_latency / 4 floats = 3/4 = 0.75 cycles
  speedup over scalar = 4.12 / 0.75 = ~5.5x

Observed: 9.54x  (more than 5.5x)
Extra speedup comes from multiple independent accumulators eliminating the
serial bottleneck that load latency imposed on the scalar case.
```

### 5.3 The L2 Bandwidth Ceiling

The Case 2 loop loads 64 bytes per iteration. At N/16 = 65,536 iterations:

```
Total data = 65,536 x 64 bytes = 4,194,304 bytes = 4 MB (the full array)
Measured time = 0.1298 ns/elem x 1,048,576 = 136,100 ns

L2 read bandwidth = 4 MB / 136,100 ns = 30.8 GB/s
```

**~31 GB/s is the single-core L2 streaming read bandwidth on M2 for this kernel.**

The Case 3 loop loads 128 bytes per iteration. At N/32 = 32,768 iterations, it also
reads 4 MB total -- in the same time, achieving the same ~31 GB/s. Doubling the
accumulators doubled the floats per iteration AND the data per iteration. The ratio
(cycles per element) is unchanged because both numerator and denominator scaled equally.

Case 4b also lands at the same ceiling:

```
Case 4b bandwidth = 4 MB / (0.1258 ns/elem x 1,048,576) = 4 MB / 131,920 ns = 30.3 GB/s
```

All three vectorized cases (2, 3, 4b) measure ~30–31 GB/s, confirming they all hit
the same L2 bandwidth wall. The conditional sum (4b) is no slower than the plain sum
(2, 3) -- `fmax.4s` is cheap enough that it does not add to the critical path.

### 5.4 Why the Compiler's 4 Accumulators Were Already the Right Choice

The minimum accumulators to saturate M2's dual NEON units:

```
2 fadd.4s units x 3-cycle latency = 6 independent operations in flight at minimum
-> 6 / 4 floats per op = 6 accumulator vectors to hit throughput ceiling
```

The compiler chose 4, which is below the compute saturation threshold. We used 8,
which is above it. Both are L2-bandwidth-bound. This proves the bottleneck is NOT
the number of accumulators. The compiler made a pragmatic choice: 4 accumulators
fit in callee-saved registers (no stack spill), generate a tighter loop body, and
reach the same hardware ceiling as 8.

### 5.5 Decomposing the Case 4b Speedup (47x)

```
Scalar branchy (4a):      19.70 cycles/elem
Scalar branchless (Case 1): 4.12 cycles/elem  <- hypothetical "what if no mispredicts"
NEON branch-free (4b):     0.42 cycles/elem
```

The 47x total speedup has two independent multiplicative components:

**Component 1 -- Branch mispredict elimination (scalar 4a → scalar branchless Case 1):**
```
19.70 / 4.12 = 4.78x
```
This is the cost of the branch alone. ~50% mispredict rate × ~15 cycle penalty ≈ 7.5
extra cycles per element. The measured gap (19.70 - 4.12 = 15.58 extra cycles) aligns
with a ~13-cycle average penalty at ~50% rate, within the expected 12-15 cycle range.

**Component 2 -- Vectorization + ILP (scalar branchless → NEON branch-free):**
```
4.12 / 0.42 = 9.8x
```
This is the same speedup seen in Cases 1→2: SIMD width (4x) plus multiple independent
accumulators (eliminating the serial dependency bottleneck).

**Combined: 4.78 × 9.8 ≈ 46.8x** (matches measured 19.70 / 0.42 = 46.9x exactly).

The two effects are fully independent and multiply cleanly. This is the key insight:
**NEON does not just make things faster -- it eliminates a different class of bottleneck
entirely (branch mispredicts) while simultaneously delivering the vectorization gains.**

### 5.6 Why `-fno-vectorize` Was Redundant for Case 4

Removing `-fno-vectorize` from `cond_sum_scalar.cpp` produced byte-for-byte identical
assembly. The compiler was blocked by two separate constraints, either of which alone
would have been sufficient:

1. **FP non-associativity (IEEE 754):** Without `-ffast-math`, the accumulation order
   `s += arr[i]` must be preserved left-to-right. Vectorization would split into parallel
   partial sums, which reorders additions. Not permitted without explicit relaxation.

2. **Semantic non-equivalence of `if (x>0) s+=x` and `s += max(x,0)`:**
   For NaN: `NaN > 0.0f` is false → the conditional skips NaN, leaving s unchanged.
   But `s += max(NaN, 0.0f)` would add NaN, corrupting s. Same issue for `-0.0f` as a
   threshold. Without programmer assertion that data is safe (which intrinsics provide
   implicitly), the compiler cannot make this substitution.

Both constraints must be resolved for vectorization to occur. The programmer resolves
both simultaneously by writing `vmaxq_f32` directly: the intrinsic encodes the semantic
intent, and the compiler is not involved in the decision.

### 5.7 The `-ffast-math` Flag and Numerical Accuracy

Without `-ffast-math`:
- The compiler treats FP addition as non-associative (per IEEE 754)
- The reduction must accumulate strictly left-to-right
- No vectorization is possible -- the compiler refuses to split into partial sums
- The scalar and vector results would be bit-identical

With `-ffast-math`:
- The compiler may reorder FP operations freely
- Vectorization splits the sum into independent partial sums accumulated in different order
- The final result differs in the last few ULPs:

```
Case 1 result: 1.04908e+06
Case 2 result: 1.05255e+06
Case 3 result: 1.05243e+06
```

All three are correct sums of the array; none is "wrong". The differences arise from
different floating-point rounding paths (adding 1M numbers in 1 chain vs 4 chains vs
8 chains). For most engineering workloads these differences are acceptable. For financial
or high-precision scientific code, `-ffast-math` should be used with care.

The Case 4 kernel does not use `-ffast-math`. The NEON version (Case 4b) achieves
vectorization purely through intrinsics -- the programmer provides the semantic guarantee
directly, no flag needed.

### 5.8 Connection to Previous Experiments

Exp6 completes the CPU execution lab series and connects directly to earlier findings:

**Exp1 (ILP):** The scalar accumulator creates a loop-carried dependency — the same
bottleneck studied in exp1. Vectorisation with multiple accumulators eliminates this
dependency, applying exp1's lesson at the SIMD level.

**Exp2 (branch prediction):** Case 4a measured 19.7 cycles/elem under ~50% mispredict
rate — consistent with exp2's finding of 8.4x slowdown from mispredictions. The
vmaxq_f32 fix is the SIMD equivalent of exp2's sorted-array fix: eliminate the branch
entirely rather than trying to predict it.

**Exp3 (memory latency):** The L2 bandwidth ceiling of 31 GB/s directly uses exp3's
measured L2 latency of 16.8 cycles. The vectorised kernels are memory-bound — a
regime exp3's pointer chase characterised precisely.

**The -fno-vectorize flag** appeared in every experiment from exp1 through exp5,
deliberately hiding SIMD effects. Exp6 finally removes it and measures what was
being left on the table: a 9.5x speedup for compute-simple kernels.

---

## 6. Key Takeaways

**SIMD delivers 9.5x speedup over scalar for float32 reduction on M2 with a 4 MB array.**
The scalar baseline runs at 4.12 cycles/element; the vectorized baseline runs at
0.432 cycles/element. The speedup is larger than the 4x SIMD width because vectorization
simultaneously eliminates the serial accumulator dependency that forced the scalar loop
to be load-latency bound.

**The bottleneck shifts completely between scalar and vector.**
Scalar is compute-plus-load-latency bound: the single accumulator sits on the critical
path, and every element must wait for the previous `fadd` AND the current load. Vector
code with 4+ independent accumulators removes the accumulator from the critical path;
the bottleneck becomes L2 bandwidth (~31 GB/s, measured directly from the timing).

**Branch misprediction is a separate, orthogonal performance cliff.**
A scalar conditional loop (`if (arr[i] > 0) s += arr[i]`) with ~50% random branch
rate measured 19.7 cycles/elem -- 4.8x slower than the branchless scalar loop at
4.12 cycles/elem. This extra cost comes purely from mispredict penalties, not from the
fadd or load. Eliminating the branch (via `vmaxq_f32`) AND vectorizing delivers a
combined 47x speedup over the branchy scalar baseline.

**`vmaxq_f32` is the SIMD equivalent of a conditional accumulation.**
`s += max(arr[i], 0)` is semantically equivalent to `if (arr[i] > 0) s += arr[i]`
for finite, non-NaN data. The `fmax.4s` instruction handles 4 lanes simultaneously with
2-cycle latency and no pipeline stall. This pattern directly models neural network ReLU
activation and any "sum of positives" operation.

**The compiler cannot make the `if → max` substitution without programmer help.**
The compiler knows about NaN and negative-zero semantics. Without `-ffast-math` and
without explicit `vmaxq_f32` intrinsics, both `cond_sum_scalar.cpp` and `cond_sum_compiler.cpp`
(compiled without `-fno-vectorize`) produce byte-for-byte identical scalar assembly.
Removing `-fno-vectorize` accomplished nothing. The only path to vectorization was
writing the intrinsic directly.

**The compiler's auto-vectorizer generated optimal code for the plain reduction.**
Clang at `-O2 -ffast-math` produced 4 independent NEON accumulators with `ldp q, q`
pair loads. Our hand-written version with 8 accumulators measured identically -- within
0.07%, which is noise. The compiler correctly identified that 4 accumulators was enough
to escape the compute-bound regime and reach the memory-bound ceiling.

**Doubling accumulators when bandwidth-bound accomplishes nothing.**
8 accumulators doubled the floats in flight but also doubled the data consumed per
iteration. The cycles-per-element ratio is invariant to this scaling. More accumulators
only help if the bottleneck is the accumulator dependency chain.

**The M2's L2 delivers ~31 GB/s of single-core streaming read bandwidth for this kernel.**
This ceiling was hit by three independent kernels: plain auto-vectorized (Case 2), plain
hand-written NEON (Case 3), and branch-free conditional NEON (Case 4b). Each measured
~30–31 GB/s. The bandwidth ceiling is a hardware constant that limits all three equally,
regardless of compute structure, as long as the compute intensity stays low.

**`-ffast-math` is the gate to auto-vectorization for FP reductions; intrinsics are
the gate when `-ffast-math` is semantically too broad.**
If you can accept any FP rounding order: use `-ffast-math`. If you need control over
which specific reordering is done (e.g., `max` substitution but not arbitrary reassociation):
write the intrinsic. Both achieve the hardware ceiling. The tradeoff is programmer
control vs. compiler convenience.
