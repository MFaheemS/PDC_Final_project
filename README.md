# SIMD Bitonic Sort — PDC Final Project

AVX2 + OpenMP accelerated merge sort with multiple optimization variants.

---

## Requirements

- GCC with AVX2 and OpenMP support (tested: GCC 15, MinGW-w64 on Windows)
- CPU with AVX2 (Intel Haswell 2013+ or AMD Ryzen 2017+)

---

## Build

```bash
cd improved/test/simd_bitonic
g++ -O3 -mavx2 -fopenmp -std=c++11 -o simd_bitonic main.cpp
```

---

## Run

```bash
./simd_bitonic
```

You will see a menu — type a number and press Enter:

```
  0  Baseline
  1  OpenMP Threading
  2  Branchless Merge
  3  Prefetch
  4  Dynamic Tile Size
  5  All Improvements
  6  Compare All        (includes std::sort — slow, 60–120 min)
  7  Compare All Fast   (no std::sort — recommended, ~5–10 min)
  8  Scalability        (thread sweep 1 → 12)
  9  AVX-512 Analysis
```

**Recommended:** option `7` for a full comparison, option `8` for scaling results.

### Non-interactive (pipe input)

```bash
echo "7" | ./simd_bitonic
```

### Set thread count

```bash
# Windows
$env:OMP_NUM_THREADS = "12"; ./simd_bitonic

# Linux
OMP_NUM_THREADS=12 ./simd_bitonic
```

### Save output to file

```bash
echo "7" | ./simd_bitonic > results.txt
echo "8" | ./simd_bitonic > scalability.txt
```
