# Experiment 1: Instruction-Level Parallelism on Apple M2 — Notes

## 1. Experimental Design

This experiment empirically measures the difference between **latency-bound** and
**throughput-bound** execution on the Apple M2's out-of-order superscalar core, and
builds direct intuition for Instruction-Level Parallelism (ILP).

Two loop structures are compared:

- **Dependent loop:** A single accumulator chain where each iteration reads the value
  written by the previous iteration — a strict Read-After-Write (RAW) dependency. The
  CPU cannot begin iteration N+1 until iteration N's result is available. ILP is not
  exploitable within this chain.

- **Independent loop:** Four separate accumulator chains with no shared inputs. Each
  chain depends only on its own previous output. The CPU can theoretically dispatch all
  four in parallel every cycle, filling multiple execution ports simultaneously.

The key design challenge is constructing a workload that the compiler cannot eliminate
or collapse into a trivial constant. This required three phases of iterative refinement,
each revealing a different compiler optimisation that had to be designed around.

**Why this matters for hardware intuition:** On a modern out-of-order CPU, instructions
are dispatched to multiple execution units in parallel. A tight dependent chain forces
sequential execution, limited by instruction latency. A wide independent workload can
fill all available execution ports, limited instead by port throughput. The gap between
these two regimes is the practical ceiling on ILP gain — and understanding what limits
that ceiling (latency, port pressure, loop control overhead, or compiler artefacts) is
the central question of this experiment.

| Property | Value |
|---|---|
| CPU | Apple M2 Max |
| Performance core frequency | Up to 3.33 GHz |
| L1 Data Cache | 64 KB |
| L2 Cache | 4 MB |
| Compiler | Apple clang 15.0.0 |
| Optimization | `-O2 -fno-vectorize` |
| Execution | Single-threaded, performance cores |

---

## 2. Hypotheses

### 2.1 Dependent Loop

A strict RAW chain — each iteration's output feeds the next. The CPU cannot overlap
iterations because the next instruction cannot begin until the prior one completes.
For `lsl` (left shift, 1-cycle latency on M2), the theoretical minimum is
**1 cycle per iteration** — one new instruction issued per cycle, one retired per
cycle, no parallelism possible beyond overlapping the countdown chain.

### 2.2 Independent Loop (4 chains)

Four chains with no shared inputs. The CPU can theoretically dispatch all four to
separate integer ALU ports every cycle. In practice three friction points reduce the
ideal 4× speedup:

1. **Port pressure:** four shifts competing for integer shift ports creates queuing.
2. **Branch resolution:** `b.ne` at the end of each iteration serializes the start
   of the next fetch — a structural bottleneck that both loops share equally.
3. **ROB pressure:** four in-flight chains mean more instructions in the reorder
   buffer simultaneously, increasing retirement backpressure.

Expected result: cycles per iteration **between 1.0 and 1.5**, with IPC significantly
higher than the dependent case (more instructions execute per clock), but cycles per
iteration also higher (more instructions to complete per loop).

### 2.3 What Would Confirm ILP Is Happening

- IPC of the independent loop should be meaningfully higher than the dependent loop.
- llvm-mca should show the four `lsl` instructions dispatching with **identical** wait
  times (parallel dispatch) rather than cumulative increasing times (serial execution).
- Port contention should be higher in the independent loop — a sign the CPU is genuinely
  attempting to use multiple ports.

---

## 3. Results — Measurement Journey

Getting a clean ILP signal required three phases. The first two failed because the
compiler either eliminated the loop entirely or introduced a hidden dependency through
the loop counter. Each failure taught something important about the compiler-hardware
interface.

### 3.1 Phase 1 — Compiler Loop Elimination

**Initial code:**

```cpp
// Dependent
for (size_t i = 0; i < N; i++) x = x + 1;

// Independent
for (size_t i = 0; i < N; i++) { a++; b++; c++; d++; }
```

**Result:** Both versions completed in nanoseconds regardless of N.

**Root cause:** The compiler recognised that both loops compute a deterministic
closed-form result and replaced the entire loop body with a single constant expression —
`final_value = initial_value + N × constant`. The CPU executed zero loop iterations
at runtime. Assembly inspection confirmed the loop was absent from the binary.

