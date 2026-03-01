# Experiment 2: Branch Prediction and Misprediction Cost — Notes

## 1. Experimental Design

This experiment isolates the cost of **branch misprediction** — specifically the pipeline
flush penalty — with memory effects explicitly excluded. Both branches perform identical
arithmetic work and the condition array is accessed sequentially in all cases. The hardware
prefetcher sees the same access pattern regardless of branch outcome. Memory is not a
variable here.

The only variable between cases is the **predictability of the branch outcome**.

| Case | Condition Array | Expected Misprediction Rate |
|---|---|---|
| Predictable | Sorted ascending | Near zero (long predictable runs) |
| Unpredictable | Random values 0–255 | ~50% |

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
single-threaded, working set fits in L1 cache.

---

## 2. Hypotheses

**H1 — Relative performance:** The predictable case will be significantly faster.
The M2 branch predictor recognizes patterns in branch history. A sorted array produces
long runs of the same branch outcome — the predictor locks onto each run with near-zero
misprediction rate. The random case forces ~50% mispredictions, each costing an estimated
10–16 cycle pipeline flush. Expected slowdown: **2–4×**.

**H2 — Sorted data:** The predictor handles sorted data well. The mechanism is pattern
recognition in branch outcome history. Long runs of identical outcomes are among the
easiest patterns for a predictor to learn.

**H3 — Theoretical penalty:** At 50% misprediction rate, N = 100,000,000, 15-cycle penalty:
```
Additional cycles = 15 × 0.50 × 100,000,000 = 750,000,000
Additional time   = 750,000,000 / 3,330,000,000 Hz ≈ 225 ms
```

---

## 3. Benchmark Design Journey — Compiler Obstacles

Getting a benchmark that actually measures branch prediction required three iterations.
The compiler aggressively eliminated branches in each attempt before a valid design
was found. This section documents why each attempt failed and what was learned.

### 3.1 First Attempt — Boolean Array with Simple Arithmetic

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
    sub   x12, x9, #1        ; precompute sum - 1  (else result)
    cmp   w11, #0             ; test condition
    csinc x9, x12, x9, eq    ; select: sum-1 if false, sum+1 if true — NO BRANCH
    add   x8, x8, #1
    b.ne  LBB0_5
```

**Why it failed:** The compiler applied **if-conversion** — replacing the branch with
`csinc` (Conditional Select Increment). It precomputed both outcomes simultaneously
(`sum-1` stored in `x12`, `sum+1` via the `csinc` increment) and selected between them
in a single instruction. No branch exists, so the predictor is never involved.

`csinc Xd, Xn, Xm, cond` semantics:
- If condition true:  `Xd = Xn`
- If condition false: `Xd = Xm + 1`

The compiler applies if-conversion whenever both branch paths are pure arithmetic on
registers with no side effects — both outcomes are cheap to precompute, and a
conditional select instruction can express the selection in one instruction.
Branchless execution avoids any misprediction cost, which is correct compiler behavior
for the general case.

### 3.2 Further Attempts — All Defeated by the Compiler

**Complex arithmetic bodies** (`sum += sum * 3 + 1` vs `sum += sum * 7 + 3`): The
compiler used ARM shifted register operands (`lsl #1`) to precompute both sides as
shift-and-add sequences, then selected with `csinc`. Multiply-by-small-constants is
free on ARM, so the bodies remained cheap enough for if-conversion.

**Memory writes inside branches**: Writing results to an output array. The compiler
proved the array was only observed after the loop and eliminated stores as dead code,
reducing back to register arithmetic and `csinc`.

**`__builtin_expect`**: Ignored by Apple clang for if-conversion purposes.

**`-fno-if-conversion`**: Not supported by Apple clang.

### 3.3 Final Design — Sorted vs Unsorted Threshold Comparison

```cpp
std::vector<uint8_t> data(N);
for (size_t i = 0; i < N; i++) data[i] = rand() % 256;

std::sort(data.begin(), data.end());   // predictable.cpp only

for (size_t i = 0; i < N; i++) {
    if (data[i] >= 128) sum += data[i];
}
```

