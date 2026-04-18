// Experiment 5: False Sharing and Cache Coherence Cost
//
// Three cases, selected at runtime:
//   ./false_sharing 1   single-threaded baseline (both counters, same cache line)
//   ./false_sharing 2   multi-threaded, false sharing (same cache line)
//   ./false_sharing 3   multi-threaded, padded (separate cache lines)
//
// Compilation:
//   clang++ -O2 -std=c++20 -fno-vectorize false_sharing.cpp -o false_sharing
//
// Run with elevated priority to reduce OS scheduler noise and increase the
// probability that both threads land on distinct P-cores:
//   sudo nice -n -20 ./false_sharing <mode>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <algorithm>

// ── LCG recurrence constants ──────────────────────────────────────────────────
//
// Knuth's multiplier (TAOCP vol 2) for a 64-bit linear congruential generator.
// Combined with Marsaglia's additive constant, this produces a full-period
// sequence: every possible uint64_t value appears exactly once per cycle.
//
// Why LCG instead of counter++:
//   A plain increment has no data dependency between iterations.
//   The compiler is free to replace the entire N-iteration loop with a single
//   counter += N, collapsing all iterations into one operation.
//   With LCG, iteration i+1's input is iteration i's output — a strict
//   read-after-write dependency the compiler cannot dissolve. Every iteration
//   must read the previous store's result before computing the next value.
static constexpr uint64_t LCG_MUL = 6364136223846793005ULL;
static constexpr uint64_t LCG_INC = 1442695040888963407ULL;

static constexpr long long N      = 100'000'000LL;
static constexpr long long WARMUP = N / 10;
static constexpr int       RUNS   = 5;
static constexpr double    GHZ    = 3.33;   // M2 Max P-core frequency

// ── Struct layouts ────────────────────────────────────────────────────────────

// Cases 1 & 2: both counters packed into a single 128-byte cache line.
// alignas(128) ensures counter_a starts on a cache-line boundary.
// counter_b follows immediately at offset +8, within the same 128-byte block.
// A store to either counter invalidates the line in all other cores' caches.
// struct alignas(128) Adjacent {
//     std::atomic<uint64_t> a{1};
//     std::atomic<uint64_t> b{1};
// };
struct alignas(128) Adjacent {
    std::atomic<uint64_t> a{1};
    std::atomic<uint64_t> b{1};
};

// Case 3: counters on separate 128-byte cache lines.
// _pad fills the remainder of counter_a's cache line (128 - 8 = 120 bytes).
// counter_b begins at offset +128, on a new 128-byte-aligned cache line.
// A store to counter_b never invalidates the cache line holding counter_a.
// struct alignas(128) Padded {
//     std::atomic<uint64_t> a{1};
//     char                  _pad[120];
//     std::atomic<uint64_t> b{1};
// };

struct alignas(128) Padded {
    std::atomic<uint64_t> a{1};
    char                  _pad[120];
    std::atomic<uint64_t> b{1};
};

// ── Utilities ─────────────────────────────────────────────────────────────────

// Forces the compiler to treat the pointed-to memory as observable output.
// Without this, the LCG chain has no visible side effects and the compiler
// may eliminate all stores as dead code.
static inline void escape(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

static void print_layout(const char* label, const void* pa, const void* pb) {
    uintptr_t ua = reinterpret_cast<uintptr_t>(pa);
    uintptr_t ub = reinterpret_cast<uintptr_t>(pb);
    std::cout << "\n──── " << label << " ────\n";
    std::cout << "  counter_a @ " << pa << "  (cache line " << (ua >> 7) << ")\n";
    std::cout << "  counter_b @ " << pb << "  (cache line " << (ub >> 7) << ")\n";
    std::cout << "  same 128-byte cache line: "
              << ((ua >> 7) == (ub >> 7) ? "YES  ← false sharing active"
                                         : "NO   ← false sharing absent") << "\n";
}

static void print_result(const char* who, double min_ns, double avg_ns) {
    std::cout << "  " << who
              << "  min = " << min_ns << " ns  =  " << (min_ns * GHZ) << " cycles"
              << "   avg = " << avg_ns << " ns  =  " << (avg_ns * GHZ) << " cycles\n";
}

// ── Case 1: single-threaded baseline ─────────────────────────────────────────
//
// One thread updates counter_a. counter_b is warmed up but not written during
// the timed section, so the cache line stays hot in L1d throughout.
// No coherence traffic. Cost = pure arithmetic + L1 load/store latency.
static void run_case1() {
    Adjacent s;
    print_layout("Case 1 — single-threaded baseline", &s.a, &s.b);

    // Touch both counters during warmup to bring their shared cache line into L1d.
    for (long long i = 0; i < WARMUP; i++) {
        // uint64_t v = s.a.load(std::memory_order_relaxed);
        // s.a.store(v * LCG_MUL + LCG_INC, std::memory_order_relaxed);
        // uint64_t u = s.b.load(std::memory_order_relaxed);
        // s.b.store(u * LCG_MUL + LCG_INC, std::memory_order_relaxed);
        uint64_t v = s.a.load(std::memory_order_seq_cst);
        s.a.store(v * LCG_MUL + LCG_INC, std::memory_order_seq_cst);
        uint64_t u = s.b.load(std::memory_order_seq_cst);
        s.b.store(u * LCG_MUL + LCG_INC, std::memory_order_seq_cst);
    }
    escape(&s);

    double min_ns = 1e18, sum_ns = 0.0;
    for (int r = 0; r < RUNS; r++) {
        auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < N; i++) {
            // Load counter_a → compute LCG step → store result back.
            // The load is on the critical path: iteration i+1 cannot begin
            // until iteration i's store is visible, because the next load
            // reads the value just written. This exposes true latency (not
            // throughput) — the same pointer-chasing technique used in exp3.
            // uint64_t v = s.a.load(std::memory_order_relaxed);
            // s.a.store(v * LCG_MUL + LCG_INC, std::memory_order_relaxed);
            uint64_t v = s.a.load(std::memory_order_seq_cst);
            s.a.store(v * LCG_MUL + LCG_INC, std::memory_order_seq_cst);
        }
        auto t1 = std::chrono::steady_clock::now();
        escape(&s);

        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
        min_ns = std::min(min_ns, ns);
        sum_ns += ns;
    }
    print_result("counter_a:", min_ns, sum_ns / RUNS);
}

