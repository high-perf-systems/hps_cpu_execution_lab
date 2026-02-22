# Experiment 1: Dependent vs. Independent Instructions on Apple M2

## Execution Environment

| Property | Value |
|---|---|
| OS | macOS (Darwin Kernel 24.6.0) |
| Architecture | arm64 (Apple Silicon) |
| CPU | Apple M2 Max |
| Performance core frequency | Up to 3.33 GHz |
| L1 Data Cache | 64 KB |
| L2 Cache | 4 MB |
| RAM | 32 GB |
| Compiler | Apple clang 15.0.0 |
| Optimization | `-O2 -fno-vectorize` |
| Execution | Single-threaded, performance cores (no explicit affinity) |

---

## 1. Objective

To empirically observe the difference between **latency-bound** and **throughput-bound** execution on a modern out-of-order superscalar CPU, and to build intuition for:

- Instruction-Level Parallelism (ILP)
- Instruction latency vs. throughput
- Execution port pressure
- Loop-carried dependency chains
- How compilers and hardware interact to reshape what actually executes

---

## 2. Phase 1 — The Compiler Surprise

### 2.1 Initial Code

The first attempt used simple increment loops:

**Dependent version:**
```cpp
for (size_t i = 0; i < N; i++) {
    x = x + 1;
}
```

**Independent version:**
```cpp
for (size_t i = 0; i < N; i++) {
    a++; b++; c++; d++;
}
```

### 2.2 Observed Result

Both versions completed in **nanoseconds** regardless of `N`. This was unexpected — the independent version has 4× more arithmetic operations and should take meaningfully longer.

### 2.3 Root Cause: Assembly Inspection

Disassembly revealed the loop had been **completely eliminated**. The compiler recognized that both loops compute a deterministic closed-form result and replaced the entire loop with a single constant expression:

```
final_value = initial_value + N × constant
```

The CPU never executed a loop at runtime.

### 2.4 Lesson Learned

> **Microbenchmarks must always be validated by inspecting the generated assembly. Compilers aggressively eliminate loops whose results are statically predictable.**

Modern compilers perform algebraic simplification, strength reduction, and loop-invariant code motion. Any benchmark that does not force the compiler to treat intermediate values as observable will silently measure nothing.

---

## 3. Phase 2 — The Hidden Confound

### 3.1 Fix: Introducing Runtime Dependency

To prevent loop elimination, computation was tied to the loop variable at runtime:

```cpp
x += (i & 1);
```

Since `i` changes every iteration and is not known at compile time, the compiler can no longer collapse the loop.

### 3.2 Benchmark Code

**Dependent version** — single dependency chain on `x`:
```cpp
for (size_t i = 0; i < N; i++) {
    x += (i & 1);
}
```

**Independent version** — four separate accumulator chains:
```cpp
for (size_t i = 0; i < N; i++) {
    a += (i & 1);
    b += (i & 1);
    c += (i & 1);
    d += (i & 1);
}
```

### 3.3 Results (N = 100,000,000)

| Version | Runtime (µs) | Ratio |
|---|---|---|
| Dependent | ~42,950 | 1.00× |
| Independent | ~73,587 | 1.71× |

The independent version is only **1.71× slower**, not the naively expected 4× slower. This suggests the CPU is overlapping some of the work in parallel — but 1.71× is also far from the ~0.25× (4× speedup) one might hope for from four truly independent chains.

### 3.4 Assembly Analysis — Dependent Loop

```asm
LBB0_13:                          ; loop header
    and  x10, x8, #0x1            ; compute (i & 1) → x10
    add  x9,  x9,  x10            ; x += x10
    add  x8,  x8,  #1             ; i++
    cmp  x19, x8                  ; compare N, i
    b.ne LBB0_13
```

**Dependency graph per iteration:**

```
x8(prev) ──→ and ──→ x10 ──→ add(x9)
x8(prev) ──→ add(x8) ──→ cmp ──→ b.ne
```

Key observation: `x` (in `x9`) depends on `x10`, which depends on the loop counter `x8`. The loop counter is a **hidden dependency chain** feeding directly into the computation being measured. This benchmark does not purely isolate a latency-bound accumulator — it conflates accumulator latency with loop counter latency.

### 3.5 Assembly Analysis — Independent Loop

