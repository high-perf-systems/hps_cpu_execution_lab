# Experiment 2: Branch Prediction and Misprediction Cost — Notes

## 1. Experimental Design

This experiment isolates the cost of **branch misprediction** — specifically the pipeline
flush penalty — with memory effects explicitly excluded. Both branches perform identical
arithmetic work and the condition array is accessed sequentially in all cases. The hardware
prefetcher sees the same access pattern regardless of branch outcome. Memory is not a
variable here.

The only variable between cases is the **predictability of the branch outcome**.

| Case | Condition Array | Expected Misprediction Rate |
|---|---|---|
| Predictable | Alternating: T,F,T,F,... | Near zero |
| Unpredictable | Random | ~50% |

**Execution environment:** Apple M2 Max, Apple clang 15.0.0, `-O2 -fno-vectorize`,
single-threaded, working set fits in L1 cache.

---

## 2. Hypotheses

**H1 — Relative performance:** The predictable case will be significantly faster.
The M2 branch predictor recognizes periodic patterns in branch history. A period-2
alternating pattern is trivial to predict — after two iterations the predictor locks
onto it with near-zero misprediction rate. The random case forces ~50% mispredictions,
each costing an estimated 10–16 cycle pipeline flush. Expected slowdown: **2–4×**.

**H2 — Alternating pattern:** The predictor handles it well. The mechanism is pattern
recognition in branch outcome history, not data prefetching. Expected to perform close
to a perfectly predictable branch.

**H3 — Theoretical penalty:** At 50% misprediction rate, N = 100,000,000, 15-cycle penalty:
```
Additional cycles = 15 × 0.50 × 100,000,000 = 750,000,000
Additional time   = 750,000,000 / 3,330,000,000 Hz ≈ 225 ms
```

---

## 3. Results

*To be filled after running benchmarks.*

---

## 4. Assembly Analysis

*To be filled after inspecting generated assembly.*

---

## 5. llvm-mca Analysis

*To be filled after running llvm-mca on the hot loop.*

---

## 6. Interpretation

*To be filled after results and assembly analysis.*

---

## 7. Key Takeaways

*To be filled at conclusion of experiment.*
