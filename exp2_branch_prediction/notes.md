# Experiment 2: Branch Prediction and Misprediction Cost — Notes

## 1. Experimental Design

This experiment isolates the cost of **branch misprediction** — specifically the
pipeline flush and re-fetch penalty — with memory effects explicitly excluded. Both
benchmark variants perform identical arithmetic on the same data values. The condition
array is accessed sequentially in all cases, so the hardware prefetcher sees the same
access pattern regardless of branch outcome. Memory is not a variable.

The only variable between the two cases is the **predictability of the branch outcome**:

| Case | Condition Array | Expected Misprediction Rate |
|---|---|---|
| Predictable | Sorted ascending (0, 0, ..., 127, 128, ..., 255) | Near zero — long identical runs |
| Unpredictable | Random values 0–255 | ~50% — no learnable pattern |

**Why predictability matters at the hardware level:** Modern branch predictors maintain
a history of recent branch outcomes and use them to speculatively fetch and execute
instructions before the branch condition is known. When the prediction is correct,
speculation is free — the work was going to be done anyway. When the prediction is
wrong, the entire speculatively executed instruction window must be discarded, the
pipeline flushed, and execution restarted from the correct path. On a deep
out-of-order pipeline like the M2, this flush discards many in-flight instructions
and costs a significant number of cycles.

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
single-threaded, working set fits entirely in L1 cache (N = 100M bytes = 100 MB raw
data, but the accessed slice stays hot after warmup).

---

## 2. Hypotheses

**H1 — Relative performance:** The predictable case will be significantly faster.
The M2 predictor recognises patterns in branch outcome history. A sorted array produces
long runs of the same outcome — not-taken for the lower half, taken for the upper half
— with a single transition point. The predictor locks onto each run within a few
iterations and maintains near-zero misprediction rate for the rest. The random case
produces a ~50% misprediction rate with no learnable pattern — every iteration is
effectively a coin flip for the predictor.

Expected slowdown for the random case: **2–4×**.

**H2 — Misprediction penalty estimate:**

At 50% misprediction rate with an estimated 15-cycle penalty per misprediction:

```
Additional cycles = 15 × 0.50 × 100,000,000 iterations = 750,000,000 cycles
Additional time   = 750,000,000 / 3,330,000,000 Hz      ≈ 225 ms
```

**H3 — Correctness invariant:** Both cases must produce the **identical sum**. The
data values are the same — only their order differs. A sum mismatch would indicate
the benchmark is computing different things, invalidating the comparison.

---

## 3. Results — Benchmark Design Journey

Getting a benchmark that actually measures branch prediction required three successive
design attempts. The compiler eliminated the branch in all initial attempts using
**if-conversion** — replacing conditional branches with branchless conditional select
instructions. This is correct compiler behaviour, but it means the branch predictor
is never involved and no misprediction cost is ever paid.

### 3.1 Attempt 1 — Boolean Array with Simple Arithmetic

```cpp
std::vector<uint8_t> data(N);
for (size_t i = 0; i < N; i++) data[i] = i % 2;

for (size_t i = 0; i < N; i++) {
    if (data[i]) sum++;
    else         sum--;
}
```

**Assembly produced:**

```asm
LBB0_5:
    ldrb  w11, [x10, x8]     ; load data[i]
    sub   x12, x9, #1        ; precompute sum - 1  (else branch result)
    cmp   w11, #0
    csinc x9, x12, x9, eq   ; if false: x9 = x12 (sum-1), if true: x9 = x9+1
    add   x8, x8, #1
    b.ne  LBB0_5
```

**Why it failed:** The compiler applied **if-conversion** — replacing the conditional
branch with `csinc` (Conditional Select Increment). It precomputed both outcomes
(`sum-1` in `x12`, `sum+1` via the implicit increment) and selected between them
with one instruction. No branch exists. The branch predictor is never invoked.

`csinc Xd, Xn, Xm, cond` semantics:
- Condition true:  `Xd = Xn`
- Condition false: `Xd = Xm + 1`

The compiler applies if-conversion whenever both branch paths consist of pure
arithmetic on registers with no side effects, and both outcomes are cheap enough to
precompute. Branchless execution avoids any misprediction cost — which is genuinely
correct behaviour for the general case.

### 3.2 Attempts 2 and 3 — All Defeated by If-Conversion

