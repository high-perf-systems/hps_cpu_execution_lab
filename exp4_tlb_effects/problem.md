# Experiment 4: TLB Effects and Page Walk Cost

## Problem

Modern CPUs operate on virtual addresses — every pointer in a running program refers
to a virtual memory location, not a physical one. The OS maintains a **page table** that
maps virtual pages to physical frames in DRAM. Before any memory access can reach the
cache hierarchy, the CPU must first translate the virtual address to a physical address.

This translation is performed by the **Memory Management Unit (MMU)**. To avoid
consulting the page table on every single memory access — which would double the cost
of every load and store — the CPU caches recent translations in a small, fast hardware
structure called the **Translation Lookaside Buffer (TLB)**.

The TLB is small by design. The Apple M2 has a TLB that can hold on the order of
hundreds to low thousands of entries. Each entry maps one virtual page (16KB on Apple
Silicon) to its corresponding physical frame. When the CPU accesses an address whose
page is already in the TLB — a **TLB hit** — translation is essentially free. When
the page is not present — a **TLB miss** — the MMU must walk the page table in memory,
a process called a **page table walk**, which costs multiple additional memory accesses
before the original load or store can proceed.

For programs with small working sets, the TLB covers all active pages and misses are
rare. As working set size grows beyond TLB capacity, miss rate increases and page walk
overhead compounds on top of raw memory latency. This is precisely what appeared as
unexplained latency growth in exp3 — the 200MB working set measured 327 cycles vs
128 cycles at 48MB, with TLB pressure responsible for the difference.

## Core Question

> **What is the actual cost of a TLB miss and page table walk on the Apple M2, and
> at what working set size does TLB pressure become the dominant performance bottleneck?**

## The Stride Sweep Technique

Exp3 varied total working set size, but cache pressure and TLB pressure increased
together — the two effects could not be separated. Exp4 decouples them using a
**stride sweep**.

An array large enough to exceed the L2 cache is allocated once. The access pattern
varies the **stride** — the distance in bytes between successive accesses:

- **Small stride (e.g. 64 bytes — one cache line):** many accesses per page, few
  unique pages touched → cache pressure high, TLB pressure low
- **Large stride (e.g. 16KB — one page):** exactly one access per page, maximum
  unique pages touched → cache pressure low, TLB pressure high
- **Larger stride (multiple pages):** same number of unique pages as one-per-page
  but working set spans more virtual address space

By fixing the array size and varying only the stride, the number of unique pages
touched per iteration changes while the total data size stays constant. When measured
latency rises sharply at the stride that causes one access per page, the increment
over baseline is the TLB miss penalty — isolated from cache effects.

## Why This Matters

TLB pressure is one of the least visible performance costs in real systems. A program
with a large heap, many allocations, or a working set spread across many virtual
pages can suffer significant TLB overhead that does not show up in cache miss counters.
Understanding the TLB miss penalty and the stride at which it activates allows a
programmer to make informed decisions about memory layout, huge page usage, and
working set organization — all of which are critical in high-performance systems
where memory access patterns determine throughput.

The numbers measured here will directly explain the latency growth observed at the
tail of exp3's sweep and provide a concrete cost model for virtual memory overhead
on the Apple M2.

## Scope

- Single-threaded stride sweep only
- Fixed large array, varying stride
- Apple M2 Max, performance cores, `-O2 -fno-vectorize`
- Apple Silicon page size: 16KB (measured in exp3)
- TLB size not published by Apple — will be inferred from the inflection point
  in the latency vs stride curve