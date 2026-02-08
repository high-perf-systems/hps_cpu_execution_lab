# Design Decisions

This document captures the key **intentional design decisions** made while
building `hps_cpu_execution_lab`.

The purpose is to clarify *what this project is optimizing for* and *what it
explicitly avoids*, so results can be interpreted correctly.

---

## Project Objective

The objective of this project is to study **CPU execution behavior on a single
core**, focusing on:

- Instruction-level parallelism (ILP)
- Dependency chains
- Pipeline utilization
- Branch behavior
- Compiler scheduling effects

This lab is **not** about memory hierarchy optimization, which is covered in
`hps_cache_behaviour_lab`.

---

## Algorithmic Complexity Is Not the Focus

We deliberately avoid:
- Algorithmic optimizations
- Asymptotic complexity improvements
- Data structure redesigns for efficiency

Instead:
- The *same computation* is retained
- Only the **instruction structure** is modified

This ensures performance differences arise from **execution behavior**, not
from doing less work.

---

## Single-Core, Scalar Execution Only

This project intentionally restricts execution to:

- Single-threaded code
- Scalar execution
- No explicit SIMD usage
- No compiler auto-vectorization

Rationale:
- To expose instruction dependencies clearly
- To reason about pipelines without parallelism masking effects
- To build intuition before introducing SIMD or multithreading

---

## Compiler Optimization Philosophy

- `-O2` is used to enable realistic instruction scheduling
- Aggressive transformations (vectorization, unrolling) are disabled where needed

The goal is:
- Let the compiler optimize *within reason*
- Avoid transformations that obscure causal relationships

---

## Cache Effects Are Minimized, Not Eliminated

Cache behavior is not ignored, but:
- Experiments are designed to keep data hot
- Working sets are intentionally small where possible

This allows execution effects (latency, throughput, dependencies) to dominate.

---

## Platform Choice: Apple Silicon

Apple Silicon is chosen intentionally despite limited public documentation.

Rationale:
- Modern, high-performance out-of-order cores
- Strong speculative execution
- Forces hypothesis-driven reasoning instead of relying on vendor manuals

This lab prioritizes **observed behavior over documented microarchitecture**.

---

## Out-of-Scope by Design

The following are excluded intentionally:
- Multithreading
- NUMA effects
- GPU or accelerator usage
- Distributed systems
- OS scheduler internals

Each of these deserves its own focused study and will be addressed separately.