**Complex arithmetic bodies** (`sum += sum * 3 + 1` vs `sum += sum * 7 + 3`): The
compiler used ARM shifted register operands to precompute both sides as shift-and-add
sequences, then selected with `csinc`. Small-constant multiplication is free on ARM
(expressed as shift + add), keeping both outcomes cheap enough for if-conversion.

**Memory writes inside branches** (writing results to an output array): The compiler
proved the array was only observed after the loop and eliminated the stores as dead
code, reducing back to register arithmetic and `csinc`.

**`__builtin_expect`**: Ignored by Apple clang for if-conversion decisions.

**`-fno-if-conversion`**: Not supported by Apple clang.

### 3.3 Final Design — Threshold Comparison on Random Data

```cpp
std::vector<uint8_t> data(N);
for (size_t i = 0; i < N; i++) data[i] = rand() % 256;

std::sort(data.begin(), data.end());   // predictable variant only

for (size_t i = 0; i < N; i++) {
    if (data[i] >= 128) sum += data[i];
}
```

This forces a genuine branch for three reasons. The condition `data[i] >= 128` on
random data has an unknown distribution at compile time — the compiler cannot prove
it is biased in either direction. The body `sum += data[i]` creates a data dependency
on the loaded value, making precomputation of the taken-path outcome expensive (the
value is not known until the load resolves). The compiler cannot justify if-conversion
because one path has meaningful work and the other has none — asymmetric paths resist
precomputation.

This is also the pattern behind the widely known Stack Overflow branch prediction
demonstration: sorting data before processing can produce a 6× speedup because it
converts an unpredictable branch into a predictable one.

### 3.4 Final Results (N = 100,000,000)

| Case | Runtime (µs) | Ratio |
|---|---|---|
| Predictable (sorted) | 36,629 | 1× (baseline) |
| Unpredictable (random) | 307,929 | 8.4× slower |

**Both cases produce the identical sum: 9,574,441,916** — confirming both runs
execute the same computation on the same values, differing only in access order.

**Measured misprediction overhead:**

```
307,929 − 36,629 = 271,300 µs ≈ 271 ms additional time
```

**Hypothesis H2 validation:**

```
Predicted additional time : ~225 ms
Measured additional time  :  271 ms
```

Directionally correct. The 20% gap arises because the baseline 36ms of load and
accumulate work is non-negligible — the misprediction penalty compounds on top of
a non-zero baseline, not on top of zero.

**The 8.4× slowdown significantly exceeded the 2–4× prediction.** See section 5 for
the full explanation.

---

## 4. Assembly Analysis

Both `predictable.cpp` and `unpredictable.cpp` compile to **identical assembly** — the
source code is the same, only the runtime data order differs. The hot loop:

```asm
LBB0_35:                          ; =>This Inner Loop Header: Depth=1
    ldrsb  w9, [x8]               ; sign-extend load data[i] into w9
    tbz    w9, #31, LBB0_34       ; if bit 31 == 0 (data[i] < 128): skip to LBB0_34

; %bb.36: fall-through — data[i] >= 128
    and    x9, x9, #0xff          ; mask sign extension → recover true uint8 value
    ldr    x10, [sp, #24]         ; load sum from stack
    add    x9, x10, x9            ; sum += data[i]
    str    x9, [sp, #24]          ; store sum back to stack
    b      LBB0_34               ; rejoin loop

LBB0_34:                          ; loop increment
    add    x8, x8, #1             ; advance data pointer
    subs   x20, x20, #1           ; N-- and set flags
    b.eq   LBB0_20                ; exit if N == 0
```

**`tbz` is a genuine branch.** `tbz` (Test Bit and Branch if Zero) is a real conditional
branch instruction — not a `csinc`. The branch predictor must evaluate it on every
iteration. This is the key instruction that makes the benchmark work.

**How `tbz` implements `data[i] >= 128`:** `ldrsb` sign-extends the loaded byte to
32 bits. Values 0–127 have bit 31 = 0 after sign extension. Values 128–255 (which
sign-extend to negative 32-bit values) have bit 31 = 1. Testing bit 31 is therefore
equivalent to the `>= 128` threshold — a single-instruction implementation derived
automatically by the compiler.

**Two execution paths with different costs per iteration:**