// ── Cases 2 & 3: multi-threaded ───────────────────────────────────────────────
//
// Thread A updates ctr_a and Thread B updates ctr_b simultaneously.
// std::barrier synchronizes both threads at the start of each timed run,
// ensuring the two timed loops run concurrently (not sequentially).
//
// Case 2 (Adjacent): Thread B's store to counter_b writes to the same cache
// line as counter_a. The coherence protocol invalidates the line in Thread A's
// L1d. Thread A's next load must fetch the line from the SLC (22.5 cycles,
// exp3). The load is on the critical path, so each iteration stalls.
//
// Case 3 (Padded): counter_b is on a separate cache line. Thread B's stores
// never reach Thread A's line. Thread A's line stays in Modified state in its
// own L1d. Both threads run at single-threaded speed simultaneously.
template <typename S>
static void run_multithreaded(const char* label, S& s) {
    print_layout(label, &s.a, &s.b);

    std::barrier<> sync(2);
    double min_a = 1e18, sum_a = 0.0;
    double min_b = 1e18, sum_b = 0.0;

    auto worker = [&](std::atomic<uint64_t>& ctr, double& out_min, double& out_sum) {
        for (long long i = 0; i < WARMUP; i++) {
            uint64_t v = ctr.load(std::memory_order_seq_cst);
            ctr.store(v * LCG_MUL + LCG_INC, std::memory_order_seq_cst);
        }
        escape(&ctr);

        for (int r = 0; r < RUNS; r++) {
            // Both threads must arrive here before either is released.
            // Ensures both timed loops start simultaneously — without this,
            // one thread could finish before the other even starts, and
            // no coherence ping-pong would occur during measurement.
            sync.arrive_and_wait();

            auto t0 = std::chrono::steady_clock::now();
            for (long long i = 0; i < N; i++) {
                uint64_t v = ctr.load(std::memory_order_seq_cst);
                ctr.store(v * LCG_MUL + LCG_INC, std::memory_order_seq_cst);
            }
            auto t1 = std::chrono::steady_clock::now();
            escape(&ctr);

            double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
            out_min = std::min(out_min, ns);
            out_sum += ns;
        }
    };

    std::thread ta([&]() { worker(s.a, min_a, sum_a); });
    std::thread tb([&]() { worker(s.b, min_b, sum_b); });
    ta.join();
    tb.join();

    // Report both threads. In case 2 both should be symmetrically slowed.
    // In case 3 both should match the single-threaded baseline.
    print_result("Thread A (counter_a):", min_a, sum_a / RUNS);
    print_result("Thread B (counter_b):", min_b, sum_b / RUNS);
}

// ── Entry point ───────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <1|2|3>\n"
                  << "  1  single-threaded baseline\n"
                  << "  2  multi-threaded, false sharing\n"
                  << "  3  multi-threaded, padded (no false sharing)\n";
        return 1;
    }

    int mode = std::atoi(argv[1]);
    std::cout << "N = " << N << "  WARMUP = " << WARMUP << "  RUNS = " << RUNS << "\n";

    switch (mode) {
        case 1: {
            run_case1();
            break;
        }
        case 2: {
            Adjacent s;
            run_multithreaded("Case 2 — multi-threaded, false sharing", s);
            break;
        }
        case 3: {
            Padded s;
            run_multithreaded("Case 3 — multi-threaded, padded", s);
            break;
        }
        default:
            std::cerr << "invalid mode: " << mode << "\n";
            return 1;
    }
    return 0;
}
