# Experiment 5: False Sharing Across Caches

## Problem

Modern multi-core CPUs rely heavily on caches to bridge the growing speed gap between the processor and main memory. When multiple cores access shared data, cache coherence protocols ensure that all cores see a consistent view of memory. However, these protocols can introduce significant performance penalties due to a phenomenon called **false sharing**.

False sharing occurs when two or more threads, running on different cores, access independent variables that happen to reside on the same cache line. Even though the variables themselves are independent, any write to one of them by one core will cause the entire cache line to be invalidated in the caches of other cores that hold a copy of that cache line. This forces other cores to reload the cache line from a higher level in the memory hierarchy, leading to unnecessary cache misses, increased bus traffic, and reduced performance.

On the Apple M2, the cache line size is 128 bytes. Consider a scenario where two integer counters, `counter_A` and `counter_B`, are allocated consecutively in memory. If they both fall within the same 128-byte cache line, then:

1. Thread A, running on Core 1, increments `counter_A`. This modifies the cache line in Core 1's cache.
2. Thread B, running on Core 2, increments `counter_B`. Since `counter_B` is on the same cache line, Core 2's cached copy of that line is now stale. Core 2 must fetch the updated cache line from Core 1 (or main memory if Core 1 has already written it back).
3. When Thread A next tries to increment `counter_A`, its cached copy of the line may now be stale (if Core 2 modified it), forcing it to refetch.

This continuous invalidation and reloading creates a "ping-pong" effect, significantly degrading performance.

## Core Question

> **What is the performance impact of false sharing on the Apple M2, and how effectively can padding mitigate this effect by ensuring independent variables reside on separate cache lines?**

## Mitigation Strategy: Padding

To mitigate false sharing, the independent variables can be strategically placed in memory such that they reside on different cache lines. This can be achieved by adding padding bytes between them. For instance, if `counter_A` is an integer (4 bytes), adding 124 bytes of padding after it would ensure that `counter_B` (if placed immediately after the padding) starts on a new cache line, preventing false sharing.

This experiment will profile two scenarios:
1. **False Sharing:** `counter_A` and `counter_B` are allocated contiguously, likely resulting in false sharing.
2. **Mitigated False Sharing:** `counter_A` is followed by padding, ensuring `counter_B` is on a separate cache line.

We will measure the execution time for a large number of increments for both scenarios to quantify the performance penalty of false sharing and the effectiveness of padding as a mitigation strategy.

## Scope

- Multi-threaded execution (two threads, each on a distinct core).
- Focus on cache line contention for simple integer counters.
- Apple M2 Max, performance cores.
- Compilation: Enable multi-threading support. Disable aggressive vectorization and loop unrolling to isolate cache effects.