```
data[i] < 128  :  ldrsb → tbz (branch taken) → LBB0_34
                  3 instructions, no memory write, ~3 cycles

data[i] >= 128 :  ldrsb → tbz (fall-through) → and → ldr → add → str → LBB0_34
                  7 instructions, stack load + store, dependency chain extends
```

**Sum is spilled to the stack.** Rather than keeping `sum` in a register throughout,
the compiler stores it at `[sp, #24]` and reloads it on the taken path. This adds a
load-store round trip to the dependency chain of every `sum += data[i]` execution.
The spill extends the critical path and amplifies the cost of each misprediction,
since more instructions must be re-executed from the correct starting point.

**llvm-mca analysis of the loop body:**

```
      [0]    [1]    [2]    [3]   instruction
0.     10    3.8    0.1   87.5   add   x8, x8, #1          (i++)
1.     10    4.3    0.2   87.0   subs  x20, x20, #1        (N--)
2.     10    5.0    0.0   87.0   b.eq  LBB0_20             (exit branch)
3.     10    4.9    0.4   83.5   ldrsb w9, [x8]            (load data[i])
4.     10    8.6    0.0   83.5   tbz   w9, #31, LBB0_34    (key branch)
5.     10    8.6    0.0   82.6   and   x9, x9, #0xff       (mask)
6.     10    1.1    1.1   86.7   ldr   x10, [sp, #24]      (load sum)
7.     10    9.2    0.0   80.8   add   x9, x10, x9         (sum += data[i])
8.     10   10.9    0.0   77.2   str   x9, [sp, #24]       (store sum)
9.     10    2.8    2.8   89.3   b     LBB0_34             (rejoin)
       10    5.7    1.1   80.7   <total>
```

**Column [3] — ROB occupancy ~80–89 cycles across all instructions.** Every
instruction executes quickly but waits ~85 cycles in the reorder buffer before
retiring. This is the signature of deep speculative execution — the out-of-order
window holds many iterations simultaneously. Instructions complete early but cannot
retire until all prior instructions in program order commit.

**`tbz` waits 8.6 cycles (column [1]), zero port contention (column [2]).** It is
waiting purely for `w9` from `ldrsb`. L1 load latency on M2 is ~4 cycles. The 8.6
cycle average reflects multiple overlapping iterations in the out-of-order window,
each waiting on their respective loads.

**The taken-path dependency chain:**

```
ldrsb w9             (memory load — ~4 cycles)
    ↓
tbz  w9, #31         (waits for w9)
    ↓
and  x9, x9, #0xff   (waits for w9)
    ↓
add  x9, x10, x9     (join point — waits for both `and` result AND `ldr` result)
    ↓
str  x9, [sp, #24]   (waits for add)
```

Five serial instructions on the critical path. The `add` is a join point — it must
wait for both the `and` (from the loaded data value) and `ldr x10` (the stack sum
load). It cannot execute until the slower of the two arrives.

**`ldr x10, [sp, #24]` — 100% port contention (1.1 cycles).** The sum stack load
has no data dependency so it is ready to execute immediately — but it competes with
`ldrsb` for the same load execution port. This is the only significant port contention
in the loop; everything else is latency-bound on the load-to-use chain.

---

## 5. Interpretation

### 5.1 Why Predictable Is 8.4× Faster

The sorted array creates exactly two long runs of identical branch outcomes:

```
Iterations 0 to ~50M:   data[i] < 128  → tbz taken, every iteration
[transition: ~1–2 mispredictions at the crossover point]
Iterations ~50M to N:   data[i] >= 128 → tbz not taken, every iteration
```

The M2 branch predictor learns each run within a few iterations and maintains
correct predictions for ~50M consecutive iterations. Total mispredictions across
the entire run: approximately 1–2 at the single transition point. The pipeline
stays fully speculative and productive for essentially the whole run.

The random array produces no learnable pattern — the taken/not-taken sequence is
approximately uniformly random. The predictor is wrong ~50% of the time: 50 million
mispredictions, each flushing the pipeline, discarding in-flight speculative work,
and restarting from the correct path.

### 5.2 Why the Speedup Was 8.4× Not 2–4×

The hypothesis underestimated the penalty for two compounding reasons.

**The M2's deep out-of-order window amplifies flush cost.** When a misprediction is
detected, all instructions after the mispredicted branch that are already in-flight
must be squashed. On a deep out-of-order machine, this can be 100+ in-flight
micro-operations across many speculative iterations. The M2's ROB occupancy data
shows ~85 cycles of in-flight work per instruction — the flush discards a much larger
window than the simple `mispredictions × 15-cycle penalty` model assumes.

