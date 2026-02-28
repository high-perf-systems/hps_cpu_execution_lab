# Experiment 2: Branch Prediction and Misprediction Cost

## Problem

Modern CPUs do not wait for a branch condition to be resolved before fetching the next
instruction. Instead, they *predict* which way the branch will go and speculatively execute
down that path. If the prediction is correct, speculation is free. If it is wrong, the CPU
must flush the speculative work, refill the pipeline, and restart from the correct path.

This flush is the **misprediction penalty** — a fixed cost paid every time the predictor
guesses wrong. On the Apple M2, this penalty is estimated at 10–16 cycles per misprediction.

The question this experiment investigates is:

> **How much does branch predictability affect performance, and can we isolate and measure
> the misprediction penalty empirically?**

## What We Are Measuring

We compare the execution time of a conditional loop body under two conditions:

- **Predictable branch** — the branch outcome follows a regular pattern the predictor can learn
- **Unpredictable branch** — the branch outcome is random, forcing ~50% misprediction rate

All other factors are held constant: same number of iterations, same arithmetic work per
iteration, same data size, same compiler flags. The only variable is branch predictability.

## Why It Matters

Branch misprediction is behind some of the most counterintuitive performance results in
real systems:

- Sorting data before processing it can make a loop 5–6× faster
- Binary search underperforms linear search on small arrays partly because of this
- Hot paths in database query engines, parsers, and network packet processors are
  explicitly designed around branch predictor behavior

Understanding this cost is essential for writing performance-sensitive code in any domain —
inference engines, robotics control loops, real-time signal processing, or network stacks.

## Scope

This experiment is intentionally limited to:

- Single-threaded execution on Apple M2 performance cores
- Small working sets that fit in L1 cache (no memory hierarchy effects)
- Scalar code only (no SIMD)
- Isolated branch behavior (arithmetic work is minimal and constant)

Memory effects, SIMD, and multithreading are addressed in separate experiments.

## Connection to Experiment 1

In the CPU execution lab's first experiment, the `b.ne` instruction appeared at the end of
every benchmark loop as unavoidable overhead — always the last instruction in the dependency
chain, always correctly predicted since the loop ran a fixed number of iterations. Here, we
make the branch itself the subject of study, varying its predictability while holding
everything else fixed.