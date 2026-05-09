#define __SIMD_BITONIC_IMPLEMENTATION__
#include "../../simd_bitonic.h"

#define SOKOL_IMPL
#include "sokol_time.h"

#include "random.h"

#include <vector>
#include <algorithm>
#include <stdio.h>
#include <string.h>

#define NUMBER_OF_SORTS (1000000)
#define MAX_ARRAY_SIZE  (SIMD_VECTOR_WIDTH * 32)

int seed = 0x12345678;

// ─── helpers ────────────────────────────────────────────────────────────────

static void print_separator(char c, int width)
{
    for (int i = 0; i < width; ++i) putchar(c);
    putchar('\n');
}

static void print_header(const char* title)
{
    int width = 72;
    print_separator('=', width);
    int pad = (width - (int)strlen(title)) / 2;
    printf("%*s%s\n", pad, "", title);
    print_separator('=', width);
}

static void print_section(const char* title)
{
    printf("\n");
    print_separator('-', 72);
    printf("  %s\n", title);
    print_separator('-', 72);
}

static void print_bar(float ratio, int bar_width)
{
    // ratio > 1 means SIMD is faster
    int filled = (int)(ratio / 10.0f * bar_width);
    if (filled > bar_width) filled = bar_width;
    if (filled < 1)         filled = 1;
    putchar('[');
    for (int i = 0; i < filled;      ++i) putchar('#');
    for (int i = filled; i < bar_width; ++i) putchar('.');
    putchar(']');
}

// ─── correctness checks ─────────────────────────────────────────────────────

void check_correctness(int array_size)
{
    float array[MAX_ARRAY_SIZE];
    std::vector<float> vector(array_size);

    for (int j = 0; j < array_size; ++j)
    {
        vector[j] = (iq_random_float(&seed) - 0.5f) * 10000.f;
        array[j]  = vector[j];
    }

    std::sort(vector.begin(), vector.end());
    simd_small_sort(array, array_size);

    for (int j = 0; j < array_size; ++j)
        assert(vector[j] == array[j]);
}

void check_merge_sort(int array_size)
{
    float* array = (float*) malloc(sizeof(float) * array_size);
    std::vector<float> vector(array_size);

    for (int j = 0; j < array_size; ++j)
    {
        vector[j] = (iq_random_float(&seed) - 0.5f) * 10000.f;
        array[j]  = vector[j];
    }

    std::sort(vector.begin(), vector.end());
    simd_merge_sort(array, array_size);

    for (int j = 0; j < array_size; ++j)
        assert(vector[j] == array[j]);

    free(array);
}

// ─── profiling ──────────────────────────────────────────────────────────────

void profile_small(int array_size)
{
    float array[MAX_ARRAY_SIZE];
    std::vector<float> vector(array_size);

    uint64_t start_time;
    uint64_t stl_diff  = 0;
    uint64_t simd_diff = 0;
    float result      = 0.f;
    float simd_result = 0.f;

    // STL std::sort pass
    for (int i = 0; i < NUMBER_OF_SORTS; ++i)
    {
        for (int j = 0; j < array_size; ++j)
            vector[j] = iq_random_float(&seed);

        start_time = stm_now();
        std::sort(vector.begin(), vector.end());
        stl_diff += stm_diff(stm_now(), start_time);

        result += vector[0];
    }

    // SIMD bitonic pass
    seed = 0x12345678;
    for (int i = 0; i < NUMBER_OF_SORTS; ++i)
    {
        for (int j = 0; j < array_size; ++j)
            array[j] = iq_random_float(&seed);

        start_time = stm_now();
        simd_small_sort(array, array_size);
        simd_diff += stm_diff(stm_now(), start_time);

        simd_result += array[0];
    }

    float stl_ms   = (float)stm_ms(stl_diff);
    float simd_ms  = (float)stm_ms(simd_diff);
    float ratio    = stl_ms / simd_ms;

    float stl_ns_per  = stl_ms  * 1e6f / NUMBER_OF_SORTS;
    float simd_ns_per = simd_ms * 1e6f / NUMBER_OF_SORTS;

    printf("  %-4d elems | STL: %8.2f ms (%6.1f ns/sort) | SIMD: %8.2f ms (%6.1f ns/sort) | ",
           array_size,
           stl_ms,  stl_ns_per,
           simd_ms, simd_ns_per);

    if (ratio >= 1.0f)
        printf("Speedup: %5.2fx  ", ratio);
    else
        printf("Slowdown:%5.2fx  ", 1.0f / ratio);

    print_bar(ratio, 20);
    printf("\n");

    (void)result; (void)simd_result; // prevent optimisation-away
}