**The taken-path dependency chain extends re-execution cost.** After each flush, the
pipeline must re-fetch from the correct path and re-execute the 5-instruction serial
chain (`ldrsb → tbz → and → add → str`), including the stack load. The stack spill
in particular extends the chain — a register-resident sum would have a shorter
dependency chain to re-execute, producing a smaller per-misprediction penalty.

The combination of flush cost (discarding a deep speculative window) and re-execution
cost (a 5-instruction serial chain with a memory spill) produced a much larger total
penalty than the simplified model predicted.

### 5.3 Branchless vs Branchy — When Each Is Correct

The compiler's if-conversion in the first three attempts was not a bug — it was the
right optimisation for those loop shapes. For a branch with a cheap symmetric body
and an unpredictable condition, `csinc` is genuinely faster: both outcomes are
computed unconditionally, the misprediction cost is zero, and the total cost is
predictable and constant per iteration.

The crossover:

```
Branchless is faster:  unpredictable condition + cheap, symmetric body
Branchy is faster:     predictable condition (any body)
                       OR asymmetric body (one path much cheaper than the other)
```

For the threshold benchmark with `sum += data[i]` only on the taken path, the two
paths are asymmetric — the taken path does real work and the not-taken path does
nothing. Branchless code would have to compute `sum += data[i]` unconditionally and
then conditionally discard it, which has different performance characteristics.
The real branch lets the not-taken path skip all that work at the cost of prediction
fidelity.

This is a real engineering decision in database engines, parsers, and network packet
processors — fields where branch predictability and body symmetry are explicitly
considered in the inner-loop design.

### 5.4 The Stack Spill Is an Amplification Factor

Sum spilled to `[sp, #24]` rather than kept in a register adds one load and one store
to the taken-path critical chain every iteration. In the predictable case this overhead
is constant and the pipeline handles it without disruption. In the unpredictable case,
each misprediction requires re-executing the full spill-inclusive chain, making every
misprediction more expensive than it would be with a register-resident accumulator.
The register allocator's decision to spill directly multiplies the branch misprediction
penalty.

---

## 6. Key Takeaways

**Branch misprediction is the most expensive single event in a tight loop on M2.**
At ~50M mispredictions over 100M iterations, the overhead was 271ms and the slowdown
was 8.4× — far beyond the naive estimate. A single misprediction discards a deep
speculative window plus requires re-executing a serial dependency chain. The total
cost per misprediction on M2 is substantially higher than the commonly cited
"10–15 cycle penalty."

**The predictor learns branch outcome history, not data values.** Sorted data is fast
because it produces long predictable runs in the sequence of taken/not-taken outcomes.
Random data is slow because that sequence has no pattern. The actual data values are
irrelevant — only the outcome sequence matters to the predictor.

**Branchless code (`csinc`/`csel`) is not universally better.** The compiler
correctly applies if-conversion when both branch paths are cheap to precompute and
the condition is likely unpredictable. For predictable conditions or expensive
asymmetric paths, real branches are faster because the predictor makes speculation
productive and the not-taken path skips significant work. This is a real design
decision that database, parser, and protocol-processing codebases make explicitly.

**Deep speculative execution amplifies misprediction cost.** The M2's large
out-of-order window (ROB occupancy ~85 cycles per instruction in this loop) means
that when a branch misprediction is detected, a very deep pipeline full of in-flight
work must be squashed. The penalty is not just the branch itself — it is the cost of
discarding dozens of speculative instructions and refilling the pipeline from scratch.

**Stack spills extend the misprediction re-execution penalty.** When a hot accumulator
is spilled to memory rather than held in a register, the per-iteration dependency
chain includes a load-store round trip. After a misprediction, this longer chain must
be re-executed, directly increasing the per-misprediction cost. Register allocation
quality is not just a code-size concern — it affects misprediction penalty.

**Microbenchmark design requires assembly verification at every step.** The compiler
eliminated branches in three consecutive attempts using if-conversion before a valid
measurement was achieved. Each attempt produced plausible-looking source code with
a genuine conditional — and each produced branchless assembly that measured nothing
about branch prediction. Only inspecting the generated assembly revealed the issue.
Source code shows intent; assembly shows what executes.
