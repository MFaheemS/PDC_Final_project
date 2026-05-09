# SIMD Bitonic Sort — PDC Project

AVX2-accelerated bitonic sort with OpenMP parallelism, branchless merge, prefetch hints, and dynamic tile sizing. Benchmarks and compares multiple optimisation strategies against the single-threaded baseline and `std::sort`.

---

## Repository Structure

```
PDC_Final_project/
├── original/                          # Unmodified baseline library + bare benchmark
│   ├── simd_bitonic.h
│   └── test/simd_bitonic/main.cpp
│
├── original_detailed_output/          # Same logic, human-readable output
│   ├── simd_bitonic.h
│   ├── test/simd_bitonic/main.cpp
│   └── benchmark_output_gcc15.txt     # Captured reference run
│
├── improved/                          # All gap improvements (primary deliverable)
│   ├── simd_bitonic.h
│   └── test/simd_bitonic/
│       ├── main.cpp                   # Interactive benchmark with 9 options (0–8)
│       ├── sokol_time.h               # High-resolution timer
│       └── random.h                  # Deterministic RNG
│
├── REPORT.md                          # Full project report
└── README.md                          # This file
```

---

## Requirements

| Tool | Version | Notes |
|------|---------|-------|
| GCC | ≥ 9 (tested with GCC 15) | MinGW-w64 on Windows, or native GCC on Linux |
| CPU | AVX2 capable | Intel Haswell (2013) or newer; AMD Ryzen (2017) or newer |
| OpenMP | Bundled with GCC | Pass `-fopenmp`; remove for single-threaded build |

---

## Build Instructions

### Improved Version (primary)

```bash
cd improved/test/simd_bitonic
g++ -O3 -mavx2 -fopenmp -std=c++11 -o bitonic_sort_improved.exe main.cpp
```

**Windows (PowerShell):**
```powershell
cd improved\test\simd_bitonic
g++ -O3 -mavx2 -fopenmp -std=c++11 -o bitonic_sort_improved.exe main.cpp
```

### Original Baseline

```bash
cd original/test/simd_bitonic
g++ -O3 -mavx2 -std=c++11 -o bitonic_sort_original.exe main.cpp
```

### Original with Detailed Output

```bash
cd original_detailed_output/test/simd_bitonic
g++ -O3 -mavx2 -std=c++11 -o bitonic_sort.exe main.cpp
```

---

## Running the Improved Benchmark

```bash
./bitonic_sort_improved.exe
```

The program prints hardware info and an interactive menu:

```
  Select an improvement option:

    0  Baseline          (original simd_merge_sort, no changes)
    1  OpenMP Threading  (Gap 1: parallel tile-sort phase)
    2  Branchless Merge  (Gap 2: AVX blend, no branch misprediction)
    3  Prefetch          (Gap 3: _mm_prefetch in merge hot-loop)
    4  Dynamic Tile Size (Gap 5: L1-cache-aware tile)
    5  All Improvements  (Gaps 1+2+3+5 combined)
    6  Compare All       (automated tests + side-by-side table, includes std::sort)
    7  Compare All Fast  (same but NO std::sort – runs much faster)
    8  Scalability       (thread sweep, strong/weak scaling, phase breakdown)

  Enter option (0-8):
```

Type a number and press Enter.

### Option Guide

| Option | What it does | Typical runtime |
|--------|-------------|-----------------|
| 0–5 | Correctness check + perf vs baseline + std::sort for one variant | 10–30 min |
| 6 | All 6 variants side-by-side including std::sort | 60–120 min |
| 7 | All 6 variants side-by-side **without** std::sort (recommended) | 5–10 min |
| 8 | Thread sweep (1→12), strong/weak scaling, phase breakdown | 15–25 min |

### Setting Thread Count

```bash
# Windows PowerShell
$env:OMP_NUM_THREADS = "12"
.\bitonic_sort_improved.exe

# Linux / bash
OMP_NUM_THREADS=12 ./bitonic_sort_improved.exe
```