This forces a real branch for three reasons. The condition `data[i] >= 128` on random
data is genuinely unpredictable at compile time — the compiler cannot prove the
distribution. The body `sum += data[i]` creates a data dependency on the loaded value,
making both-path precomputation expensive. The compiler cannot prove the branch is
biased enough to justify if-conversion.

This is also the pattern behind the famous Stack Overflow branch prediction demonstration
that showed sorting data before processing it can produce a 6× speedup.

---

## 4. Results (N = 100,000,000)

| Case | Runtime (µs) | Ratio |
|---|---|---|
| Predictable (sorted) | 36,629 | 1× (baseline) |
| Unpredictable (random) | 307,929 | 8.4× slower |

**Sum is identical in both cases: 9,574,441,916** — confirming both runs execute the
same algorithm over the same data values, differing only in order.

**Measured misprediction penalty:**
```
307,929 - 36,629 = 271,300 µs ≈ 271 ms additional time
```

**Hypothesis 3 validation:**
```
Predicted : ~225 ms
Measured  :  271 ms
```

Order of magnitude correct. The gap is because the baseline iteration cost is not
negligible — 36ms of real load and accumulation work adds to the total penalty.

**The 8.4× speedup significantly exceeded the 2–4× hypothesis.** See interpretation.

---

## 5. Assembly Analysis

Both `predictable.cpp` and `unpredictable.cpp` produce identical assembly — the code
is the same, only the data differs. The hot loop:

```asm
LBB0_35:                          ; =>This Inner Loop Header: Depth=1
    ldrsb  w9, [x8]               ; load data[i] as signed byte into w9
    tbz    w9, #31, LBB0_34       ; test bit 31 — if 0 (data[i]<128): skip to LBB0_34

; %bb.36: fall-through — data[i] >= 128
    and    x9, x9, #0xff          ; mask sign extension → recover true uint8 value
    ldr    x10, [sp, #24]         ; load sum from stack into x10
    add    x9, x10, x9            ; sum += data[i]
    str    x9, [sp, #24]          ; store sum back to stack
    b      LBB0_34                ; rejoin loop increment

LBB0_34:                          ; loop increment
    add    x8, x8, #1             ; advance data pointer
    subs   x20, x20, #1           ; N-- and set flags
    b.eq   LBB0_20                ; exit if N == 0, else fall through to LBB0_35
```

**`tbz` — a real branch.** `tbz` (Test Bit and Branch if Zero) is a genuine conditional
branch instruction — not `csinc`. The predictor must evaluate it every iteration.

**How `tbz` implements `data[i] >= 128`:** `ldrsb` sign-extends the loaded byte to
32 bits. Values 0–127 have bit 31 = 0 after sign extension. Values 128–255 have
bit 31 = 1. Testing bit 31 is therefore equivalent to the `>= 128` threshold — an
elegant single-instruction implementation derived automatically by the compiler.

**Two execution paths per iteration:**

```
data[i] < 128:   ldrsb → tbz (branch taken) → LBB0_34
                 3 instructions, no memory write

data[i] >= 128:  ldrsb → tbz (fall through) → and → ldr → add → str → LBB0_34
                 7 instructions, stack load + store for sum
```

**Sum is spilled to the stack.** Rather than keeping `sum` in a register, the compiler
stores it at `[sp, #24]` and reloads it each taken iteration. This adds a load-store
round trip to the critical path of every `sum += data[i]` execution, extending the
dependency chain length and amplifying the misprediction penalty.

---

## 6. llvm-mca Analysis

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

**Column [3] — ROB occupancy ~85 cycles across all instructions.**

Every instruction executes quickly but waits ~85 cycles in the reorder buffer before
retiring. This is the signature of deep speculative execution — the out-of-order window
holds many iterations simultaneously. Instructions complete but cannot retire until all
prior instructions in program order commit. The CPU is productive; retirement is the
bottleneck, not execution.

**`tbz` waits 8.6 cycles, zero port contention.**

Waiting entirely for `w9` from `ldrsb`. L1 load latency on M2 is ~4 cycles. The 8.6
cycle average reflects multiple overlapping iterations in the out-of-order window, each
waiting on their respective loads.

**The taken path dependency chain:**