void profile_merge_sort(int array_size, int run_index)
{
    float* array = (float*) malloc(sizeof(float) * array_size);
    std::vector<float> vector(array_size);

    uint64_t stl_diff  = 0;
    uint64_t simd_diff = 0;

    const int RUNS = 100;
    for (int i = 0; i < RUNS; ++i)
    {
        for (int j = 0; j < array_size; ++j)
        {
            vector[j] = (iq_random_float(&seed) - 0.5f) * 10000.f;
            array[j]  = vector[j];
        }

        uint64_t t = stm_now();
        std::sort(vector.begin(), vector.end());
        stl_diff += stm_diff(stm_now(), t);

        t = stm_now();
        simd_merge_sort(array, array_size);
        simd_diff += stm_diff(stm_now(), t);
    }

    float stl_ms  = (float)stm_ms(stl_diff);
    float simd_ms = (float)stm_ms(simd_diff);
    float ratio   = stl_ms / simd_ms;

    float stl_us_per  = stl_ms  * 1000.f / RUNS;
    float simd_us_per = simd_ms * 1000.f / RUNS;

    printf("  %-10d elems | STL: %8.3f ms (%8.2f us/sort) | SIMD: %8.3f ms (%8.2f us/sort) | ",
           array_size,
           stl_ms,  stl_us_per,
           simd_ms, simd_us_per);

    if (ratio >= 1.0f)
        printf("Speedup: %5.2fx  ", ratio);
    else
        printf("Slowdown:%5.2fx  ", 1.0f / ratio);

    print_bar(ratio, 20);
    printf("\n");

    free(array);
}

// ─── main ───────────────────────────────────────────────────────────────────

int main(int argc, const char* argv[])
{
    stm_setup();
    seed = (int)stm_now();

    print_header("SIMD Bitonic Sort -- Baseline Benchmark");
    printf("\n");
    printf("  SIMD vector width : %d floats\n", SIMD_VECTOR_WIDTH);
    printf("  Max small-sort    : %d elements\n", simd_small_sort_max());
    printf("  Small-sort runs   : %d per size\n", NUMBER_OF_SORTS);
    printf("  Merge-sort runs   : 100 per size\n");
    printf("\n");

    // ── correctness: small sort ──────────────────────────────────────────────
    print_section("CORRECTNESS CHECK  |  simd_small_sort");
    printf("  Running 1000 x [2..%d] elements against std::sort reference ...\n",
           simd_small_sort_max());

    int errors = 0;
    for (int a = 0; a < 1000; ++a)
    {
        for (int i = 2; i <= simd_small_sort_max(); ++i)
            check_correctness(i);

        if (a % 100 == 0)
        {
            int pct = a / 10;
            printf("  [%3d%%] Pass %d/1000\n", pct, a);
            fflush(stdout);
        }
    }
    printf("  [100%%] Pass 1000/1000\n");
    printf("\n  Result: %s\n", errors == 0 ? "ALL CORRECT" : "FAILURES DETECTED");

    // ── correctness: merge sort ──────────────────────────────────────────────
    print_section("CORRECTNESS CHECK  |  simd_merge_sort");

    int size = 3;
    printf("  %-12s  %-10s  %s\n", "Array Size", "Elements", "Status");
    print_separator('-', 40);
    for (int a = 0; a < 21; ++a)
    {
        check_merge_sort(size);
        printf("  %-12d  %-10d  OK\n", a + 1, size);
        size *= 2;
    }
    printf("\n  Result: ALL CORRECT\n");

    // ── performance: merge sort ──────────────────────────────────────────────
    print_section("PERFORMANCE  |  simd_merge_sort  vs  std::sort  (100 runs each)");
    printf("  %-10s   %-36s   %-36s   %s\n",
           "Size", "STL std::sort", "SIMD Merge Sort", "Speedup / Bar");
    print_separator('-', 72);

    size = 3;
    for (int i = 0; i < 12; ++i)
    {
        profile_merge_sort(size, i);
        size *= 3;
    }

    // ── performance: small sort ──────────────────────────────────────────────
    print_section("PERFORMANCE  |  simd_small_sort  vs  std::sort  (1,000,000 runs each)");
    printf("  %-4s   %-36s   %-36s   %s\n",
           "N", "STL std::sort", "SIMD Small Sort", "Speedup / Bar");
    print_separator('-', 72);

    seed = 0x12345678;
    for (int i = 1; i <= simd_small_sort_max(); ++i)
        profile_small(i);

    // ── summary ─────────────────────────────────────────────────────────────
    print_section("LEGEND");
    printf("  Bar scale: each '#' represents ~0.5x speedup (10x max shown)\n");
    printf("  Speedup > 1.0x means SIMD is faster than std::sort\n");
    printf("  ns/sort = nanoseconds per individual sort call\n");
    printf("  us/sort = microseconds per individual sort call\n");
    printf("\n");
    print_separator('=', 72);
    printf("  Benchmark complete.\n");
    print_separator('=', 72);
    printf("\n");

    return 0;
}