The program detects and displays the thread count at startup. Default is all logical processors.

### Non-interactive (scripted) run

```bash
echo "7" | ./bitonic_sort_improved.exe    # Linux
```

```powershell
# Windows PowerShell — use Bash for pipe input
bash -c 'echo "7" | ./bitonic_sort_improved.exe'
```

---

## Reproducing Benchmark Results

All benchmarks use a deterministic local seed (`0xABCDEF01`) so all variants sort identical arrays. Results are reproducible to within ~5% run-to-run (OS scheduling noise on timing).

**Recommended sequence for a full reproduction:**

```bash
# Step 1: Quick correctness + performance, no std::sort
echo "7" | ./bitonic_sort_improved.exe > results_no_stl.txt

# Step 2: Scalability / thread sweep
echo "8" | ./bitonic_sort_improved.exe > results_scalability.txt

# Step 3: Full comparison (takes ~60–120 min)
echo "6" | ./bitonic_sort_improved.exe > results_full.txt
```

---

## Understanding the Output

### Options 0–5

```
  CORRECTNESS CHECK
  simd_small_sort ... [100%] Pass 1000/1000
  Result: ALL CORRECT

  merge sort (selected variant) ... ALL CORRECT

  PERFORMANCE  |  OpenMP  vs  Baseline  vs  std::sort  (100 runs each)
  Size        std::sort    Baseline      OpenMP   Speedup  Bar
  --------------------------------------------------------------------------
  2000000   2400.00 ms   240.00 ms    80.00 ms    3.00x  [############........]
```

- **Speedup > 1.0×** = this variant is faster than the single-threaded baseline
- **Bar scale**: each `#` ≈ 0.5× speedup (20 chars = 10× max)

### Option 7 (recommended)

Adds a winner-per-size table and a speedup grid over baseline for all 6 variants.

### Option 8 (Scalability)

```
  STRONG SCALING  (n=2000000, 10 runs each)
  Threads    Time (ms)     Speedup  Efficiency  Bar
  1          240.00 ms      1.00x      100.0%  [####################]
  4           96.00 ms      2.50x       62.5%  [############........]
  12          75.00 ms      3.20x       26.7%  [######..............]

  PHASE BREAKDOWN
  Tile-sort (OMP)    30.00 ms     30.0%
  Merge (serial)     70.00 ms     70.0%
  Amdahl ceiling ≈ 1.43x (at infinite threads)
```

- **Strong scaling efficiency** = speedup / threads × 100%. 100% = perfect linear scaling.
- **Amdahl ceiling** = 1 / serial_fraction — the theoretical maximum speedup regardless of thread count.
- **Weak scaling normalised** = time at T threads / time at 1 thread. 1.0× = perfect; rising = overhead growing.

---

## Gap Summary

| # | Name | Technique | Observed Speedup |
|---|------|-----------|-----------------|
| 1 | OpenMP | Task-based recursive parallelism | **2.5–3.2×** at n≥1M |
| 2 | Branchless | `_mm256_blendv_ps` in merge hot-loop | ~1.05–1.15× |
| 3 | Prefetch | `_mm_prefetch` hints in merge | ~1.05–1.20× |
| 4 | AVX-512 Sim | Double tile=384, one fewer merge pass (option 9) | **~1.10–1.15×** (conservative lower bound) |
| 5 | Dynamic tile | CPUID-detected L1 cache sizing | ~1.00× on this hardware |
| — | All combined | 1+2+3+5 | **~3.0–3.2×** (dominated by OpenMP) |

---

## Notes

- Gap 4 (AVX-512) is **not implemented** — it requires hardware with AVX-512 support (Intel Skylake-X / Ice Lake Xeon or AMD Zen 4+).
- Options 0–5 each run 1,000,000 small-sort correctness passes first (~5 min). Skip to option 7 or 8 for faster results.
- On Linux, pre-built binaries are not provided — compile from source as shown above.