```
ldrsb w9            (memory load — ~4 cycle latency)
    ↓
tbz  w9, #31        (waits for w9)
    ↓
and  x9, x9, #0xff  (waits for w9)
    ↓
add  x9, x10, x9    (waits for x9 from and AND x10 from ldr — join point)
    ↓
str  x9, [sp, #24]  (waits for x9 from add)
```

Five serial instructions. The `add` at instruction 7 is a join point — it must wait
for both the `and` result (x9) and the stack load result (x10). It cannot execute until
the slower of the two arrives.

**`ldr x10, [sp, #24]` — 100% port contention (1.1 cycles).**

The sum stack load has no data dependency so it is ready immediately — but competes
with `ldrsb` for the same load execution port. This is the only significant port
contention in the loop.

**Total port contention: 1.1 cycles.** The loop is latency-bound on the load-to-use
chain, not port-bound.

---

## 7. Interpretation

### 7.1 Why Predictable Is 8.4× Faster

The sorted array produces two long runs of identical branch outcomes:

```
Iterations 0 – ~50M:  data[i] < 128  → branch not taken, every iteration
[transition: ~1-2 mispredictions]
Iterations ~50M – N:  data[i] >= 128 → branch taken, every iteration
```

The predictor quickly learns each run and predicts correctly for ~50M consecutive
iterations. Total mispredictions: approximately 1–2 at the single transition point.
The pipeline stays full and productive for essentially the entire run.

The random array produces no learnable pattern. The predictor is wrong ~50% of the
time — 50 million mispredictions, each flushing the pipeline and wasting ~15 cycles
of speculative work that must be discarded and redone.

### 7.2 Why the Speedup Was 8.4× Not 2–4×

The hypothesis underestimated the penalty for two reasons.

Each misprediction flushes an entire window of speculative work — not just one
instruction but potentially 10–20 instructions already in-flight across multiple
iterations. The deep out-of-order window of the M2 means more work is in-flight
and more is wasted per flush.

The taken path has a 5-instruction serial dependency chain ending in a stack store.
After a misprediction, this entire chain must be re-executed from the correct starting
point. The combination of pipeline refill cost and re-execution of the serial chain
produces a larger penalty than the simple `mispredictions × penalty_cycles` model.

### 7.3 Branchless vs Branchy — When Each Wins

The compiler's `csinc` optimization in the first benchmark attempts was not a bug — it
was correct behavior. For a branch with a simple symmetric body and unpredictable
outcomes, branchless code is genuinely faster because it eliminates misprediction cost
entirely by always computing both outcomes.

The crossover point:

```
Branchless wins: unpredictable branch + cheap symmetric body
Branchy wins:    predictable branch (any body) OR expensive asymmetric body
```

This is a real design decision in performance-critical code. Database engines, parsers,
and network packet processors are explicitly designed around this tradeoff.

### 7.4 The Stack Spill Amplifies the Penalty

Sum being spilled to the stack rather than kept in a register adds a load-store round
trip to every taken iteration. In the predictable case this is a fixed overhead the
pipeline handles efficiently. In the unpredictable case, each misprediction forces
re-execution of the longer spill-inclusive dependency chain, amplifying the per-
misprediction cost beyond what a register-resident `sum` would incur.

---

## 8. Key Takeaways

**Branch misprediction is expensive.** A single misprediction on M2 costs 10–16 cycles
of pipeline flush plus re-execution. At ~50M mispredictions over 100M iterations, this
produced 271ms of overhead and an 8.4× slowdown.

**The predictor learns patterns in branch outcome history, not data values.** Sorted
data produces long predictable runs. Random data produces unpredictable outcomes. The
data values themselves are irrelevant — only the sequence of taken/not-taken matters.

**Branchless code is not always faster.** For truly unpredictable branches with cheap
symmetric bodies, the compiler's if-conversion (csinc/csel) produces better code.
For predictable branches, real branches are faster because speculation is productive.

**Microbenchmark design requires assembly validation.** The compiler eliminated branches
in three successive attempts before a valid experiment was achieved. What you write in
C++ and what the CPU executes can be fundamentally different. Always inspect the assembly
before trusting timing numbers.

**Stack spills extend the misprediction penalty.** When a hot variable is spilled to
the stack instead of kept in a register, each misprediction must re-execute a longer
dependency chain. Register allocation quality directly affects misprediction cost.