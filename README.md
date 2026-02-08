# hps_cpu_execution_lab

A hands-on experimental lab to understand **CPU execution, performance bottlenecks, and microarchitectural behavior** through carefully designed experiments.

This repository is a continuation of my work on cache behavior and memory hierarchy, and focuses on **how CPUs actually execute code** — not just how code looks at the source level.

The goal is to build *intuition backed by measurement*.

---

## Motivation

Modern CPUs are deeply complex:
- Out-of-order execution
- Speculation and branch prediction
- Multiple execution ports
- Cache hierarchies and prefetchers
- Compiler optimizations that reshape code

Textbooks explain these concepts, but **true understanding comes from experiments**.

This lab follows a **do → observe → reason → study → repeat** approach:
1. Write minimal, controlled code
2. Measure behavior
3. Form hypotheses
4. Validate using architecture knowledge and tools
5. Refine mental models

---

## Scope of This Lab

This repository focuses on **single-node CPU performance**, especially:

- Instruction execution and pipelines
- Instruction-level parallelism (ILP)
- Branch prediction and misprediction costs
- Loop structure and dependency chains
- Compiler transformations and their impact
- Interaction between execution units and memory

> Distributed systems, GPUs, and clusters are intentionally **out of scope here** and will be covered in separate labs. 

This lab is about **understanding why performance behaves the way it does**.

---

## Experiment Philosophy

Each experiment follows these principles:

- One idea at a time
- Minimal code
- Explicit assumptions
- Controlled compilation flags
- Reproducible measurements
- Clear interpretation

