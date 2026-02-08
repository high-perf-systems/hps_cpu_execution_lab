# Execution Environment and Constraints

This document describes the execution environment and assumptions under which
all experiments in `hps_cpu_execution_lab` are conducted.

The goal of this lab is to study **CPU execution behavior**, not to obtain
cycle-accurate or architecture-neutral benchmarks. Emphasis is placed on
**relative trends and causal reasoning** rather than absolute numbers.

---

## Hardware Platform

### System
- Operating System: macOS
- Kernel: Darwin Kernel Version 24.6.0
- Architecture: arm64 (Apple Silicon)

### CPU
- Processor: Apple M2 Max
- Total cores: 12
  - Performance cores (P-cluster): up to ~3.3 GHz
  - Efficiency cores (E-cluster): up to ~1.5 GHz

Apple Silicon uses a **heterogeneous core architecture**. Performance
characteristics differ significantly between P-cores and E-cores.

Unless explicitly stated otherwise:
- Benchmarks are *intended* to reflect execution on performance cores
- No explicit CPU affinity is enforced
- Some execution may occur on E-cores due to OS scheduling

This is acknowledged as a **source of variability**.

---

## Cache and Memory System

- L1 Instruction Cache: 128 KB (per core)
- L1 Data Cache: 64 KB (per core)
- L2 Cache: 4 MB (shared per cluster)
- Cache policy: non-inclusive
- System Level Cache (SLC): present but not explicitly modeled

Cache effects are not ignored, but in this lab they are considered **secondary**
to execution effects (pipelines, dependencies, speculation).

The focus is on **execution behavior assuming data is hot unless otherwise stated**.

---

## Memory

- Usable RAM: 32 GB

Memory capacity is not a limiting factor in any experiment.

---

## Compiler and Build Configuration

- Compiler: clang 15.0.0
- Target: arm64-apple-darwin
- Language: C++17
- Optimization level: `-O2`

Additional constraints:
- Auto-vectorization disabled where required
- Loop unrolling disabled where required
- Compilation flags are documented per experiment

The intent is to:
- Preserve realistic compiler scheduling
- Avoid hidden parallelism unless explicitly studied

---

## Runtime Constraints

- All benchmarks are **single-threaded**
- No explicit control over:
  - CPU frequency scaling
  - Power management
  - Thermal throttling
- Background system activity is minimized but not eliminated

Measurements should be interpreted statistically and comparatively, not as
absolute truth.

---

## Measurement Philosophy

- Absolute cycle counts are not assumed to be stable
- Trends, ratios, and inflection points are prioritized
- Warm-up runs are used where appropriate
- Experiments are repeated to detect anomalies

---

## Out of Scope

The following are intentionally excluded from this lab:

- Multithreaded execution
- NUMA effects
- Explicit SIMD intrinsics
- GPU or accelerator usage
- Distributed systems
- OS scheduler internals

These topics will be addressed in **separate, dedicated labs**.

---

## Apple Silicon–Specific Notes

Apple Silicon is a highly proprietary architecture. Some microarchitectural
details (pipeline depth, execution port mapping, branch predictor internals)
are not publicly documented.

As a result:
- Some interpretations are **hypothesis-driven**
- Observations may not generalize to x86 CPUs
- Experiments are designed to reveal *behavior*, not implementation details

This limitation is accepted as part of the learning process.