```asm
LBB0_13:                          ; loop header
    and  x10, x8, #0x1            ; compute (i & 1) → x10
    add  x9,  x9,  x10            ; a += x10
    add  x21, x21, x10            ; b += x10
    add  x23, x23, x10            ; c += x10
    add  x22, x22, x10            ; d += x10
    add  x8,  x8,  #1             ; i++
    cmp  x19, x8
    b.ne LBB0_13
```

The four `add` instructions are independent of each other and the CPU can dispatch them in parallel. However, **all four depend on `x10`**, which is the result of the single `and` instruction. This is a **fan-out bottleneck** — the four chains are not truly independent because they share a common upstream input computed once per iteration from the loop counter.

This explains the 1.71× result: some parallelism is exploited, but the fan-out from `and` and the loop counter chain serialize each iteration.

### 3.6 llvm-mca Confirmation — Dependent Loop

`llvm-mca` was used to simulate instruction-level scheduling on AArch64. The tool reports average cycles spent at each pipeline stage across simulated iterations.

```
      [0]    [1]    [2]    [3]   instruction
0.     10    4.3    0.1    2.4   and  x10, x8, #0x1
1.     10    7.5    0.0    0.0   add  x9, x9, x10
2.     10    4.6    0.8    3.7   add  x8, x8, #1
3.     10    7.1    1.7    0.0   cmp  x19, x8
4.     10    9.0    0.0    0.0   b.ne LBB0_13
       10    6.5    0.5    1.2   <total>

[1] = avg cycles waiting in scheduler queue
[2] = avg cycles waiting while ready (port contention)
[3] = avg cycles in reorder buffer after writeback (retirement wait)
```

**Reading the data:**

- `add x9` (the accumulator): waits 7.5 cycles, **zero port contention**. It is ready to execute the moment `x10` arrives but cannot start earlier — this is pure latency-bound behavior.
- `add x8` (loop counter): 0.8 cycles of port contention — it occasionally competes with the accumulator add for the same integer ALU port.
- `b.ne`: waits 9.0 cycles — last in the dependency chain, must wait for `cmp` which waits for `add x8` which waits for `and`.
- **Total port contention: 0.5 cycles** — near zero. The bottleneck is entirely latency, not execution port availability.

### 3.7 llvm-mca Confirmation — Independent Loop

```
      [0]    [1]    [2]    [3]   instruction
0.     10    1.4    0.3   15.0   and  x10, x8, #0x1
1.     10   16.2    2.6    0.0   add  x9,  x9,  x10
2.     10   16.6    2.8    0.0   add  x21, x21, x10
3.     10   17.2    2.8    0.0   add  x23, x23, x10
4.     10   17.6    3.0    0.0   add  x22, x22, x10
5.     10    1.1    0.8   17.5   add  x8,  x8,  #1
6.     10   18.0   16.5    0.0   cmp  x19, x8
7.     10   20.0    0.0    0.0   b.ne LBB0_13
       10   13.5    3.6    4.1   <total>
```

**Reading the data:**

- `and x10` executes quickly (1.4 cycles wait) but sits in the reorder buffer for **15 cycles** — its result is consumed by four downstream adds simultaneously, so the ROB must keep it live until all four have read it.
- The four `add` instructions show identical wait times (~16–17 cycles) confirming **parallel dispatch** — if they were serial, wait times would be cumulative.
- Port contention rises to **3.6 cycles** (vs 0.5 in the dependent case) — four instructions competing for ALU ports simultaneously creates measurable pressure.
- Despite this, the dominant source of waiting (~14 cycles) is not port contention but the loop-carried dependency: each iteration's `and` must wait for the previous iteration's `add x8` to complete, which itself waited for the full chain ahead of it.
- `cmp` shows 16.5 cycles of port contention — it is stuck behind both the four parallel adds and the loop counter update, all contending for ALU ports.

**Summary:** Port contention increased (0.5 → 3.6 cycles), confirming the CPU is attempting parallel execution. But the loop-carried dependency through `x8` remains the dominant bottleneck, not port availability. The fan-out design did not achieve true independence.

---

## 4. Phase 3 — Isolating the True Bottleneck

### 4.1 Design: Removing the Loop Counter from the Computation

