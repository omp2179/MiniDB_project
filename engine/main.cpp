// ═══════════════════════════════════════════════
//  MiniDB — Step 7 Validation & Benchmark Harness
//  Benchmarking & Metrics
// ═══════════════════════════════════════════════

#include "DiskManager.h"
#include "Engine.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace minidb;
using Clock = std::chrono::high_resolution_clock;

// ── Helpers ──────────────────────────────────

void cleanup(const std::vector<std::string> &files) {
  std::error_code ec;
  for (const auto &f : files) {
    std::filesystem::remove(f, ec);
  }
}

std::string pad_key(int n) {
  // Zero-padded 8-digit key for sequential ordering.
  // "key_00000001", "key_00000002", etc.
  std::string s = "key_";
  std::string num = std::to_string(n);
  s += std::string(8 - num.size(), '0') + num;
  return s;
}

std::string make_value(int n) { return "value_" + std::to_string(n); }

// ─────────────────────────────────────────────
//  Functional Regression Tests
// ─────────────────────────────────────────────
//  Run the Step 6 validation PLUS additional
//  tests before benchmarking to ensure correctness.

void test_regression() {
  std::cout << "[TEST] Functional regression... ";

  const std::string db = "bench_regress.db";
  const std::string wal = "bench_regress.wal";
  cleanup({db, wal});

  {
    Engine engine(db, wal, 32);

    engine.put("apple", "1.50");
    engine.put("banana", "0.75");
    engine.put("cherry", "3.00");
    engine.put("date", "5.00");

    assert(engine.get("banana") == "0.75");
    assert(engine.get("apple") == "1.50");
    assert(engine.get("nonexistent") == "");

    auto results = engine.scan("banana", "cherry");
    assert(results.size() == 2);
    assert(results[0].first == "banana");
    assert(results[1].first == "cherry");

    // Test update.
    engine.put("banana", "1.25");
    assert(engine.get("banana") == "1.25");

    assert(engine.index_size() == 4);
  }

  cleanup({db, wal});
  std::cout << "PASSED\n";
}

// ─────────────────────────────────────────────
//  Persistence & Recovery Test
// ─────────────────────────────────────────────

void test_persistence() {
  std::cout << "[TEST] Persistence across restart... ";

  const std::string db = "bench_persist.db";
  const std::string wal = "bench_persist.wal";
  cleanup({db, wal});

  // Phase 1: Write data and close.
  {
    Engine engine(db, wal, 16);
    for (int i = 0; i < 100; ++i) {
      engine.put(pad_key(i), make_value(i));
    }
  }

  // Phase 2: Reopen and verify all data survived.
  {
    Engine engine(db, wal, 16);
    for (int i = 0; i < 100; ++i) {
      std::string val = engine.get(pad_key(i));
      assert(val == make_value(i));
    }
  }

  cleanup({db, wal});
  std::cout << "PASSED — 100 keys survived restart\n";
}

// ═══════════════════════════════════════════════
//  BENCHMARK 1: Write Throughput (Indexed)
// ═══════════════════════════════════════════════
//
// Measures: Engine::put() × 10,000
// Includes: WAL log + fsync + buffer pool write + index update + commit
// This is the "real" end-to-end write cost.

struct BenchResult {
  int ops;
  double ms;
  double ops_per_sec;
};

