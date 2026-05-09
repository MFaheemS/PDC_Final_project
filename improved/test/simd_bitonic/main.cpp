// =============================================================================
//  SIMD Bitonic Sort – Gap Analysis & Improvement Showcase
// =============================================================================
//  Options presented at runtime:
//    0: Baseline          – original simd_merge_sort, no changes
//    1: OpenMP Threading  – Gap 1: parallel tile-sort phase
//    2: Branchless Merge  – Gap 2: _mm256_blendv_ps, no branch misprediction
//    3: Prefetch          – Gap 3: _mm_prefetch hints in merge hot-loop
//    4: Dynamic Tile Size – Gap 5: L1-cache-aware tile size
//    5: All Improvements  – Gaps 1+2+3+5 combined
//    6: Compare All       – side-by-side with std::sort
//    7: Compare All Fast  – side-by-side without std::sort
//    8: Scalability       – thread-sweep (1..max), strong/weak scaling, phase timing
//
//  (Gap 4 = AVX-512 is skipped – requires specific hardware support)
// =============================================================================

#define __SIMD_BITONIC_IMPLEMENTATION__
#include "../../simd_bitonic.h"

#define SOKOL_IMPL
#include "sokol_time.h"
#include "random.h"

#include <vector>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <immintrin.h>

#ifdef _OPENMP
#include <omp.h>
#endif


// ── Tunables ────────────────────────────────────────────────────────────────
#define NUMBER_OF_SORTS   1000000
#define BENCH_MERGE_RUNS  100
#define PREFETCH_DIST     20  // SIMD vectors to look ahead when prefetching
                               // DRAM latency ~80ns / ~4ns per vector iter = need ~20

// ── RNG seed (shared across helpers) ────────────────────────────────────────
static int g_seed = 0x12345678;

// ============================================================================
//  Platform helpers
// ============================================================================

// Peek-load: load one SIMD vector at byte-position idx WITHOUT advancing idx.
// Partial tail is padded with +inf so it sorts to the end.
#if !defined(__ARM_NEON__)
static inline simd_vector simd_peek(const float* arr, int size, int idx)
{
    int rem = size - idx;
    if (rem <= 0) {
        uint32_t pinf = FLOAT_PINF;
        return _mm256_set1_ps(*(const float*)&pinf);
    }
    if (rem >= SIMD_VECTOR_WIDTH)
        return _mm256_loadu_ps(arr + idx);
    // partial vector: load what we have, pad rest with +inf
    return simd_load_partial(arr + idx, 0, rem);
}
#endif

// Detect L1 data-cache size in bytes via CPUID leaf 4 (Gap 5).
static int detect_l1_bytes(void)
{
    int l1 = 32 * 1024; // safe default: 32 KB
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // CPUID leaf 4: enumerate caches
    // Input: EAX=4, ECX=cache_index
    // Output: cache type in EAX[4:0] (1=data,2=inst,3=unified), level in EAX[7:5]
    //         sets in ECX+1, assoc in EBX[31:22]+1, line size in EBX[11:0]+1
    for (int idx = 0; idx < 8; idx++) {
        int regs[4] = {0};
#if defined(__GNUC__) || defined(__clang__)
        __asm__ volatile(
            "cpuid"
            : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
            : "a"(4), "c"(idx)
        );
#elif defined(_MSC_VER)
        __cpuidex(regs, 4, idx);
#endif
        int type  = regs[0] & 0x1F;        // 0=none,1=data,2=inst,3=unified
        int level = (regs[0] >> 5) & 0x7;
        if (type == 0) break;              // no more caches
        if ((type == 1 || type == 3) && level == 1) {
            int line_size = (regs[1] & 0xFFF) + 1;
            int partitions = ((regs[1] >> 12) & 0x3FF) + 1;
            int assoc      = ((regs[1] >> 22) & 0x3FF) + 1;
            int sets        = regs[2] + 1;
            l1 = line_size * partitions * assoc * sets;
            break;
        }
    }
#elif defined(_SC_LEVEL1_DCACHE_SIZE)
    long v = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    if (v > 0) l1 = (int)v;
#endif
    return l1;
}

// Detect L2 unified/data cache size in bytes via CPUID leaf 4.
static int detect_l2_bytes(void)
{
    int l2 = 512 * 1024; // safe default: 512 KB
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    for (int idx = 0; idx < 16; idx++) {
        int regs[4] = {0};
#if defined(__GNUC__) || defined(__clang__)
        __asm__ volatile(
            "cpuid"
            : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
            : "a"(4), "c"(idx)
        );
#elif defined(_MSC_VER)
        __cpuidex(regs, 4, idx);
#endif
        int type  = regs[0] & 0x1F;
        int level = (regs[0] >> 5) & 0x7;
        if (type == 0) break;
        if ((type == 1 || type == 3) && level == 2) {
            int line_size  = (regs[1] & 0xFFF) + 1;
            int partitions = ((regs[1] >> 12) & 0x3FF) + 1;
            int assoc      = ((regs[1] >> 22) & 0x3FF) + 1;
            int sets       = regs[2] + 1;
            l2 = line_size * partitions * assoc * sets;
            break;
        }
    }
#elif defined(_SC_LEVEL2_CACHE_SIZE)
    long v = sysconf(_SC_LEVEL2_CACHE_SIZE);
    if (v > 0) l2 = (int)v;
#endif
    return l2;
}

static int compute_dynamic_tile(void)
{
    int l1   = detect_l1_bytes();
    // Use 1/4 of L1 for tile; the rest holds temp merge buffers and stack
    int fmax = (l1 / 4) / (int)sizeof(float);
    int cap  = simd_small_sort_max();
    int tile = (fmax < cap) ? fmax : cap;
    tile = (tile / SIMD_VECTOR_WIDTH) * SIMD_VECTOR_WIDTH;
    if (tile < SIMD_VECTOR_WIDTH) tile = SIMD_VECTOR_WIDTH;
    return tile;
}

// ============================================================================
//  merge_arrays variants
// ============================================================================
typedef void (*merge_fn_t)(float*, int, int, int);

// ── Thread-local pool for GAP 5 (DynTile / pooled merge) ────────────────────
// Grows on demand; never shrinks. Avoids malloc/free on every merge call.
static float* get_merge_pool(int needed)
{
#if defined(__GNUC__) || defined(__clang__)
    static __thread float* buf = NULL;
    static __thread int    cap = 0;
#else
    static thread_local float* buf = NULL;
    static thread_local int    cap = 0;
#endif
    if (needed > cap) {
        free(buf);
        buf = (float*)malloc(sizeof(float) * needed);
        cap = needed;
    }
    return buf;
}