To properly isolate latency-bound vs. throughput-bound behavior, the computation must not involve the loop counter `i` at all. The revised approach uses `x += x` (equivalent to left shift by 1), which forms a pure self-referential chain with no dependency on `i`:

**Dependent version:**
```cpp
for (size_t i = 0; i < N; i++) {
    x += x;
}
```

**Independent version:**
```cpp
for (size_t i = 0; i < N; i++) {
    a += a;
    b += b;
    c += c;
    d += d;
}
```

Assembly was inspected to confirm the compiler did not eliminate the loops.

### 4.2 Dependent Loop Assembly

```asm
LBB0_15:
    lsl  x8,  x8,  #1    ; x = x << 1  (i.e. x += x)
    subs x19, x19, #1    ; N-- and set flags
    b.ne LBB0_15
```

The compiler transformed `i++` / `i < N` into a **countdown from N to 0** using `subs`, which sets the zero flag directly and eliminates a separate `cmp` instruction. `lsl` and `subs` operate on completely different registers — they are **truly independent of each other** within each iteration. The only carried dependency is each instruction's output feeding its own next iteration.

### 4.3 Dependent Loop — Results and IPC

```
N            = 100,000,000
Runtime      = 34,622 µs
```

```
Cycles           = 0.034622 s × 3,330,000,000 Hz = 115,291,260
Instructions     = 3 per iteration × 100,000,000 = 300,000,000
IPC              = 300,000,000 / 115,291,260     = 2.60
Cycles/iteration = 115,291,260 / 100,000,000     = 1.15
```

### 4.4 Dependent Loop — llvm-mca

```
      [0]    [1]    [2]    [3]   instruction
0.     10    3.5    0.1    0.0   lsl  x8,  x8,  #1
1.     10    3.5    0.1    0.0   subs x19, x19, #1
2.     10    4.5    0.0    0.0   b.ne LBB0_15
       10    3.8    0.1    0.0   <total>
```

`lsl` and `subs` show **identical wait times (3.5 cycles)** and near-zero port contention (0.1 cycles each). This is the machine confirming they execute in parallel every iteration — they share no dependency and are dispatched to separate execution ports simultaneously. The 3.5 cycle wait reflects the depth of the out-of-order window across multiple overlapping iterations, not sequential stalling. Total port contention across the entire loop is **0.1 cycles** — essentially zero.

Cycles per iteration of **1.15** is close to the theoretical minimum of 1.0 for two parallel 1-cycle-latency chains. The small gap is explained by branch resolution overhead on a short loop and clock frequency variation under thermal management.

### 4.5 Independent Loop Assembly

```asm
LBB0_15:
    lsl  x11, x11, #1    ; d += d
    lsl  x10, x10, #1    ; c += c
    lsl  x9,  x9,  #1    ; b += b
    lsl  x8,  x8,  #1    ; a += a
    subs x19, x19, #1    ; N--
    b.ne LBB0_15
```

Six instructions per iteration. The four `lsl` operations all target different registers with no shared inputs — they are genuinely independent of each other.

### 4.6 Independent Loop — Results and IPC

```
N            = 100,000,000
Runtime      = 42,212 µs
```

```
Cycles           = 0.042212 s × 3,330,000,000 Hz = 140,565,960
Instructions     = 6 per iteration × 100,000,000 = 600,000,000
IPC              = 600,000,000 / 140,565,960     = 4.27
Cycles/iteration = 140,565,960 / 100,000,000     = 1.41
```

### 4.7 Independent Loop — llvm-mca

```
      [0]    [1]    [2]    [3]   instruction
0.     10    5.5    1.0    0.0   lsl  x11, x11, #1
1.     10    5.5    1.0    0.0   lsl  x10, x10, #1
2.     10    6.5    1.1    0.0   lsl  x9,  x9,  #1
3.     10    6.5    1.1    0.0   lsl  x8,  x8,  #1
4.     10    1.0    0.1    5.5   subs x19, x19, #1
5.     10    2.0    0.0    5.5   b.ne LBB0_15
       10    4.5    0.7    1.8   <total>
```

**Reading the data:**