BenchResult bench_write(int count) {
  const std::string db = "bench_write.db";
  const std::string wal = "bench_write.wal";
  cleanup({db, wal});

  BenchResult result{};
  result.ops = count;

  {
    Engine engine(db, wal, 256);

    auto start = Clock::now();

    for (int i = 0; i < count; ++i) {
      engine.put(pad_key(i), make_value(i));
    }

    auto end = Clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.ops_per_sec = (result.ops / result.ms) * 1000.0;
  cleanup({db, wal});
  return result;
}

// ═══════════════════════════════════════════════
//  BENCHMARK 2: Read Throughput (Indexed)
// ═══════════════════════════════════════════════
//
// Measures: Engine::get() × 10,000
// Includes: Index lookup + buffer pool fetch (copy)
// Pre-populates the database, then measures reads only.

BenchResult bench_read(int count) {
  const std::string db = "bench_read.db";
  const std::string wal = "bench_read.wal";
  cleanup({db, wal});

  BenchResult result{};
  result.ops = count;

  {
    Engine engine(db, wal, 256);

    // Pre-populate.
    for (int i = 0; i < count; ++i) {
      engine.put(pad_key(i), make_value(i));
    }

    auto start = Clock::now();

    for (int i = 0; i < count; ++i) {
      std::string val = engine.get(pad_key(i));
      assert(!val.empty());
    }

    auto end = Clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.ops_per_sec = (result.ops / result.ms) * 1000.0;
  cleanup({db, wal});
  return result;
}

// ═══════════════════════════════════════════════
//  BENCHMARK 3: Scan Throughput
// ═══════════════════════════════════════════════
//
// Measures: Engine::scan() over the full key range.
// Shows the benefit of std::map's sorted iteration.

BenchResult bench_scan(int count) {
  const std::string db = "bench_scan.db";
  const std::string wal = "bench_scan.wal";
  cleanup({db, wal});

  BenchResult result{};
  result.ops = count;

  {
    Engine engine(db, wal, 256);

    for (int i = 0; i < count; ++i) {
      engine.put(pad_key(i), make_value(i));
    }

    auto start = Clock::now();

    auto results = engine.scan(pad_key(0), pad_key(count - 1));
    assert(static_cast<int>(results.size()) == count);

    auto end = Clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.ops_per_sec = (result.ops / result.ms) * 1000.0;
  cleanup({db, wal});
  return result;
}

// ═══════════════════════════════════════════════
//  BENCHMARK 3a: Cold Read Throughput
// ═══════════════════════════════════════════════
//
// Demonstrates disk I/O cost when buffer pool cache misses.

BenchResult bench_cold_read(int count) {
  const std::string db = "bench_cold_read.db";
  const std::string wal = "bench_cold_read.wal";
  cleanup({db, wal});

  BenchResult result{};
  result.ops = count;

  {
    Engine engine(db, wal, 256);
    for (int i = 0; i < count; ++i) {
      engine.put(pad_key(i), make_value(i));
    }
  } // Destructor forces all to disk, clears memory.

  {
    // Reopen with tiny buffer pool (16 frames).
    // For 10,000 keys, 99.8% of random reads will miss the cache
    // and force a disk read (though OS page cache may help).
    Engine engine(db, wal, 16);

    auto start = Clock::now();
    for (int i = 0; i < count; ++i) {
      int target =
          (i * 101) % count; // Pseudo-random to defeat sequential prefetch
      std::string val = engine.get(pad_key(target));
      assert(!val.empty());
    }
    auto end = Clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.ops_per_sec = (result.ops / result.ms) * 1000.0;
  cleanup({db, wal});
  return result;
}

// ═══════════════════════════════════════════════
//  BENCHMARK 3b: Concurrent Write Throughput
// ═══════════════════════════════════════════════

BenchResult bench_concurrent_write(int total_count, int num_threads) {
  const std::string db = "bench_concurrent.db";
  const std::string wal = "bench_concurrent.wal";
  cleanup({db, wal});

  BenchResult result{};
  result.ops = total_count;

  {
    Engine engine(db, wal, 256);
    std::vector<std::thread> threads;
    int ops_per_thread = total_count / num_threads;

    auto start = Clock::now();

    for (int t = 0; t < num_threads; ++t) {
      threads.emplace_back([&engine, t, ops_per_thread]() {
        for (int i = 0; i < ops_per_thread; ++i) {
          int key_idx = t * ops_per_thread + i;
          engine.put(pad_key(key_idx), make_value(key_idx));
        }
      });
    }

    for (auto &th : threads) {
      th.join();
    }

    auto end = Clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.ops_per_sec = (result.ops / result.ms) * 1000.0;
  cleanup({db, wal});
  return result;
}

// ═══════════════════════════════════════════════
//  BENCHMARK 3c: Group Commit (Batch fsync)
// ═══════════════════════════════════════════════

BenchResult bench_group_commit(int count, int batch_size) {
  const std::string db = "bench_group.db";
  const std::string wal = "bench_group.wal";
  cleanup({db, wal});

  BenchResult result{};
  result.ops = count;

  {
    Engine engine(db, wal, 256);

    auto start = Clock::now();

    for (int i = 0; i < count; ++i) {
      // put without sync
      engine.put(pad_key(i), make_value(i), false);

      if ((i + 1) % batch_size == 0) {
        engine.sync_wal();
      }
    }
    if (count % batch_size != 0) {
      engine.sync_wal();
    }

    auto end = Clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.ops_per_sec = (result.ops / result.ms) * 1000.0;
  cleanup({db, wal});
  return result;
}

// ═══════════════════════════════════════════════
//  BENCHMARK 4: Indexed Read vs Full Disk Scan
// ═══════════════════════════════════════════════
//
// Compares:
//   (a) Engine::get() — uses index for O(log n) lookup
//   (b) Brute-force disk scan — reads every page until key found
//
// This demonstrates the concrete speedup the index provides.

struct SpeedupResult {
  double indexed_ms;
  double scan_ms;
  double speedup;
  int lookups;
};

SpeedupResult bench_index_speedup(int total_keys, int lookups) {
  const std::string db = "bench_speedup.db";
  const std::string wal = "bench_speedup.wal";
  cleanup({db, wal});

  SpeedupResult result{};
  result.lookups = lookups;

  {
    Engine engine(db, wal, 256);

    // Populate the database.
    for (int i = 0; i < total_keys; ++i) {
      engine.put(pad_key(i), make_value(i));
    }

    // Force all pages to disk so the scan benchmark is fair.
    // (Destructor calls flush_all, but we need it now.)
  }

  // --- Method A: Indexed reads via Engine ---
  {
    Engine engine(db, wal, 256);

    auto start = Clock::now();
    for (int i = 0; i < lookups; ++i) {
      // Look up keys spread across the range.
      int target = (i * 97) % total_keys; // Pseudo-random, deterministic.
      std::string val = engine.get(pad_key(target));
      assert(!val.empty());
    }
    auto end = Clock::now();

    result.indexed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
  }

  // --- Method B: Brute-force disk scan (no index) ---
  {
    DiskManager disk(db);
    uint32_t num_pages = disk.num_pages();

    auto start = Clock::now();
    for (int i = 0; i < lookups; ++i) {
      int target = (i * 97) % total_keys;
      std::string target_key = pad_key(target);

      // Scan every page until we find the key.
      bool found = false;
      for (uint32_t pid = 0; pid < num_pages; ++pid) {
        char page[DISK_PAGE_SIZE] = {};
        disk.read_page(pid, page);

        const char *ks = page + PAGE_KEY_OFFSET;
        const char *ke = std::find(ks, ks + LogRecord::KEY_SIZE, '\0');
        std::string key(ks, ke);

        if (key == target_key) {
          found = true;
          break;
        }
      }
      assert(found);
    }
    auto end = Clock::now();

    result.scan_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.speedup = result.scan_ms / result.indexed_ms;
  cleanup({db, wal});
  return result;
}

// ─────────────────────────────────────────────
//  Print Helpers
// ─────────────────────────────────────────────

void print_separator() {
  std::cout << "───────────────────────────────────────────────\n";
}

void print_bench(const std::string &label, const BenchResult &r) {
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  [" << label << "]  " << r.ops << " ops in " << r.ms
            << " ms → " << std::setprecision(0) << r.ops_per_sec
            << " ops/sec\n";
}

// ─────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────

int main() {
  std::cout << "═══════════════════════════════════════════════\n";
  std::cout << " MiniDB — Step 7: Benchmarking & Metrics\n";
  std::cout << "═══════════════════════════════════════════════\n\n";

  // ── Phase 1: Functional Verification ──────
  std::cout << "Phase 1: Functional Verification\n";
  print_separator();

  test_regression();
  test_persistence();

  std::cout << "\n";

  // ── Phase 2: Performance Benchmarks ───────
  constexpr int OPS = 10'000;

  std::cout << "Phase 2: Performance Benchmarks (" << OPS << " operations)\n";
  print_separator();

  auto write_result = bench_write(OPS);
  print_bench("WRITE ", write_result);

  auto read_result = bench_read(OPS);
  print_bench("READ  ", read_result);

  auto scan_result = bench_scan(OPS);
  print_bench("SCAN  ", scan_result);

  auto cold_read_result = bench_cold_read(OPS);
  print_bench("C-READ", cold_read_result);

  auto conc_write_result = bench_concurrent_write(OPS, 4); // 4 threads
  print_bench("C-WRT ", conc_write_result);

  auto gc10_result = bench_group_commit(OPS, 10);
  print_bench("GRP-10", gc10_result);

  auto gc100_result = bench_group_commit(OPS, 100);
  print_bench("GRP100", gc100_result);

  std::cout << "\n";

  // ── Phase 3: Index Speedup ────────────────
  constexpr int SPEEDUP_KEYS = 5'000;
  constexpr int SPEEDUP_LOOKUPS = 500;

  std::cout << "Phase 3: Index Speedup (" << SPEEDUP_KEYS << " keys, "
            << SPEEDUP_LOOKUPS << " lookups)\n";
  print_separator();

  auto speedup = bench_index_speedup(SPEEDUP_KEYS, SPEEDUP_LOOKUPS);

  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  [INDEXED]    " << speedup.lookups << " lookups in "
            << speedup.indexed_ms << " ms\n";
  std::cout << "  [FULL SCAN]  " << speedup.lookups << " lookups in "
            << speedup.scan_ms << " ms\n";
  std::cout << "  [SPEEDUP]    " << std::setprecision(0) << speedup.speedup
            << "x faster with index\n";

  std::cout << "\n";

  // ── Phase 4: Summary Table ────────────────
  std::cout << "═══════════════════════════════════════════════\n";
  std::cout << " BENCHMARK SUMMARY\n";
  std::cout << "═══════════════════════════════════════════════\n";
  std::cout << std::fixed;
  std::cout << "  Write throughput (sync):   " << std::setprecision(0)
            << write_result.ops_per_sec << " ops/sec\n";
  std::cout << "  Concurrent Write (4T):     " << std::setprecision(0)
            << conc_write_result.ops_per_sec << " ops/sec\n";
  std::cout << "  Group Commit (10/batch):   " << std::setprecision(0)
            << gc10_result.ops_per_sec << " ops/sec\n";
  std::cout << "  Group Commit (100/batch):  " << std::setprecision(0)
            << gc100_result.ops_per_sec << " ops/sec\n";
  std::cout << "  Read throughput (warm):    " << std::setprecision(0)
            << read_result.ops_per_sec << " ops/sec\n";
  std::cout << "  Read throughput (cold):    " << std::setprecision(0)
            << cold_read_result.ops_per_sec << " ops/sec\n";
  std::cout << "  Scan throughput:           " << std::setprecision(0)
            << scan_result.ops_per_sec << " ops/sec\n";
  std::cout << "  Index speedup:             " << std::setprecision(0)
            << speedup.speedup << "x\n";
  std::cout << "═══════════════════════════════════════════════\n\n";

  // ── Sanity Assertions ─────────────────────
  assert(write_result.ops_per_sec > 1000);
  std::cout << "✓ STEP 7 COMPLETE — Benchmark numbers recorded\n";

  return 0;
}
