# Problem Statement – Experiment 1  
## Instruction Dependencies and CPU Execution Throughput

### Background

Modern CPUs are capable of executing multiple instructions per cycle through
deep pipelines, out-of-order execution, and instruction-level parallelism (ILP).
However, the effective throughput of a program is often limited not by the number
of instructions, but by **data dependencies between them**.

Two programs that perform the same arithmetic operations and access the same
data may exhibit significantly different performance purely due to differences
in how instructions depend on one another.

This experiment investigates how instruction dependency structure influences
execution performance on a single CPU core.

---

### Problem Description

We study the execution behavior of simple arithmetic loops under different
dependency patterns while keeping all other factors constant:

- Same number of operations
- Same data size
- Same memory access pattern
- Same compiler and optimization settings

The only difference between variants is the **instruction dependency graph**.

Specifically, we compare:
1. A loop with a long dependency chain (each operation depends on the previous)
2. A loop with independent operations that can execute in parallel

---

### Objectives

This experiment aims to:

1. Understand how instruction dependencies limit CPU throughput
2. Observe how CPUs exploit instruction-level parallelism when dependencies are removed
3. Distinguish between *latency-bound* and *throughput-bound* execution
4. Build intuition about why “more instructions” does not always mean “slower code”

---

### Hypotheses (Before Experiment)

1. A dependency-heavy loop will execute close to the latency of the longest dependency chain
2. A dependency-free loop will achieve higher throughput due to ILP
3. Modern CPUs will reorder and overlap independent instructions effectively
4. Performance differences will arise even when memory access is not a bottleneck

---

### Scope

This experiment is intentionally limited to:

- Single-threaded execution
- Scalar code only (no SIMD)
- Small working sets that fit in L1 cache
- Arithmetic-focused workloads

The following are explicitly out of scope:
- Memory hierarchy effects
- Cache misses
- Branch prediction behavior
- Multithreading or parallel execution

These topics are explored in later experiments.

---

### Goal

The goal of this experiment is to build a concrete mental model of how CPUs
execute instructions, how dependencies restrict parallelism, and why
performance optimization often requires restructuring computations rather
than reducing instruction count.