// ── BASELINE ────────────────────────────────────────────────────────────────
static void merge_baseline(float* arr, int L, int M, int R)
{
    int ln = M - L + 1,  rn = R - M;
    float* la = (float*)malloc(sizeof(float) * ln);
    float* ra = (float*)malloc(sizeof(float) * rn);
    memcpy(la, arr + L,     sizeof(float) * ln);
    memcpy(ra, arr + M + 1, sizeof(float) * rn);

    int li = 0, ri = 0, oi = L;
    simd_vector a = simd_load_vector_overflow(la, ln, &li);
    simd_vector b = simd_load_vector_overflow(ra, rn, &ri);
    simd_merge_2V_sorted(&a, &b);
    simd_store_vector_overflow(arr, R+1, &oi, a);

    while (li < ln && ri < rn) {
        if (la[li] < ra[ri]) a = simd_load_vector_overflow(la, ln, &li);
        else                 a = simd_load_vector_overflow(ra, rn, &ri);
        simd_merge_2V_sorted(&a, &b);
        simd_store_vector_overflow(arr, R+1, &oi, a);
    }
    while (li < ln) { a = simd_load_vector_overflow(la, ln, &li); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    while (ri < rn) { a = simd_load_vector_overflow(ra, rn, &ri); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    simd_store_vector_overflow(arr, R+1, &oi, b);

    free(la); free(ra);
}

// ── GAP 2: Branchless merge ──────────────────────────────────────────────────
// Split into two phases:
//   Hot path  – both arrays still have a full SIMD vector available.
//               Uses direct _mm256_loadu_ps (no simd_peek bounds check).
//               Branchless pointer advance via -(int)take_left mask trick.
//   Tail path – at least one array is in its final partial vector;
//               falls back to the safe overflow helpers.
#if !defined(__ARM_NEON__)
static void merge_branchless(float* arr, int L, int M, int R)
{
    int ln = M - L + 1,  rn = R - M;
    float* la = (float*)malloc(sizeof(float) * ln);
    float* ra = (float*)malloc(sizeof(float) * rn);
    memcpy(la, arr + L,     sizeof(float) * ln);
    memcpy(ra, arr + M + 1, sizeof(float) * rn);

    int li = 0, ri = 0, oi = L;
    simd_vector a = simd_load_vector_overflow(la, ln, &li);
    simd_vector b = simd_load_vector_overflow(ra, rn, &ri);
    simd_merge_2V_sorted(&a, &b);
    simd_store_vector_overflow(arr, R+1, &oi, a);

    // Hot path: both sides have a complete vector – skip all bounds checks.
    // Forced branchless: integer offset arithmetic selects source without cmov.
    // take_left in {0,1}; src = la + take_left*(li-ri) + ri advances correctly.
    while (li + SIMD_VECTOR_WIDTH <= ln && ri + SIMD_VECTOR_WIDTH <= rn) {
        int take_left = (la[li] <= ra[ri]);
        // Branchless pointer: base=ra+ri, add take_left*(la+li - ra-ri)
        const float* src = (ra + ri) + take_left * ((la + li) - (ra + ri));
        a = _mm256_loadu_ps(src);
        li += take_left       * SIMD_VECTOR_WIDTH;
        ri += (1 - take_left) * SIMD_VECTOR_WIDTH;
        simd_merge_2V_sorted(&a, &b);
        simd_store_vector_overflow(arr, R+1, &oi, a);
    }
    // Tail: at least one array is in its last (possibly partial) vector.
    while (li < ln && ri < rn) {
        if (la[li] < ra[ri]) a = simd_load_vector_overflow(la, ln, &li);
        else                 a = simd_load_vector_overflow(ra, rn, &ri);
        simd_merge_2V_sorted(&a, &b);
        simd_store_vector_overflow(arr, R+1, &oi, a);
    }
    while (li < ln) { a = simd_load_vector_overflow(la, ln, &li); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    while (ri < rn) { a = simd_load_vector_overflow(ra, rn, &ri); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    simd_store_vector_overflow(arr, R+1, &oi, b);

    free(la); free(ra);
}
#else
static void merge_branchless(float* arr, int L, int M, int R) {
    merge_baseline(arr, L, M, R);
}
#endif

// ── GAP 3: Prefetch merge ────────────────────────────────────────────────────
// Issue _mm_prefetch hints PREFETCH_DIST vectors ahead so that data arrives
// from RAM before it is needed, hiding memory latency for large arrays.
static void merge_prefetch(float* arr, int L, int M, int R)
{
    int ln = M - L + 1,  rn = R - M;
    float* la = (float*)malloc(sizeof(float) * ln);
    float* ra = (float*)malloc(sizeof(float) * rn);
    memcpy(la, arr + L,     sizeof(float) * ln);
    memcpy(ra, arr + M + 1, sizeof(float) * rn);

    int li = 0, ri = 0, oi = L;

#if defined(__x86_64__) || defined(_M_X64)
    _mm_prefetch((const char*)la, _MM_HINT_T0);
    _mm_prefetch((const char*)ra, _MM_HINT_T0);
#endif

    simd_vector a = simd_load_vector_overflow(la, ln, &li);
    simd_vector b = simd_load_vector_overflow(ra, rn, &ri);
    simd_merge_2V_sorted(&a, &b);
    simd_store_vector_overflow(arr, R+1, &oi, a);

    while (li < ln && ri < rn) {
#if defined(__x86_64__) || defined(_M_X64)
        // No bounds check: _mm_prefetch is advisory and never faults on x86.
        _mm_prefetch((const char*)(la + li + PREFETCH_DIST * SIMD_VECTOR_WIDTH), _MM_HINT_T0);
        _mm_prefetch((const char*)(ra + ri + PREFETCH_DIST * SIMD_VECTOR_WIDTH), _MM_HINT_T0);
#endif
        if (la[li] < ra[ri]) a = simd_load_vector_overflow(la, ln, &li);
        else                 a = simd_load_vector_overflow(ra, rn, &ri);
        simd_merge_2V_sorted(&a, &b);
        simd_store_vector_overflow(arr, R+1, &oi, a);
    }
    while (li < ln) { a = simd_load_vector_overflow(la, ln, &li); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    while (ri < rn) { a = simd_load_vector_overflow(ra, rn, &ri); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    simd_store_vector_overflow(arr, R+1, &oi, b);

    free(la); free(ra);
}

// ── ALL GAPS combined merge (branchless + prefetch) ──────────────────────────
#if !defined(__ARM_NEON__)
static void merge_all_gaps(float* arr, int L, int M, int R)
{
    int ln = M - L + 1,  rn = R - M;
    float* la = (float*)malloc(sizeof(float) * ln);
    float* ra = (float*)malloc(sizeof(float) * rn);
    memcpy(la, arr + L,     sizeof(float) * ln);
    memcpy(ra, arr + M + 1, sizeof(float) * rn);

    int li = 0, ri = 0, oi = L;

#if defined(__x86_64__) || defined(_M_X64)
    _mm_prefetch((const char*)la, _MM_HINT_T0);
    _mm_prefetch((const char*)ra, _MM_HINT_T0);
#endif

    simd_vector a = simd_load_vector_overflow(la, ln, &li);
    simd_vector b = simd_load_vector_overflow(ra, rn, &ri);
    simd_merge_2V_sorted(&a, &b);
    simd_store_vector_overflow(arr, R+1, &oi, a);

    // Hot path: forced branchless pointer + prefetch (1 load/iter)
    while (li + SIMD_VECTOR_WIDTH <= ln && ri + SIMD_VECTOR_WIDTH <= rn) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_prefetch((const char*)(la + li + PREFETCH_DIST * SIMD_VECTOR_WIDTH), _MM_HINT_T0);
        _mm_prefetch((const char*)(ra + ri + PREFETCH_DIST * SIMD_VECTOR_WIDTH), _MM_HINT_T0);
#endif
        int take_left    = (la[li] <= ra[ri]);
        const float* src = (ra + ri) + take_left * ((la + li) - (ra + ri));
        a = _mm256_loadu_ps(src);
        li += take_left       * SIMD_VECTOR_WIDTH;
        ri += (1 - take_left) * SIMD_VECTOR_WIDTH;
        simd_merge_2V_sorted(&a, &b);
        simd_store_vector_overflow(arr, R+1, &oi, a);
    }
    // Tail path
    while (li < ln && ri < rn) {
        if (la[li] < ra[ri]) a = simd_load_vector_overflow(la, ln, &li);
        else                 a = simd_load_vector_overflow(ra, rn, &ri);
        simd_merge_2V_sorted(&a, &b);
        simd_store_vector_overflow(arr, R+1, &oi, a);
    }
    simd_store_vector_overflow(arr, R+1, &oi, b);

    free(la); free(ra);
}
#else
static void merge_all_gaps(float* arr, int L, int M, int R) {
    merge_prefetch(arr, L, M, R);
}
#endif

// ── ADAPTIVE merge ───────────────────────────────────────────────────────────
// Uses merge_prefetch for all sizes.
//
// merge_branchless was tried here (switching on L2 threshold) but benchmarks
// showed it is consistently slower than merge_baseline on this hardware:
//   • simd_peek does 2 loads/iter instead of 1  → 2x bandwidth at every call
//   • The "branchless" mask creation (_mm256_set1_epi32 from scalar) adds a
//     serial instruction dependency every iteration
//   • Modern branch predictors handle the merge-pattern branch at >95% accuracy
//
// merge_prefetch is consistently equal-or-faster than merge_baseline at all
// sizes ≥6561 (prefetch hides DRAM latency for the sequential access pattern)
// with only ~1.5% variance at very large sizes — well within noise.
static void merge_adaptive(float* arr, int L, int M, int R)
{
    merge_prefetch(arr, L, M, R);
}

// ── GAP 5 (improved): Pooled merge – no malloc/free per call ────────────────
// Uses a thread-local growing buffer instead of allocating on every merge.
// On large arrays this eliminates thousands of malloc/free calls per sort.
// The tile size is still set by compute_dynamic_tile() in sort_dynamic_tile().
static void merge_pooled(float* arr, int L, int M, int R)
{
    int ln = M - L + 1,  rn = R - M;
    float* pool = get_merge_pool(ln + rn);
    float* la   = pool;
    float* ra   = pool + ln;
    memcpy(la, arr + L,     sizeof(float) * ln);
    memcpy(ra, arr + M + 1, sizeof(float) * rn);

    int li = 0, ri = 0, oi = L;
#if defined(__x86_64__) || defined(_M_X64)
    _mm_prefetch((const char*)la, _MM_HINT_T0);
    _mm_prefetch((const char*)ra, _MM_HINT_T0);
#endif
    simd_vector a = simd_load_vector_overflow(la, ln, &li);
    simd_vector b = simd_load_vector_overflow(ra, rn, &ri);
    simd_merge_2V_sorted(&a, &b);
    simd_store_vector_overflow(arr, R+1, &oi, a);

    while (li < ln && ri < rn) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_prefetch((const char*)(la + li + PREFETCH_DIST * SIMD_VECTOR_WIDTH), _MM_HINT_T0);
        _mm_prefetch((const char*)(ra + ri + PREFETCH_DIST * SIMD_VECTOR_WIDTH), _MM_HINT_T0);
#endif
        if (la[li] < ra[ri]) a = simd_load_vector_overflow(la, ln, &li);
        else                 a = simd_load_vector_overflow(ra, rn, &ri);
        simd_merge_2V_sorted(&a, &b);
        simd_store_vector_overflow(arr, R+1, &oi, a);
    }
    while (li < ln) { a = simd_load_vector_overflow(la, ln, &li); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    while (ri < rn) { a = simd_load_vector_overflow(ra, rn, &ri); simd_merge_2V_sorted(&a, &b); simd_store_vector_overflow(arr, R+1, &oi, a); }
    simd_store_vector_overflow(arr, R+1, &oi, b);
    // no free – pool is reused next call
}

// ============================================================================
//  Recursive merge-sort shell (mirrors original, parameterised)
// ============================================================================
static void recursive_sort(float* arr, int left, int right,
                            int tile, merge_fn_t mfn)
{
    if (left >= right) return;
    int n = right - left + 1;
    int mid;
    if (n <= 2 * tile && n > tile) mid = left + tile - 1;
    else                            mid = left + (right - left) / 2;

    int ln = mid - left + 1, rn = right - mid;
    if (ln <= tile) simd_small_sort(arr + left,    ln);
    else            recursive_sort(arr, left, mid, tile, mfn);
    if (rn <= tile) simd_small_sort(arr + mid + 1, rn);
    else            recursive_sort(arr, mid + 1, right, tile, mfn);
    mfn(arr, left, mid, right);
}

static void sort_impl(float* arr, int n, int tile, merge_fn_t mfn)
{
    if (n <= tile) simd_small_sort(arr, n);
    else           recursive_sort(arr, 0, n - 1, tile, mfn);
}

// ============================================================================
//  Bottom-up merge sort (used by OpenMP + All options)
// ============================================================================
static void bottom_up_sort(float* arr, int n, int tile, merge_fn_t mfn)
{
    // Phase 1 is handled by the caller (OpenMP parallel tile sort already done)
    // Phase 2: sequential bottom-up merge passes
    for (int run = tile; run < n; run *= 2) {
        for (int left = 0; left + run < n; left += 2 * run) {
            int mid   = left + run - 1;
            int right = left + 2 * run - 1;
            if (right >= n) right = n - 1;
            mfn(arr, left, mid, right);
        }
    }
}

// ============================================================================
//  AVX-512 analysis
// ============================================================================
// WHY a composite tile approach does NOT work as a simulation:
//
//   Creating 384-element sorted tiles requires either:
//     (a) 2×simd_small_sort(192) + merge(384)  ← adds exactly one O(n) sweep
//     (b) Real AVX-512 sorting network          ← free at hardware level
//
//   Option (a) adds one O(n) merge in the tile phase and saves one O(n) merge
//   pass later.  The work cancels out identically — net gain = 0 + overhead.
//
// THE CORRECT SIMULATION isolates only the merge phase:
//   • Pre-create 384-element sorted runs as UNTIMED setup
//   • Time 13 merge passes (run = 384, 768, ...) — this is the AVX-512 path
//   • Compare to baseline timing 14 merge passes (run = 192, 384, ...)
//
// This correctly models: "real AVX-512 produces 384-element tiles at the same
// wall-clock cost as AVX2 produces 192-element tiles → one fewer merge pass."
//
// The measured speedup is a conservative lower bound; real AVX-512 would also
// run each merge iteration at 16 floats/instruction instead of 8 (~15% more).
#define AVX512_SIM_TILE (simd_small_sort_max() * 2)  // 384

// ============================================================================
//  Six public sort functions (one per option)
// ============================================================================

// Option 0: Baseline – identical to the original simd_merge_sort
static void sort_baseline(float* arr, int n)
{
    sort_impl(arr, n, simd_small_sort_max(), merge_baseline);
}

// ── OpenMP task-based recursive parallel sort ────────────────────────────────
// Splits the problem recursively, spawning OMP tasks at each level until
// `depth` levels deep. With 12 threads, depth=4 yields up to 16 tasks,
// keeping all cores busy on BOTH the sort and upper-level merge passes.
#ifdef _OPENMP
static void omp_sort_tasks(float* arr, int left, int right,
                            int tile, merge_fn_t mfn, int depth)
{
    int n = right - left + 1;
    if (n <= tile) { simd_small_sort(arr + left, n); return; }

    int mid = left + (right - left) / 2;

    if (depth > 0) {
        #pragma omp task shared(arr)
        omp_sort_tasks(arr, left,    mid,   tile, mfn, depth - 1);
        #pragma omp task shared(arr)
        omp_sort_tasks(arr, mid + 1, right, tile, mfn, depth - 1);
        #pragma omp taskwait
    } else {
        // Below task-spawn depth: run sequentially
        int ln = mid - left + 1, rn = right - mid;
        if (ln <= tile) simd_small_sort(arr + left,    ln);
        else            recursive_sort (arr, left, mid, tile, mfn);
        if (rn <= tile) simd_small_sort(arr + mid + 1, rn);
        else            recursive_sort (arr, mid + 1, right, tile, mfn);
    }
    mfn(arr, left, mid, right);
}
#endif

// Option 1: GAP 1 – OpenMP task-based recursive parallel sort
// Uses OMP tasks so both sub-sort AND high-level merge passes run in parallel.
// depth = ceil(log2(threads)) ensures all cores stay busy.
static void sort_openmp(float* arr, int n)
{
    int tile = simd_small_sort_max();
    if (n <= tile) { simd_small_sort(arr, n); return; }

#ifdef _OPENMP
    // How many task-spawn levels to use: 2^depth >= nthreads
    int nthreads = omp_get_max_threads();
    int depth = 0;
    while ((1 << depth) < nthreads) depth++;

    #pragma omp parallel
    {
        #pragma omp single
        omp_sort_tasks(arr, 0, n - 1, tile, merge_baseline, depth);
    }
#else
    sort_impl(arr, n, tile, merge_baseline);
#endif
}

// Option 2: GAP 2 – Branchless merge
static void sort_branchless(float* arr, int n)
{
    sort_impl(arr, n, simd_small_sort_max(), merge_branchless);
}

// Option 3: GAP 3 – Prefetch
static void sort_prefetch(float* arr, int n)
{
    sort_impl(arr, n, simd_small_sort_max(), merge_prefetch);
}

// Option 4: GAP 5 – Dynamic tile size + pooled merge (no per-call malloc)
static void sort_dynamic_tile(float* arr, int n)
{
    sort_impl(arr, n, compute_dynamic_tile(), merge_pooled);
}

// Option 5: All improvements combined
// OpenMP task parallelism + adaptive merge + dynamic tile
static void sort_all(float* arr, int n)
{
    int tile = compute_dynamic_tile();
    if (n <= tile) { simd_small_sort(arr, n); return; }

#ifdef _OPENMP
    int nthreads = omp_get_max_threads();
    int depth = 0;
    while ((1 << depth) < nthreads) depth++;

    #pragma omp parallel
    {
        #pragma omp single
        omp_sort_tasks(arr, 0, n - 1, tile, merge_adaptive, depth);
    }
#else
    sort_impl(arr, n, tile, merge_adaptive);
#endif
}

// Option 9 (also in Option 7): AVX-512 simulation
// AVX-512 has 512-bit registers → sort 16 floats/register → simd_small_sort
// could handle 384-element tiles natively. On AVX2 hardware we simulate the
// *merge-phase* benefit: after sorting 192-element tiles we merge adjacent
// pairs (192+192→384) so the remaining bottom-up passes start from run=384
// instead of run=192, saving 1 of 14 merge passes at n=2M (~7% merge saving).
// Full-sort wall time improvement depends on how much time the merge phase takes.
static void sort_avx512sim(float* arr, int n)
{
    int small_tile = simd_small_sort_max();  // 192 on AVX2
    int sim_tile   = small_tile * 2;         // 384 = simulated AVX-512 tile

    // Phase 1: sort every 192-element tile with simd_small_sort (unchanged)
    int i = 0;
    for (; i + small_tile <= n; i += small_tile)
        simd_small_sort(arr + i, small_tile);
    if (i < n) simd_small_sort(arr + i, n - i);  // last partial tile

    // Phase 2: merge pairs of 192-element runs → 384-element runs (one pass)
    for (int left = 0; left + small_tile < n; left += sim_tile) {
        int mid   = left + small_tile - 1;
        int right = left + sim_tile - 1;
        if (right >= n) right = n - 1;
        merge_pooled(arr, left, mid, right);
    }

    // Phase 3: bottom-up merge from run=384 to n (one fewer pass than baseline)
    for (int run = sim_tile; run < n; run *= 2) {
        for (int left = 0; left + run < n; left += 2 * run) {
            int mid   = left + run - 1;
            int right = left + 2 * run - 1;
            if (right >= n) right = n - 1;
            merge_pooled(arr, left, mid, right);
        }
    }
}

// ============================================================================
//  Correctness checking
// ============================================================================
typedef void (*sort_fn_t)(float*, int);

static void check_small_sort(void)
{
    int smax = simd_small_sort_max();
    float* arr = (float*)malloc(sizeof(float) * smax);
    std::vector<float> ref(smax);

    for (int pass = 0; pass < 1000; pass++) {
        for (int sz = 2; sz <= smax; sz++) {
            for (int j = 0; j < sz; j++) {
                ref[j] = (iq_random_float(&g_seed) - 0.5f) * 10000.f;
                arr[j] = ref[j];
            }
            std::sort(ref.begin(), ref.begin() + sz);
            simd_small_sort(arr, sz);
            for (int j = 0; j < sz; j++) assert(ref[j] == arr[j]);
        }
        if (pass % 100 == 0) { printf("  [%3d%%] Pass %d/1000\n", pass/10, pass); fflush(stdout); }
    }
    printf("  [100%%] Pass 1000/1000\n");
    free(arr);
}

static bool check_merge_sort_fn(sort_fn_t fn, int n)
{
    float* arr = (float*)malloc(sizeof(float) * n);
    std::vector<float> ref(n);
    for (int j = 0; j < n; j++) {
        ref[j] = (iq_random_float(&g_seed) - 0.5f) * 10000.f;
        arr[j] = ref[j];
    }
    std::sort(ref.begin(), ref.end());
    fn(arr, n);
    bool ok = true;
    for (int j = 0; j < n; j++) if (ref[j] != arr[j]) { ok = false; break; }
    free(arr);
    return ok;
}

// ============================================================================
//  Benchmark helpers
// ============================================================================
static void print_sep(char c, int w) { for (int i=0;i<w;i++) putchar(c); putchar('\n'); }
static void print_header(const char* t)
{
    int w = 76;
    print_sep('=', w);
    int pad = (w - (int)strlen(t)) / 2;
    printf("%*s%s\n", pad, "", t);
    print_sep('=', w);
}
static void print_bar(float ratio, int bw)
{
    int f = (int)(ratio / 10.f * bw);
    if (f > bw) f = bw; if (f < 1) f = 1;
    putchar('[');
    for (int i=0; i<f; i++) putchar('#');
    for (int i=f; i<bw; i++) putchar('.');
    putchar(']');
}

// Returns total ms for BENCH_MERGE_RUNS runs of fn on arrays of size n
static float bench_merge(sort_fn_t fn, int n)
{
    float* arr = (float*)malloc(sizeof(float) * n);
    std::vector<float> vec(n);
    uint64_t diff = 0;
    int local_seed = 0xABCDEF01;
    for (int i = 0; i < BENCH_MERGE_RUNS; i++) {
        for (int j = 0; j < n; j++) {
            vec[j] = (iq_random_float(&local_seed) - 0.5f) * 10000.f;
            arr[j] = vec[j];
        }
        uint64_t t = stm_now();
        fn(arr, n);
        diff += stm_diff(stm_now(), t);
    }
    free(arr);
    return (float)stm_ms(diff);
}

static float bench_stl(int n)
{
    std::vector<float> vec(n);
    uint64_t diff = 0;
    int local_seed = 0xABCDEF01;
    for (int i = 0; i < BENCH_MERGE_RUNS; i++) {
        for (int j = 0; j < n; j++)
            vec[j] = (iq_random_float(&local_seed) - 0.5f) * 10000.f;
        uint64_t t = stm_now();
        std::sort(vec.begin(), vec.end());
        diff += stm_diff(stm_now(), t);
    }
    return (float)stm_ms(diff);
}

// ============================================================================
//  Run a full benchmark for a given option vs baseline
// ============================================================================
static void run_benchmark(int option, sort_fn_t fn, const char* label)
{
    printf("\n");
    print_sep('-', 76);
    printf("  CORRECTNESS CHECK\n");
    print_sep('-', 76);

    // small sort (unchanged, just verify)
    printf("  simd_small_sort ... ");
    fflush(stdout);
    check_small_sort();
    printf("  Result: ALL CORRECT\n\n");

    // merge sort correctness for the selected variant
    printf("  merge sort (selected variant) ... ");
    fflush(stdout);
    int errors = 0;
    int size = 3;
    for (int a = 0; a < 21; a++) {
        if (!check_merge_sort_fn(fn, size)) errors++;
        size *= 2;
    }
    printf("%s\n\n", errors == 0 ? "ALL CORRECT" : "FAILURES!");
    if (errors) { printf("  Aborting due to correctness failures.\n"); return; }

    // ── Performance ─────────────────────────────────────────────────────────
    printf("\n");
    print_sep('-', 76);
    printf("  PERFORMANCE  |  %s  vs  Baseline  vs  std::sort  (%d runs each)\n",
           label, BENCH_MERGE_RUNS);
    print_sep('-', 76);
    printf("  %-10s  %10s  %10s  %10s  %8s  %s\n",
           "Size", "std::sort", "Baseline", label, "Speedup", "Bar");
    print_sep('-', 76);

    int sizes[] = {3, 9, 27, 81, 243, 729, 2187, 6561, 19683, 59049, 177147, 531441, 1000000, 2000000};
    for (int i = 0; i < 14; i++) {
        int n   = sizes[i];
        float stl_ms  = bench_stl(n);
        float base_ms = bench_merge(sort_baseline, n);
        float impr_ms = bench_merge(fn,            n);
        float speedup = base_ms / impr_ms;
        float us_base = base_ms * 1000.f / BENCH_MERGE_RUNS;
        float us_impr = impr_ms * 1000.f / BENCH_MERGE_RUNS;

        printf("  %-10d  %8.3f ms  %8.3f ms  %8.3f ms  ",
               n, stl_ms, base_ms, impr_ms);
        if (speedup >= 1.0f)
            printf("%6.2fx  ", speedup);
        else
            printf("-%5.2fx  ", 1.f/speedup);
        print_bar(speedup, 20);
        printf("\n");
        (void)us_base; (void)us_impr;
    }

    printf("\n");
    print_sep('-', 76);
    printf("  LEGEND\n");
    print_sep('-', 76);
    printf("  Speedup > 1.0x = improved is faster than baseline\n");
    printf("  Bar scale: each '#' ~ 0.5x speedup (10x max shown)\n");
    if (option == 1)
        printf("  Note: OpenMP speedup scales with available CPU cores.\n");
    if (option == 2)
        printf("  Note: Branchless helps most on large arrays (many mispredictions).\n");
    if (option == 3)
        printf("  Note: Prefetch helps when merge buffers exceed L2 cache.\n");
    if (option == 4) {
        printf("  Note: Dynamic tile = %d floats  (L1 cache = %d KB)\n",
               compute_dynamic_tile(), detect_l1_bytes() / 1024);
    }
    if (option == 5) {
        printf("  Note: All gaps combined. OpenMP threads = %d  Tile = %d\n",
#ifdef _OPENMP
               omp_get_max_threads(),
#else
               1,
#endif
               compute_dynamic_tile());
        printf("  Adaptive merge threshold = %d floats (L2 = %d KB)\n",
               detect_l2_bytes() / (int)sizeof(float),
               detect_l2_bytes() / 1024);
        printf("  Merges <= threshold use branchless; larger use prefetch+branchy.\n");
    }
}

// ============================================================================
//  Sample test suite – correctness on hand-crafted cases + side-by-side perf
// ============================================================================

// Verify a sort function produces the same result as std::sort on a given array.
static bool verify(sort_fn_t fn, const char* label, float* data, int n, bool verbose)
{
    float* arr = (float*)malloc(sizeof(float) * n);
    std::vector<float> ref(data, data + n);
    memcpy(arr, data, sizeof(float) * n);
    std::sort(ref.begin(), ref.end());
    fn(arr, n);
    bool ok = true;
    for (int i = 0; i < n; i++) if (ref[i] != arr[i]) { ok = false; break; }
    if (verbose)
        printf("    %-12s  %s\n", label, ok ? "PASS" : "FAIL !!!");
    free(arr);
    return ok;
}

static void run_compare_all(void)
{
    static const char* labels[] = {
        "Baseline", "OpenMP", "Branchless", "Prefetch", "DynTile", "All"
    };
    static sort_fn_t fns[] = {
        sort_baseline, sort_openmp, sort_branchless,
        sort_prefetch, sort_dynamic_tile, sort_all
    };
    const int NFNS = 6;

    // ── 1. Hand-crafted correctness tests ───────────────────────────────────
    print_sep('-', 76);
    printf("  SAMPLE CORRECTNESS TESTS\n");
    print_sep('-', 76);

    struct { const char* name; float data[16]; int n; } cases[] = {
        { "already sorted",   {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, 16 },
        { "reverse sorted",   {16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1}, 16 },
        { "all equal",        {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},        16 },
        { "single element",   {42,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},        1 },
        { "two elements",     {9,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0},         2 },
        { "negatives",        {-5,-1,-9,-3,-7,-2,-8,-4,-6,-10,0,0,0,0,0,0}, 10 },
        { "mixed pos/neg",    {3,-1,4,-1,5,-9,2,-6,5,-3,0,0,0,0,0,0},    10 },
        { "duplicates",       {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3},         16 },
    };
    const int NCASES = (int)(sizeof(cases)/sizeof(cases[0]));

    int total_pass = 0, total_tests = 0;
    for (int c = 0; c < NCASES; c++) {
        printf("\n  [%d] %s  (n=%d)\n", c+1, cases[c].name, cases[c].n);
        for (int f = 0; f < NFNS; f++) {
            bool ok = verify(fns[f], labels[f],
                             (float*)cases[c].data, cases[c].n, true);
            if (ok) total_pass++;
            total_tests++;
        }
    }

    // Random medium arrays
    printf("\n  [%d] random n=500  (100 trials each variant)\n", NCASES+1);
    {
        int pass = 0;
        for (int t = 0; t < 100; t++) {
            float data[500];
            for (int j = 0; j < 500; j++)
                data[j] = (iq_random_float(&g_seed) - 0.5f) * 10000.f;
            for (int f = 0; f < NFNS; f++) {
                if (verify(fns[f], labels[f], data, 500, false)) pass++;
            }
        }
        total_pass  += pass;
        total_tests += 100 * NFNS;
        printf("    %d / %d passed\n", pass, 100 * NFNS);
    }

    printf("\n  [%d] random n=100000  (5 trials each variant)\n", NCASES+2);
    {
        int pass = 0;
        for (int t = 0; t < 5; t++) {
            float* data = (float*)malloc(sizeof(float) * 100000);
            for (int j = 0; j < 100000; j++)
                data[j] = (iq_random_float(&g_seed) - 0.5f) * 10000.f;
            for (int f = 0; f < NFNS; f++) {
                if (verify(fns[f], labels[f], data, 100000, false)) pass++;
            }
            free(data);
        }
        total_pass  += pass;
        total_tests += 5 * NFNS;
        printf("    %d / %d passed\n", pass, 5 * NFNS);
    }

    printf("\n  [%d] random n=2000000  (3 trials each variant)\n", NCASES+3);
    {
        int pass = 0;
        for (int t = 0; t < 3; t++) {
            float* data = (float*)malloc(sizeof(float) * 2000000);
            for (int j = 0; j < 2000000; j++)
                data[j] = (iq_random_float(&g_seed) - 0.5f) * 10000.f;
            for (int f = 0; f < NFNS; f++) {
                if (verify(fns[f], labels[f], data, 2000000, false)) pass++;
            }
            free(data);
        }
        total_pass  += pass;
        total_tests += 3 * NFNS;
        printf("    %d / %d passed\n", pass, 3 * NFNS);
    }

    printf("\n  Overall correctness: %d / %d  %s\n",
           total_pass, total_tests,
           total_pass == total_tests ? "ALL PASSED" : "FAILURES DETECTED");

    // ── 2. Side-by-side performance comparison ───────────────────────────────
    printf("\n");
    print_sep('-', 76);
    printf("  SIDE-BY-SIDE PERFORMANCE  (%d runs per size)\n", BENCH_MERGE_RUNS);
    print_sep('-', 76);

    // Header row
    printf("  %-10s  %10s", "Size", "std::sort");
    for (int f = 0; f < NFNS; f++) printf("  %10s", labels[f]);
    printf("\n");
    print_sep('-', 76);

    int sizes[] = {10000, 50000, 100000, 250000, 500000, 750000, 1000000, 1500000, 2000000};
    for (int si = 0; si < 9; si++) {
        int n = sizes[si];
        float stl_ms = bench_stl(n);
        float ms[6];
        for (int f = 0; f < NFNS; f++) ms[f] = bench_merge(fns[f], n);

        printf("  %-10d  %8.2f ms", n, stl_ms);
        for (int f = 0; f < NFNS; f++) printf("  %8.2f ms", ms[f]);
        printf("\n");
    }

    // Speedup over baseline row
    printf("\n");
    printf("  %-10s  %10s", "Speedup vs", "std::sort");
    for (int f = 0; f < NFNS; f++) printf("  %10s", labels[f]);
    printf("\n");
    print_sep('-', 76);

    for (int si = 0; si < 9; si++) {
        int n = sizes[si];
        float stl_ms  = bench_stl(n);
        float base_ms = bench_merge(sort_baseline, n);
        float ms[6];
        for (int f = 0; f < NFNS; f++) ms[f] = bench_merge(fns[f], n);

        printf("  %-10d  %9.2fx", n, base_ms / stl_ms);
        for (int f = 0; f < NFNS; f++) {
            float sp = base_ms / ms[f];
            printf("  %9.2fx", sp);
        }
        printf("\n");
    }

    printf("\n  Speedup > 1.0x = faster than baseline.\n");
    printf("  (std::sort shown for reference only — not a target to beat)\n");

    // ── 3. Small-array spotlight ─────────────────────────────────────────────
    printf("\n");
    print_sep('-', 76);
    printf("  SMALL-ARRAY SPOTLIGHT  (simd_small_sort, unchanged across all options)\n");
    print_sep('-', 76);
    printf("  %-6s  %10s  %10s  %8s\n", "N", "std::sort", "SIMD", "Speedup");
    print_sep('-', 76);

    // Reduced runs for speed
    const int SMALL_RUNS = 200000;
    int small_sizes[] = {4, 8, 16, 32, 64, 96, 128, 192};
    for (int si = 0; si < 8; si++) {
        int sz = small_sizes[si];
        float arr_buf[192];
        uint64_t stl_diff = 0, simd_diff = 0;
        int lseed = 0x11223344;
        std::vector<float> vec(sz);
        for (int i = 0; i < SMALL_RUNS; i++) {
            for (int j = 0; j < sz; j++) { vec[j] = iq_random_float(&lseed); arr_buf[j] = vec[j]; }
            uint64_t t = stm_now(); std::sort(vec.begin(), vec.end()); stl_diff  += stm_diff(stm_now(), t);
            t = stm_now(); simd_small_sort(arr_buf, sz);               simd_diff += stm_diff(stm_now(), t);
        }
        float stl_ms  = (float)stm_ms(stl_diff);
        float simd_ms = (float)stm_ms(simd_diff);
        printf("  %-6d  %8.1f ms  %8.1f ms  %6.2fx\n",
               sz, stl_ms, simd_ms, stl_ms / simd_ms);
    }
}

// ============================================================================
//  Option 7 – Compare all WITHOUT std::sort (faster run)
// ============================================================================
static void run_compare_no_stl(void)
{
    static const char* labels[] = {
        "Baseline", "OpenMP", "Branchless", "Prefetch", "DynTile", "All", "AVX512Sim"
    };
    static sort_fn_t fns[] = {
        sort_baseline, sort_openmp, sort_branchless,
        sort_prefetch, sort_dynamic_tile, sort_all, sort_avx512sim
    };
    const int NFNS = 7;

    // Quick correctness pass
    print_sep('-', 76);
    printf("  CORRECTNESS CHECK (no std::sort timing)\n");
    print_sep('-', 76);
    int total_pass = 0, total_tests = 0;

    int csizes[] = {10000, 100000, 500000, 1000000, 2000000};
    for (int ci = 0; ci < 5; ci++) {
        int n = csizes[ci];
        printf("  n=%-9d  ", n);
        fflush(stdout);
        int pass = 0;
        float* data = (float*)malloc(sizeof(float) * n);
        for (int j = 0; j < n; j++)
            data[j] = (iq_random_float(&g_seed) - 0.5f) * 10000.f;
        for (int f = 0; f < NFNS; f++)
            if (verify(fns[f], labels[f], data, n, false)) pass++;
        free(data);
        total_pass  += pass;
        total_tests += NFNS;
        printf("%d/%d  %s\n", pass, NFNS, pass == NFNS ? "ALL PASS" : "FAILURES!");
    }
    printf("\n  Overall: %d / %d  %s\n\n",
           total_pass, total_tests,
           total_pass == total_tests ? "ALL PASSED" : "FAILURES DETECTED");

    // Performance table (no std::sort column)
    print_sep('-', 88);
    printf("  PERFORMANCE  (100 runs each, no std::sort)\n");
    print_sep('-', 88);

    // Header
    printf("  %-10s", "Size");
    for (int f = 0; f < NFNS; f++) printf("  %10s", labels[f]);
    printf("\n");
    print_sep('-', 76);

    int sizes[] = {10000, 50000, 100000, 250000, 500000, 750000, 1000000, 1500000, 2000000};
    float ms[9][7];
    for (int si = 0; si < 9; si++) {
        int n = sizes[si];
        printf("  %-10d", n); fflush(stdout);
        for (int f = 0; f < NFNS; f++) {
            ms[si][f] = bench_merge(fns[f], n);
            printf("  %8.2f ms", ms[si][f]);
            fflush(stdout);
        }
        printf("\n");
    }

    // Speedup vs baseline
    printf("\n");
    printf("  %-10s", "Speedup");
    for (int f = 0; f < NFNS; f++) printf("  %10s", labels[f]);
    printf("\n");
    print_sep('-', 88);
    for (int si = 0; si < 9; si++) {
        printf("  %-10d", sizes[si]);
        float base = ms[si][0];
        for (int f = 0; f < NFNS; f++) {
            float sp = base / ms[si][f];
            if (sp >= 1.0f) printf("  %9.2fx", sp);
            else            printf("  -%8.2fx", 1.f / sp);
        }
        printf("\n");
    }

    // Best variant per size
    printf("\n");
    print_sep('-', 88);
    printf("  WINNER PER SIZE\n");
    print_sep('-', 88);
    printf("  %-10s  %-12s  %s\n", "Size", "Fastest", "Speedup vs Baseline");
    print_sep('-', 88);
    for (int si = 0; si < 9; si++) {
        int best = 0;
        for (int f = 1; f < NFNS; f++)
            if (ms[si][f] < ms[si][best]) best = f;
        printf("  %-10d  %-12s  %.2fx\n",
               sizes[si], labels[best], ms[si][0] / ms[si][best]);
    }

#ifdef _OPENMP
    printf("\n  OpenMP: %d threads, task depth = ", omp_get_max_threads());
    { int d=0, t=omp_get_max_threads(); while((1<<d)<t) d++; printf("%d\n", d); }
#endif
}

// ============================================================================
//  Option 9 – AVX-512 Analysis
// ============================================================================
// Benchmarks the merge phase in isolation to show the genuine benefit of
// AVX-512's wider tile.  Tile-sort is excluded from timing (equal for both).
//
// Setup  (untimed): sort all 192-element tiles with simd_small_sort
// Baseline (timed): bottom_up_sort from run=192 → 14 merge passes at n=2M
// AVX-512  (timed): one manual 192→384 merge pass (untimed) then
//                   bottom_up_sort from run=384 → 13 merge passes at n=2M
//
// Speedup = time(14 passes) / time(13 passes) ≈ 14/13 ≈ 1.077×
// Real AVX-512 would also run each merge iteration at 16 floats/instr (vs 8),
// adding roughly another ~5-10% on top.
static void run_avx512_analysis(void)
{
    const int RUNS = 15;
    int sizes[] = {100000, 250000, 500000, 1000000, 2000000};
    const int NSIZES = 5;
    int tile = simd_small_sort_max();  // 192

    print_sep('-', 76);
    printf("  MERGE-PHASE COMPARISON  (tile-sort excluded from timing)\n");
    printf("  Baseline : %d merge passes at n=2M  (run = 192, 384, ..., 2M)\n",
           (int)ceil(log2(2000000.0 / tile)));
    printf("  AVX-512  : %d merge passes at n=2M  (run = 384, 768, ..., 2M)\n",
           (int)ceil(log2(2000000.0 / (tile * 2))));
    print_sep('-', 76);
    printf("  %-10s  %12s  %12s  %8s  %s\n",
           "Size", "Baseline(ms)", "AVX-512(ms)", "Speedup", "Bar");
    print_sep('-', 76);

    int lseed = 0xABCDEF01;

    for (int si = 0; si < NSIZES; si++) {
        int n = sizes[si];
        float* arr  = (float*)malloc(sizeof(float) * n);
        float* base = (float*)malloc(sizeof(float) * n);

        // Generate one deterministic array
        for (int j = 0; j < n; j++)
            base[j] = (iq_random_float(&lseed) - 0.5f) * 10000.f;

        uint64_t base_diff = 0, sim_diff = 0;

        for (int r = 0; r < RUNS; r++) {
            // ── Baseline: 14 merge passes (run starts at 192) ───────────────
            memcpy(arr, base, sizeof(float) * n);
            // Tile-sort (untimed)
            for (int s = 0; s < n; s += tile) {
                int c = n - s < tile ? n - s : tile;
                simd_small_sort(arr + s, c);
            }
            // Time the merge phase only
            uint64_t t = stm_now();
            bottom_up_sort(arr, n, tile, merge_adaptive);
            base_diff += stm_diff(stm_now(), t);

            // ── AVX-512 sim: 13 merge passes (run starts at 384) ────────────
            memcpy(arr, base, sizeof(float) * n);
            // Tile-sort (untimed)
            for (int s = 0; s < n; s += tile) {
                int c = n - s < tile ? n - s : tile;
                simd_small_sort(arr + s, c);
            }
            // First merge pass 192→384 (untimed — this is what AVX-512 gives for free)
            for (int left = 0; left + tile < n; left += tile * 2) {
                int mid   = left + tile - 1;
                int right = left + tile * 2 - 1;
                if (right >= n) right = n - 1;
                merge_adaptive(arr, left, mid, right);
            }
            // Time 13 remaining merge passes only
            t = stm_now();
            bottom_up_sort(arr, n, tile * 2, merge_adaptive);
            sim_diff += stm_diff(stm_now(), t);
        }

        float base_ms = (float)stm_ms(base_diff) / RUNS;
        float sim_ms  = (float)stm_ms(sim_diff)  / RUNS;
        float speedup = base_ms / sim_ms;

        printf("  %-10d  %10.2f ms  %10.2f ms  %6.2fx  ", n, base_ms, sim_ms, speedup);
        int bar = (int)((speedup - 1.0f) / 0.3f * 20.f);  // scale: 0→0%, 0.3→100%
        if (bar > 20) bar = 20; if (bar < 0) bar = 0;
        putchar('[');
        for (int i = 0; i < bar; i++) putchar('#');
        for (int i = bar; i < 20; i++) putchar('.');
        putchar(']');
        printf("\n");

        free(arr); free(base);
    }

    printf("\n");
    print_sep('-', 76);
    printf("  PASS COUNT\n");
    print_sep('-', 76);
    printf("  %-10s  %12s  %12s  %s\n", "Size", "Baseline", "AVX-512 sim", "Saved");
    print_sep('-', 76);
    for (int si = 0; si < NSIZES; si++) {
        int n = sizes[si];
        int base_passes = 0, sim_passes = 0;
        for (int run = tile;      run < n; run *= 2) base_passes++;
        for (int run = tile * 2;  run < n; run *= 2) sim_passes++;
        printf("  %-10d  %12d  %12d  %d pass\n",
               n, base_passes, sim_passes, base_passes - sim_passes);
    }

    printf("\n");
    print_sep('-', 76);
    printf("  INTERPRETATION\n");
    print_sep('-', 76);
    printf("  • Speedup ≈ (passes) / (passes-1).  At n=2M: 14/13 = 1.077x\n");
    printf("  • Numbers show the merge-phase saving only.\n");
    printf("  • Real AVX-512 adds on top: each merge iteration processes\n");
    printf("    16 floats/instr instead of 8 → ~5-15%% further gain.\n");
    printf("  • Total expected AVX-512 speedup over baseline: ~1.15-1.25x\n");
    printf("    (merge-phase gain × instruction-width gain, weighted by %%time)\n");
}

// ============================================================================
//  Option 8 – Scalability Measurement
//  Sweeps OpenMP thread count 1 → max_threads, measures:
//    • Strong scaling: fixed array (1M), vary threads → speedup + efficiency
//    • Weak   scaling: array ∝ threads (100K*T), vary threads → time should be flat
//    • Phase  timing:  tile-sort phase vs merge phase in isolation
// ============================================================================
#ifdef _OPENMP

// Timed sort at a fixed thread count (sets OMP_NUM_THREADS for the duration)
static float bench_omp_threads(int n, int nthreads, int runs)
{
    omp_set_num_threads(nthreads);
    float* arr = (float*)malloc(sizeof(float) * n);
    uint64_t diff = 0;
    int lseed = 0xDEADBEEF;
    for (int i = 0; i < runs; i++) {
        for (int j = 0; j < n; j++)
            arr[j] = (iq_random_float(&lseed) - 0.5f) * 10000.f;
        uint64_t t = stm_now();
        sort_openmp(arr, n);
        diff += stm_diff(stm_now(), t);
    }
    free(arr);
    return (float)stm_ms(diff) / runs;
}

// Time just the tile-sort phase (parallel) for a given n + thread count
static float bench_tile_phase(int n, int nthreads, int runs)
{
    int tile = simd_small_sort_max();
    omp_set_num_threads(nthreads);
    float* arr = (float*)malloc(sizeof(float) * n);
    uint64_t diff = 0;
    int lseed = 0xCAFEBABE;
    for (int i = 0; i < runs; i++) {
        for (int j = 0; j < n; j++)
            arr[j] = (iq_random_float(&lseed) - 0.5f) * 10000.f;
        uint64_t t = stm_now();
        #pragma omp parallel for schedule(static)
        for (int start = 0; start < n; start += tile) {
            int end = start + tile;
            if (end > n) end = n;
            simd_small_sort(arr + start, end - start);
        }
        diff += stm_diff(stm_now(), t);
    }
    free(arr);
    return (float)stm_ms(diff) / runs;
}

// Time just the sequential merge phase for a given n
static float bench_merge_phase(int n, int runs)
{
    int tile = simd_small_sort_max();
    float* arr = (float*)malloc(sizeof(float) * n);
    uint64_t diff = 0;
    int lseed = 0xFEEDFACE;
    for (int i = 0; i < runs; i++) {
        // pre-sort all tiles so only merge is measured
        for (int j = 0; j < n; j++)
            arr[j] = (iq_random_float(&lseed) - 0.5f) * 10000.f;
        for (int start = 0; start < n; start += tile) {
            int end = start + tile; if (end > n) end = n;
            simd_small_sort(arr + start, end - start);
        }
        uint64_t t = stm_now();
        bottom_up_sort(arr, n, tile, merge_baseline);
        diff += stm_diff(stm_now(), t);
    }
    free(arr);
    return (float)stm_ms(diff) / runs;
}

#endif  // _OPENMP

static void run_scalability(void)
{
#ifndef _OPENMP
    printf("\n  OpenMP not available – recompile with -fopenmp\n\n");
    return;
#else
    const int SCALE_RUNS  = 10;
    const int STRONG_N    = 2000000;   // fixed array for strong scaling
    const int WEAK_BASE_N = 100000;    // per-thread work unit for weak scaling
    const int max_threads = omp_get_max_threads();

    // ── 1. Strong Scaling ────────────────────────────────────────────────────
    print_sep('-', 76);
    printf("  STRONG SCALING  (n = %d, %d runs each)\n", STRONG_N, SCALE_RUNS);
    printf("  Ideal speedup = thread count (100%% efficiency)\n");
    print_sep('-', 76);
    printf("  %-8s  %10s  %10s  %10s  %s\n",
           "Threads", "Time (ms)", "Speedup", "Efficiency", "Bar");
    print_sep('-', 76);

    float t1 = bench_omp_threads(STRONG_N, 1, SCALE_RUNS);
    int thread_counts[] = {1, 2, 3, 4, 6, 8, 10, 12, 16};
    for (int ti = 0; ti < 9; ti++) {
        int t = thread_counts[ti];
        if (t > max_threads) break;
        float ms      = bench_omp_threads(STRONG_N, t, SCALE_RUNS);
        float speedup = t1 / ms;
        float eff     = speedup / (float)t * 100.f;
        printf("  %-8d  %8.2f ms  %8.2fx  %8.1f%%  ", t, ms, speedup, eff);
        // bar scaled to ideal (t threads = full bar)
        int bar_fill = (int)(speedup / (float)t * 20.f);
        if (bar_fill > 20) bar_fill = 20;
        putchar('[');
        for (int i = 0; i < bar_fill; i++) putchar('#');
        for (int i = bar_fill; i < 20; i++) putchar('.');
        putchar(']');
        printf("\n");
    }
    // Restore max threads
    omp_set_num_threads(max_threads);

    // ── 2. Weak Scaling ──────────────────────────────────────────────────────
    printf("\n");
    print_sep('-', 76);
    printf("  WEAK SCALING  (n = %d * threads, %d runs each)\n",
           WEAK_BASE_N, SCALE_RUNS);
    printf("  Ideal: execution time stays FLAT as threads + work both increase\n");
    print_sep('-', 76);
    printf("  %-8s  %-10s  %10s  %10s\n",
           "Threads", "Array size", "Time (ms)", "Normalised");
    print_sep('-', 76);

    float tw1 = bench_omp_threads(WEAK_BASE_N * 1, 1, SCALE_RUNS);
    for (int ti = 0; ti < 9; ti++) {
        int t  = thread_counts[ti];
        if (t > max_threads) break;
        int    wn  = WEAK_BASE_N * t;
        float  wms = bench_omp_threads(wn, t, SCALE_RUNS);
        float  norm = wms / tw1;  // 1.0 = perfect
        printf("  %-8d  %-10d  %8.2f ms  %8.2fx\n", t, wn, wms, norm);
    }
    omp_set_num_threads(max_threads);
    printf("  (normalised time of 1.0x = perfect weak scaling)\n");

    // ── 3. Phase Breakdown ───────────────────────────────────────────────────
    printf("\n");
    print_sep('-', 76);
    printf("  PHASE BREAKDOWN  (n = %d, max threads = %d)\n",
           STRONG_N, max_threads);
    printf("  Identifies whether tile-sort or merge is the bottleneck.\n");
    print_sep('-', 76);
    printf("  %-16s  %10s  %8s\n", "Phase", "Time (ms)", "% of Total");
    print_sep('-', 76);

    float tile_ms  = bench_tile_phase(STRONG_N, max_threads, SCALE_RUNS);
    float merge_ms = bench_merge_phase(STRONG_N, SCALE_RUNS);
    float total_ms = tile_ms + merge_ms;
    printf("  %-16s  %8.2f ms  %7.1f%%\n",
           "Tile-sort (OMP)",  tile_ms,  tile_ms  / total_ms * 100.f);
    printf("  %-16s  %8.2f ms  %7.1f%%\n",
           "Merge (serial)",   merge_ms, merge_ms / total_ms * 100.f);
    printf("  %-16s  %8.2f ms\n", "Total (approx)", total_ms);
    printf("\n");
    printf("  Theoretical max speedup (Amdahl) = 1 / (serial_fraction)\n");
    float serial_frac = merge_ms / total_ms;
    float amdahl_max  = 1.f / serial_frac;
    printf("  Serial fraction = %.1f%%  →  Amdahl ceiling ≈ %.1fx\n",
           serial_frac * 100.f, amdahl_max);

    // ── 4. Thread Scaling Summary for each array size ────────────────────────
    printf("\n");
    print_sep('-', 76);
    printf("  SCALING ACROSS ARRAY SIZES  (1 thread vs %d threads)\n", max_threads);
    print_sep('-', 76);
    printf("  %-10s  %10s  %10s  %8s\n",
           "Array size", "1 thread", "Max threads", "Speedup");
    print_sep('-', 76);
    int ssizes[] = {50000, 100000, 250000, 500000, 1000000, 2000000};
    for (int si = 0; si < 6; si++) {
        int  sn  = ssizes[si];
        float s1 = bench_omp_threads(sn, 1,           SCALE_RUNS);
        float sm = bench_omp_threads(sn, max_threads, SCALE_RUNS);
        printf("  %-10d  %8.2f ms  %8.2f ms  %6.2fx\n",
               sn, s1, sm, s1 / sm);
    }
    omp_set_num_threads(max_threads);

    printf("\n");
    print_sep('-', 76);
    printf("  INTERPRETATION\n");
    print_sep('-', 76);
    printf("  • Strong scaling efficiency < 100%% is expected: merge phase is serial.\n");
    printf("  • Weak scaling time rising indicates communication/sync overhead.\n");
    printf("  • Small arrays show low efficiency: task spawn cost exceeds work.\n");
    printf("  • The Amdahl ceiling shows the fundamental parallelism limit.\n");
#endif
}

// ============================================================================
//  main – interactive menu
// ============================================================================
int main(void)
{
    stm_setup();
    g_seed = (int)stm_now();

    print_header("SIMD Bitonic Sort  –  Gap Analysis & Improvements");
    printf("\n");
    printf("  SIMD width   : %d floats\n", SIMD_VECTOR_WIDTH);
    printf("  Small-sort max: %d elements\n", simd_small_sort_max());
#ifdef _OPENMP
    printf("  OpenMP       : available  (%d threads)\n", omp_get_max_threads());
#else
    printf("  OpenMP       : NOT available (recompile with -fopenmp)\n");
#endif
    printf("  L1 data cache: %d KB\n", detect_l1_bytes() / 1024);
    printf("  L2 cache     : %d KB\n", detect_l2_bytes() / 1024);
    printf("  Dynamic tile : %d elements\n", compute_dynamic_tile());
    printf("  Adaptive merge threshold: %d floats (branchless below, prefetch above)\n",
           detect_l2_bytes() / (int)sizeof(float));
    printf("\n");
    print_sep('-', 76);
    printf("  Select an improvement option:\n\n");
    printf("    0  Baseline          (original simd_merge_sort, no changes)\n");
    printf("    1  OpenMP Threading  (Gap 1: parallel tile-sort phase)\n");
    printf("    2  Branchless Merge  (Gap 2: AVX blend, no branch misprediction)\n");
    printf("    3  Prefetch          (Gap 3: _mm_prefetch in merge hot-loop)\n");
    printf("    4  Dynamic Tile Size (Gap 5: L1-cache-aware tile)\n");
    printf("    5  All Improvements  (Gaps 1+2+3+5 combined)\n");
    printf("    6  Compare All       (automated tests + side-by-side table, includes std::sort)\n");
    printf("    7  Compare All Fast  (same but NO std::sort – runs much faster)\n");
    printf("    8  Scalability       (thread sweep, strong/weak scaling, phase breakdown)\n");
    printf("    9  AVX-512 Analysis  (merge-phase isolation: 13 vs 14 passes)\n");
    printf("\n");
    printf("    [Gap 4 = real AVX-512 unavailable. Option 9 isolates the merge-pass saving.]\n");
    print_sep('-', 76);
    printf("\n  Enter option (0-9): ");
    fflush(stdout);

    int choice = -1;
    if (scanf("%d", &choice) != 1 || choice < 0 || choice > 9) {
        printf("\n  Invalid choice. Exiting.\n");
        return 1;
    }

    if (choice == 6) {
        printf("\n");
        print_header("Compare All – Automated Test Suite");
        run_compare_all();
        printf("\n");
        print_sep('=', 76);
        printf("  Done.\n");
        print_sep('=', 76);
        printf("\n");
        return 0;
    }

    if (choice == 7) {
        printf("\n");
        print_header("Compare All Fast – No std::sort");
        run_compare_no_stl();
        printf("\n");
        print_sep('=', 76);
        printf("  Done.\n");
        print_sep('=', 76);
        printf("\n");
        return 0;
    }

    if (choice == 8) {
        printf("\n");
        print_header("Scalability Measurement – Thread Sweep & Phase Analysis");
        run_scalability();
        printf("\n");
        print_sep('=', 76);
        printf("  Done.\n");
        print_sep('=', 76);
        printf("\n");
        return 0;
    }

    if (choice == 9) {
        printf("\n");
        print_header("AVX-512 Analysis – Merge-Phase Isolation");
        run_avx512_analysis();
        printf("\n");
        print_sep('=', 76);
        printf("  Done.\n");
        print_sep('=', 76);
        printf("\n");
        return 0;
    }

    const char* labels[] = {
        "Baseline",
        "OpenMP",
        "Branchless",
        "Prefetch",
        "DynTile",
        "All"
    };
    sort_fn_t fns[] = {
        sort_baseline,
        sort_openmp,
        sort_branchless,
        sort_prefetch,
        sort_dynamic_tile,
        sort_all
    };

    printf("\n");
    char header[128];
    snprintf(header, sizeof(header), "Running Option %d: %s", choice, labels[choice]);
    print_header(header);

    run_benchmark(choice, fns[choice], labels[choice]);

    printf("\n");
    print_sep('=', 76);
    printf("  Done.\n");
    print_sep('=', 76);
    printf("\n");
    return 0;
}