**Lesson:** Microbenchmarks must always be validated by inspecting the generated
assembly. Compilers perform algebraic simplification and loop-invariant code motion
aggressively. If the result is statically predictable, the loop will be eliminated.

### 3.2 Phase 2 — Hidden Loop Counter Dependency

**Fix:** Tied computation to the loop variable at runtime: `x += (i & 1)`.

Since `i` changes every iteration and is not a compile-time constant, the compiler
cannot collapse the loop. The loops survive in the binary.

**Results (N = 100,000,000):**

| Version | Runtime (µs) | Ratio |
|---|---|---|
| Dependent | ~42,950 | 1.00× |
| Independent (4 adds) | ~73,587 | 1.71× |

The 1.71× result was puzzling — expected either close to 1× (if the CPU fully
parallelised the four chains) or 4× (if it executed them serially). 1.71× suggests
partial parallelism but something is interfering.

**Root cause — fan-out bottleneck:** Assembly inspection revealed that all four `add`
instructions consumed the same `x10` register, produced by a single `and x10, x8, #1`
instruction that itself depended on the loop counter `x8`:

```asm
LBB0_13:
    and  x10, x8, #0x1    ; depends on loop counter x8
    add  x9,  x9,  x10    ; a += x10
    add  x21, x21, x10    ; b += x10  } all four read the same x10
    add  x23, x23, x10    ; c += x10  }
    add  x22, x22, x10    ; d += x10  }
    add  x8,  x8,  #1     ; i++
    cmp  x19, x8
    b.ne LBB0_13
```

The four chains share a common upstream `and` whose input is the loop counter. This is
a **fan-out bottleneck** — the four adds are independent of each other but all gated
on a single upstream result. Additionally, the loop counter update chain (`add x8 → cmp
→ b.ne`) feeds into the computation, coupling the arithmetic latency to the loop control
latency.

**llvm-mca confirmed:** the `cmp` showed 16.5 cycles of port contention from everything
converging at that point, and the `and` result sat in the reorder buffer for 15 cycles
while all four consumers read it.

**Lesson:** Tying computation to `i` introduces a hidden loop-counter dependency. The
four chains were not truly independent. To isolate ILP cleanly, the workload must be
**self-referential** — each chain depending only on its own previous output, with no
connection to the loop counter.

### 3.3 Phase 3 — Clean Isolation (Definitive Results)

**Design:** `x += x` — equivalent to `x = x << 1`. Each chain depends only on its
own previous value. The loop counter has no connection to the arithmetic.

**Dependent loop:**

```
N            = 100,000,000
Runtime      = 34,622 µs
Cycles       = 115,291,260
Instructions = 300,000,000  (3 per iteration: lsl + subs + b.ne)
IPC          = 2.60
Cycles/iter  = 1.15
```

**Independent loop (4 chains):**

```
N            = 100,000,000
Runtime      = 42,212 µs
Cycles       = 140,565,960
Instructions = 600,000,000  (6 per iteration: 4×lsl + subs + b.ne)
IPC          = 4.27
Cycles/iter  = 1.41
```

### 3.4 Hypothesis Validation

| Prediction | Expected | Measured | Verdict |
|---|---|---|---|
| Dependent cycles/iter | ~1.0 cycle | 1.15 cycles | ✓ Near theoretical minimum |
| Independent cycles/iter | 1.0–1.5 cycles | 1.41 cycles | ✓ Within predicted range |
| IPC higher for independent | Yes | 4.27 vs 2.60 | ✓ Confirmed |
| Port contention higher for independent | Yes | 0.7 vs 0.1 cycles | ✓ Confirmed |
| Parallel dispatch (identical mca wait times) | Yes | Yes (5.5/6.5 for all 4 lsl) | ✓ Confirmed |

---

## 4. Assembly Analysis

### 4.1 Dependent Loop

```asm
LBB0_15:
    lsl  x8,  x8,  #1    ; x = x << 1  (x += x)
    subs x19, x19, #1    ; N-- and set flags (countdown replaces cmp)
    b.ne LBB0_15
```