- All four `lsl` instructions show the **same wait times** (5.5 or 6.5 cycles) — confirming simultaneous parallel dispatch. Serial execution would show increasing wait times.
- Port contention rises to **1.0–1.1 cycles** per `lsl` — four shift operations competing for execution ports creates real pressure, confirmed by measurement.
- `subs` executes almost immediately (1.0 cycle wait) because its chain (`x19`) is independent of the four `lsl` chains. But it then sits in the ROB for **5.5 cycles** waiting to retire — it has finished executing but must wait for the four `lsl` instructions ahead of it in program order to complete (the CPU retires instructions in-order even when it executes out-of-order).
- The 5.5 cycle ROB wait on `subs` and `b.ne` matches almost exactly the 5.5 cycle wait time of the `lsl` instructions — same pipeline depth seen from opposite perspectives.
- **Total port contention: 0.7 cycles** — higher than the dependent case (0.1) but still not the dominant cost. The primary overhead is branch resolution serializing the start of each iteration.

---

## 5. Cross-Experiment Summary

| Experiment | Instructions/iter | Cycles/iter | IPC | Port contention | Bottleneck |
|---|---|---|---|---|---|
| Dependent `i&1` | 5 | ~1.43 | 3.5 | 0.5 cycles | Loop counter latency chain |
| Independent `i&1` (4 adds) | 8 | ~1.71× slower | — | 3.6 cycles | Fan-out from `and`, loop counter |
| Clean dependent (`x += x`) | 3 | 1.15 | 2.60 | 0.1 cycles | Near-peak, 2 parallel chains |
| Clean independent (4× `a += a`) | 6 | 1.41 | 4.27 | 0.7 cycles | Branch resolution, port pressure |

**Key insight:** The clean independent loop achieves **4.27 IPC** — significantly higher than the dependent loop's 2.60 — proving the M2 is genuinely dispatching multiple instructions in parallel across its execution ports. Yet its *cycles per iteration* is higher (1.41 vs 1.15) because adding more instructions also adds more pressure on the branch resolution and retirement pipeline. Higher IPC does not automatically mean faster execution — what matters is cycles per iteration, and that is constrained by the slowest serialization point in the loop.

---

## 6. Concepts Demonstrated

**Latency** is the time for one instruction's result to become available to the next. A dependent chain is bounded by accumulated latency: `time ≈ N × latency_per_op`.

**Throughput** is the rate at which a CPU can sustain execution of a given instruction type across its execution ports. Independent instructions are bounded by throughput: `time ≈ N × instructions / port_width`.

**Out-of-order execution** allows the CPU to look ahead in the instruction stream and execute independent instructions before prior instructions have retired — this is why multiple iterations can be in-flight simultaneously, and why `subs` can execute while `lsl` instructions are still running.

**Loop-carried dependencies** are the dominant bottleneck in tight loops. Even when individual instructions are fast, a dependency that crosses iteration boundaries serializes execution at the granularity of that dependency's latency.

**Port contention** is real but secondary in these experiments. The M2's wide execution backend (estimated 6+ integer ALU ports) absorbs 4 simultaneous shift operations with only ~1 cycle of average contention — the architecture is genuinely wide.

**Compiler interaction** is non-negotiable knowledge for anyone doing performance work. The compiler transformed `i < N` counting-up into a `subs`-based countdown, merged a `cmp` into the subtract, and replaced `x += x` with `lsl`. The assembly is what executes — source code is a description of intent, not of execution.

---

## 7. Tooling Notes

**Assembly inspection:** `objdump -d` or Compiler Explorer (`godbolt.org`) with `--target=aarch64`.

**llvm-mca:** Static pipeline simulation tool. Run against just the hot loop body for accurate results. Apple's clang does not ship `llvm-mca` — install via `brew install llvm` and invoke as `/opt/homebrew/opt/llvm/bin/llvm-mca`. Use Apple's clang (`/usr/bin/clang++`) for compilation to avoid libc++ compatibility issues with Homebrew LLVM 21.

**Hardware PMU counters:** `xctrace` (Instruments) is the native macOS profiler but requires entitlements that are frequently killed in terminal sessions on M2. For cycle-level PMU access, Instruments.app is more reliable than the CLI. Apple Silicon does not expose per-execution-port counters publicly — `llvm-mca` simulation is the closest available substitute for port-level visibility.

**Timing:** `std::chrono::steady_clock` with microsecond resolution. Sufficient for N=100M workloads. For smaller N, switch to nanosecond resolution and run multiple trials to establish min/avg/max.
