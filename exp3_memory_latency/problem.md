# Experiment 3: Memory Latency Measurement via Pointer Chasing

## Problem

Performance optimization requires knowing where time is actually spent. For memory-bound
workloads, the dominant cost is not computation but the latency of fetching data from
the memory hierarchy. Knowing the exact cycle cost of an L1 hit, L2 hit, and DRAM access
is a prerequisite for reasoning about whether any given algorithm is compute-bound or
memory-bound — and therefore where optimization effort should be directed.

Apple does not publish memory latency specifications for the M2. This experiment measures
them empirically.

## Core Question

> **What is the actual access latency in CPU cycles for each level of the Apple M2 memory
> hierarchy — L1 cache, L2 cache, and DRAM?**

## Why Standard Benchmarks Fail

Two hardware mechanisms hide memory latency from naive benchmarks:

**Hardware prefetching** — the CPU detects sequential or strided access patterns and
fetches data before it is requested. By the time the load instruction executes, the data
is already in cache. True latency is never observed.

**Out-of-order execution** — the CPU overlaps multiple independent memory requests
simultaneously. Latency is hidden behind other work. The observed time reflects
throughput, not latency.

Both mechanisms must be defeated to measure true latency.

## The Pointer Chasing Technique

An array is filled with a random permutation of indices. Each element holds the index
of the next element to visit:

```
array[0] = 7
array[7] = 3
array[3] = 15
...
```

Traversal:
```cpp
index = array[index];   // each load depends on the previous result
```

Because each load's address is the result of the previous load, the CPU cannot start
the next load until the current one completes. Out-of-order execution cannot overlap
requests — there is a strict serial dependency. The prefetcher cannot predict the next
address — the access pattern is random. Full memory latency is exposed at every step.

## Experimental Approach

The same pointer chase is run on arrays of increasing size, sweeping across the cache
hierarchy boundaries:

| Working Set | Expected Location | Measures |
|---|---|---|
| < 64 KB | L1 data cache | L1 hit latency |
| 64 KB – 4 MB | L2 cache | L2 hit latency |
| > 4 MB | DRAM | DRAM access latency |

For each array size, total traversal time is measured and divided by the number of
steps to obtain average latency per access. Converting to cycles using the M2 performance
core frequency (3.33 GHz) gives the cycle cost at each memory level.

The result should show a clear step function — flat latency within each cache level,
jumping at each overflow boundary.

## Why These Numbers Matter

The latency values measured here are the hardware constants that underpin every
performance model in this lab series. Cache miss cost determines whether an algorithm
is compute-bound or memory-bound. Working set sizing decisions in matrix operations,
inference engines, and real-time control loops all depend on knowing these numbers.
Every future experiment that involves memory will reference the baselines established
here.

## Scope

- Single-threaded pointer chase only
- No SIMD, no prefetch instructions
- Random permutation to defeat hardware prefetcher
- Apple M2 Max, performance cores, `-O2 -fno-vectorize`
- System Level Cache (SLC) behavior is not explicitly modeled — its effects
  will appear as anomalies in the sweep and will be noted where observed