Three instructions. The compiler transformed the ascending `i < N` loop into a
**countdown from N to 0** using `subs`, which sets the zero flag directly and eliminates
a separate `cmp`. `lsl` and `subs` operate on completely different registers — they are
**genuinely independent within each iteration**. Two parallel dependency chains run
simultaneously: the shift chain (`x8`) and the countdown chain (`x19`).

**llvm-mca:**

```
      [0]    [1]    [2]    [3]   instruction
0.     10    3.5    0.1    0.0   lsl  x8,  x8,  #1
1.     10    3.5    0.1    0.0   subs x19, x19, #1
2.     10    4.5    0.0    0.0   b.ne LBB0_15
       10    3.8    0.1    0.0   <total>
```

`lsl` and `subs` show **identical scheduler wait times (3.5 cycles)** — the machine
confirming they execute in parallel every iteration. Near-zero port contention (0.1 cycles).
Total port contention: **0.1 cycles** — essentially zero. Throughput is limited by
latency, not port availability.

Cycles per iteration of **1.15** is close to the theoretical minimum of 1.0 for two
parallel 1-cycle-latency chains. The small gap comes from branch resolution overhead
at loop entry and minor frequency variation under thermal management.

### 4.2 Independent Loop

```asm
LBB0_15:
    lsl  x11, x11, #1    ; d += d
    lsl  x10, x10, #1    ; c += c
    lsl  x9,  x9,  #1    ; b += b
    lsl  x8,  x8,  #1    ; a += a
    subs x19, x19, #1    ; N--
    b.ne LBB0_15
```

Six instructions per iteration. The four `lsl` operations target different registers
with no shared inputs — **genuinely independent** of each other. Each forms its own
loop-carried chain, and the four chains never interact.

**llvm-mca:**

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

- All four `lsl` instructions show **identical wait times** (5.5 or 6.5 cycles) —
  confirming simultaneous parallel dispatch. Serial execution would show increasing,
  cumulative wait times.
- Port contention rises to **1.0–1.1 cycles per lsl** — four shift operations competing
  for the same integer shift ports creates measurable queuing pressure.
- `subs` executes at only 1.0 cycle wait (its chain is independent of the four `lsl`
  chains) but sits in the ROB for **5.5 cycles** — it has finished executing but cannot
  retire because the CPU retires instructions **in program order** even when executing
  out-of-order. It must wait for the four `lsl` instructions ahead of it.
- The 5.5 cycle ROB wait on `subs` and `b.ne` matches the 5.5 cycle scheduler wait of
  the `lsl` instructions — the same pipeline depth seen from opposite perspectives.
- **Total port contention: 0.7 cycles** — higher than the dependent case (0.1) but
  not the dominant cost. Branch resolution is still the primary serialisation point.

---

## 5. Interpretation

### 5.1 IPC vs Cycles per Iteration — Two Different Questions

| Loop | Instr/iter | Cycles/iter | IPC | Port contention |
|---|---|---|---|---|
| Dependent (`x += x`) | 3 | 1.15 | 2.60 | 0.1 cycles |
| Independent (4× `a += a`) | 6 | 1.41 | 4.27 | 0.7 cycles |

The independent loop achieves **4.27 IPC** vs the dependent loop's **2.60 IPC** —
the M2 is genuinely dispatching more instructions per clock when they are independent.
Yet the independent loop's cycles per iteration is **higher** (1.41 vs 1.15).

This is not a contradiction. There are twice as many instructions to complete per
iteration (6 vs 3). IPC measures how efficiently the CPU is being utilised; cycles
per iteration measures how fast the loop actually runs. For performance, cycles per
iteration is what matters. Adding ILP-exploitable work makes the CPU work harder
but does not always make it finish sooner, because every iteration still ends with
a branch that partially serialises the start of the next.

### 5.2 Why the Dependent Loop Is Faster Per Iteration

The dependent loop has only 3 instructions and two genuinely parallel chains (`lsl`
and `subs`). With 1-cycle latency per instruction and complete overlap, the theoretical
minimum is 1.0 cycle/iteration. The measured 1.15 is near-optimal.

The independent loop adds four arithmetic chains but does not remove the branch
resolution bottleneck. Port pressure increases to 0.7 cycles. The ROB holds more
in-flight work. These factors push cycles/iteration from 1.15 to 1.41 despite the
higher IPC — the marginal work added by extra chains costs more than it saves because
the iteration boundary bottleneck remains fixed.

