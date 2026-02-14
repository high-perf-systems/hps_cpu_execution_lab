# Dependent vs. Independent Instruction Benchmark on Apple M2

## 1. Objective

To experimentally observe and understand:
*   The difference between latency-bound and throughput-bound execution.
*   The effect of Instruction-Level Parallelism (ILP).
*   The roles of:
    *   Instruction latency
    *   Execution port width
    *   Issue width
    *   Out-of-order execution

## 2. Initial Experiment – Unexpected Result

### 2.1 Code Structure (Original Attempt)

**Dependent version:**
```c++
for (size_t i = 0; i < N; i++) {
    x = x + 1;
}
```

**Independent version:**
```c++
for (size_t i = 0; i < N; i++) {
    a++;
    b++;
    c++;
    d++;
}
```

### 2.2 Observed Result

Both versions completed in nanoseconds, even for very large `N`. This was surprising because:
*   The independent version has 4× more arithmetic instructions.
*   We expected significantly higher runtime.

## 3. Assembly Inspection – Root Cause

Disassembly showed:
*   The loop had been completely removed.
*   The compiler computed a closed-form algebraic expression.
*   The result was replaced by: `final_value = constant + N * constant`.
*   The CPU never executed the loop at runtime.

## 4. Lesson #1

Modern compilers aggressively:
*   Perform algebraic simplification.
*   Remove loops with predictable behavior.
*   Strength-reduce arithmetic.
*   Collapse deterministic recurrences.

**Microbenchmarks must be carefully constructed to prevent optimization collapse.**

## 5. Fixing the Benchmark

To prevent algebraic elimination, we introduced runtime-dependent computation:
```c++
x += (i & 1);
```
This makes each iteration depend on the loop variable, meaning the compiler can no longer reduce the loop to a simple closed form.

## 6. True Benchmark – Dependent vs. Independent

### 6.1 Dependent Version
```c++
for (size_t i = 0; i < N; i++) {
    x += (i & 1);
}
```
Single dependency chain on `x`.

### 6.2 Independent Version
```c++
for (size_t i = 0; i < N; i++) {
    a += (i & 1);
    b += (i & 1);
    c += (i & 1);
    d += (i & 1);
}
```
Four independent dependency chains.

## 7. Results (N = 100,000,000)

| Version     | Runtime (µs) |
| :---------- | :----------- |
| Dependent   | ~42,950 µs   |
| Independent | ~73,587 µs   |

**Runtime ratio:** Independent / Dependent ≈ 1.71× (Not 4×).

## 8. Interpretation

### 8.1 Naive Expectation

*   Independent version has 4× arithmetic instructions.
*   If the CPU were scalar and in-order: Expected runtime ≈ 4× slower.

### 8.2 Actual Behavior

*   Observed slowdown ≈ 1.7×.
*   This indicates:
    *   The CPU executes multiple independent instructions per cycle.
    *   The machine is superscalar.
    *   Instruction-level parallelism is being exploited.

## 9. Microarchitectural Concepts Learned

### 9.1 Latency

*   **Latency** = time for one instruction result to become available.
*   **Example:**
    *   Add latency ≈ 1 cycle
    *   Multiply latency ≈ 4 cycles (typical)
*   Latency limits dependent chains.

### 9.2 Throughput

*   **Throughput** = number of instructions of a type that can be started per cycle.
*   **Example:**
    *   If CPU has 2 integer add ports: Throughput = 2 adds per cycle (after pipeline fill).
*   Throughput limits independent instruction streams.

### 9.3 Issue Width

*   **Issue width** = maximum number of instructions the CPU can dispatch per cycle.
*   Even if execution units exist, issue width can limit sustained execution.

### 9.4 Execution Ports

*   Modern CPUs have multiple execution ports:
    *   Integer add ports
    *   Multiply ports
    *   Load/store ports
    *   Branch units
*   Throughput for an instruction type is limited by the number of ports that support it.

## 10. Latency vs. Throughput – Key Distinction

**Dependent Chain**
```c++
x = x + 1;
x = x + 1;
x = x + 1;
```
Each instruction depends on the previous result.
Performance limited by: `Total time ≈ N × latency`

**Independent Instructions**
```c++
a++;
b++;
c++;
d++;
```
No inter-dependency.
Performance limited by: `Total time ≈ N × (instructions / execution_width)`
Latency is hidden via pipelining.

## 11. Pipelining Insight

Even if:
*   Multiply latency = 4 cycles
*   Multiply ports = 2

The CPU can still sustain:
*   2 multiplies per cycle (after pipeline warm-up).
This is because execution units are pipelined.
**Latency ≠ Throughput.**

## 12. Why Independent Was Only 1.7× Slower (Not 2×)

Possible factors contributing to the observed 1.7× slowdown instead of 2× (due to 4 independent chains being processed by a superscalar CPU that can theoretically handle 2 instructions per cycle):
*   Loop counter dependency
*   Branch overhead
*   AND instruction (`i & 1`)
*   Issue width limits
*   Scheduler behavior
*   Port contention

The system is multi-resource constrained. Real performance = minimum across multiple bottlenecks.

## 13. General Performance Model

For a kernel:
`Time ≈ max(arithmetic_cycles, loop_control_cycles, memory_cycles)`
Where: `arithmetic_cycles ≈ work / execution_port_width`

*   **Dependent code:** `Time ≈ work × latency`
*   **Independent code:** `Time ≈ work / throughput`

## 14. Key Takeaways

*   Microbenchmarks must be validated using assembly.
*   Compilers aggressively remove predictable loops.
*   Latency dominates dependent chains.
*   Throughput dominates independent instruction streams.
*   Modern CPUs overlap:
    *   Instructions
    *   Dependency chains
    *   Loop iterations
*   Performance modeling requires identifying the limiting resource.

## 15. Conceptual Upgrade Achieved

This experiment built intuition about:
*   Out-of-order execution
*   Superscalar design
*   Pipelined functional units
*   Execution port bottlenecks
*   Latency hiding
*   Instruction-Level Parallelism (ILP)