### 5.3 What the M2's Architecture Reveals

**The M2 is a genuinely wide machine.** Four simultaneous shift operations produce
only ~1 cycle of average port contention per instruction — the execution backend absorbs
them without significant queuing. The out-of-order window is deep enough to find and
dispatch parallel work across multiple iterations simultaneously, which is why
instructions appear to wait 3.5–6.5 cycles in the scheduler rather than executing
immediately: they are not stalled, they are being held while other in-flight iterations
overlap around them.

**In-order retirement is the hidden structural cost.** `subs` executes at 1.0 cycle
wait but waits 5.5 cycles in the ROB. The CPU is fast at executing — it is constrained
by the requirement to retire in program order. Adding more instructions to a loop means
more time in the ROB queue, which sets a floor on the minimum cycles per iteration
regardless of how many execution ports are available.

**Loop-carried dependencies are the true bottleneck.** Even in the independent loop,
the loop counter chain (`subs → b.ne`) forms its own loop-carried dependency that
serialises iteration starts. The theoretical ILP ceiling from four independent chains
is never fully achieved because this structural chain always exists.

### 5.4 Three Compiler Transformations That Matter

1. **Loop elimination (Phase 1):** A deterministic sum was replaced with a single
   constant addition. The binary contained no loop at all.
2. **Countdown transformation:** `i < N` ascending iteration became a `subs`-based
   countdown, merging decrement and flag-set into one instruction and eliminating `cmp`.
3. **Strength reduction:** `x += x` was compiled to `lsl x, x, #1` — multiply-by-2
   expressed as a zero-cost shift.

All three are correct compiler behaviour. The benchmarker's job is to know about them
and design experiments that survive them. Assembly inspection is the verification step
that confirms the workload is what you think it is.

---

## 6. Key Takeaways

**ILP is real and measurable on Apple M2.** The independent loop achieves 4.27 IPC
vs 2.60 for the dependent loop, directly demonstrating that the M2 dispatches multiple
instructions to separate execution units every cycle. The hardware is wide — four
simultaneous 1-cycle-latency instructions encounter only ~1 cycle of average port
contention.

**IPC and cycles per iteration measure different things — use cycles per iteration.**
The independent loop has higher IPC (more CPU utilisation) yet more cycles per
iteration (slower wall-clock progress). In tight loops with fixed branch overhead, the
benefit of ILP is partial, not linear. Cycles per iteration is the correct metric for
comparing loop performance; IPC characterises how efficiently the hardware is being
used, not how fast the work completes.

**Loop-carried dependencies are the dominant bottleneck in tight arithmetic loops.**
Even with four genuinely independent chains, branch resolution at the end of each
iteration serialises the start of the next. The theoretical maximum speedup from
independent arithmetic is never fully achieved because the loop control structure
itself forms a dependency chain that no amount of arithmetic ILP can eliminate.

**A hidden dependency can silently corrupt an experiment.** The `i & 1` design
funnelled all four chains through a single `and` result that depended on the loop
counter. What appeared to be four independent chains was actually one chain with a
fan-out. The llvm-mca data revealed the structural error — the fan-out sat in the
ROB for 15 cycles and `cmp` accumulated 16.5 cycles of port contention, both
clear diagnostic signatures of a convergence bottleneck.

**Assembly inspection is mandatory, not optional.** The compiler eliminated the loop
entirely in Phase 1 and introduced a hidden dependency in Phase 2. Both produced
plausible-looking timing numbers. Only reading the generated assembly revealed that
neither was measuring what was intended. Source code describes intent; assembly is
what executes.

**`llvm-mca` is the best available tool for port-level visibility on Apple Silicon.**
Apple's hardware PMU does not expose per-execution-port counters. `llvm-mca` static
simulation provides scheduler wait times, port contention, and ROB occupancy data
that make dependency chain analysis possible. Install via `brew install llvm` and
invoke as `/opt/homebrew/opt/llvm/bin/llvm-mca`. Use Apple's own clang
(`/usr/bin/clang++`) for compilation to avoid libc++ compatibility issues with
Homebrew LLVM.
