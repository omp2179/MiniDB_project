# MiniDB

A key-value storage engine written from scratch in C++17. Not a wrapper. Not a tutorial toy. Every piece of it — the disk I/O layer, the buffer pool, the write-ahead log, the crash recovery, the sparse index, the Bloom filter, the concurrent request handling — was written by hand, one component at a time.

The goal was to understand exactly what a real database does under the hood by building one. If you've ever wondered how Postgres survives a power cut, or how SQLite keeps your data consistent, this is the answer — in readable, well-commented C++ code you can actually step through.

---

## What it does

MiniDB is a persistent key-value store. You give it a key and a value, it writes them to disk, survives crashes, and gives them back to you exactly as you left them — even after you kill the process mid-write.

It exposes three interfaces:

- **`minidb`** — a benchmark harness that stress-tests the engine and prints throughput numbers
- **`minidb_cli`** — an interactive command-line shell for direct put/get/delete/scan
- **`minidb_server`** — an HTTP server that runs on port 8080 and powers a live web UI

The web UI lets you interact with the database through a browser, watch the WAL records appear in real time, trigger intentional crashes to test recovery, compare indexed lookups against brute-force disk scans side by side, and run live benchmarks.

---

## Architecture

The engine is built in layers, each one sitting cleanly on top of the one below it.

```
┌─────────────────────────────────────────┐
│              Engine (facade)             │
│   put / get / del / scan / recover      │
├──────────────┬──────────────────────────┤
│  SparseIndex │      BloomFilter          │
│  (std::map)  │   (probabilistic guard)   │
├──────────────┴──────────────────────────┤
│             BufferPool (LRU)             │
│    fixed frames · dirty tracking        │
├─────────────────────────────────────────┤
│             DiskManager                  │
│    flat file · 4 KB pages · fsync       │
├─────────────────────────────────────────┤
│         LogManager (WAL)                 │
│  fixed-size records · CRC32 · fsync     │
├─────────────────────────────────────────┤
│         RecoveryManager                  │
│   analysis → redo → undo → checkpoint   │
└─────────────────────────────────────────┘
```

### DiskManager

The file is a flat sequence of 4 KB pages. Page N lives at byte offset `N × 4096`. Reading and writing are done through `fstream` with a mutex protecting seekg/seekp so concurrent threads don't step on each other. Every flush calls `fsync` (or `_commit` on Windows) to push OS buffers all the way to physical media.

### BufferPool

A fixed-size pool of in-memory frames, each holding one 4 KB page. It uses LRU eviction — every access promotes the page to the front of the list, and when the pool is full, the frame at the back (least recently used) gets written to disk and reused. The pool tracks a dirty bit per frame so clean pages are evicted for free. A shared reader-writer latch protects internal state while letting concurrent reads proceed without blocking each other.

### Write-Ahead Log (WAL)

Every write is recorded to the WAL *before* the buffer pool is touched. Each `LogRecord` is a fixed-size 345-byte binary struct with an LSN, transaction ID, page ID, the old value (for UNDO), the new value (for REDO), a status byte, and a CRC32 checksum. Fixed-size records mean scanning is O(1) arithmetic — record N starts at `N × 345`. Group commit is supported: you can batch writes without calling fsync after each one, then flush the WAL explicitly when you're ready.

### RecoveryManager

On every startup, the recovery manager runs a three-phase process before accepting any requests:

1. **Analysis** — scan the WAL, identify which transactions committed and which did not, find the last checkpoint to skip already-applied records
2. **Redo** — re-apply `new_val` from every committed transaction, in case the dirty page was never flushed to disk before the crash
3. **Undo** — restore `old_val` for every uncommitted transaction, in case the dirty page *was* flushed before the crash

After recovery, a fresh CHECKPOINT record is written so the next startup doesn't have to replay records that are already on disk. This is a simplified variant of ARIES — the same algorithm behind IBM DB2, SQL Server, and InnoDB.

### SparseIndex

An in-memory `std::map<string, page_id_t>` that maps every live key to its page ID on disk. Index lookups are O(log n) and point directly to the right page — no scanning. The index is rebuilt from disk on startup by reading every page header. Range scans use `lower_bound` and walk the map in sorted order.

### BloomFilter

A probabilistic guard in front of the storage layer. Before any index lookup, the key is checked against an 8192-bit Bloom filter with three hash functions. If the filter says the key definitely does not exist, the engine returns immediately — no lock acquisition, no index lookup, no disk I/O. False positives are possible (the filter may say "maybe" for keys that aren't there), but false negatives are not — if a key exists, the filter will never reject it.

### Engine

The public facade that wires everything together. A single `std::shared_mutex` coordinates readers and writers at the engine level: multiple concurrent reads proceed in parallel, writes are exclusive. It manages transaction IDs, the free-page queue for reclaiming deleted pages, and exposes the methods used by all three interfaces.

---

## Building

**Requirements:** CMake 3.16+, a C++17 compiler (GCC, Clang, or MSVC), and POSIX threads (automatically linked on Linux/macOS; Winsock2 is linked on Windows for the server).

```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build

# Or build one at a time
cmake --build build --target minidb
cmake --build build --target minidb_cli
cmake --build build --target minidb_server
```

### Docker

```bash
docker build -t minidb .
docker run -p 8080:8080 minidb
```

Then open `http://localhost:8080` in your browser.

---

## Usage

### Benchmark harness

Runs functional tests followed by a suite of throughput benchmarks across write, read, scan, cold read, concurrent write, and group commit scenarios. Also measures the concrete speedup of indexed lookup versus brute-force disk scan.

```bash
./build/minidb
```

Sample output:

```
Phase 1: Functional Verification
───────────────────────────────────────────────
[TEST] Functional regression... PASSED
[TEST] Persistence across restart... PASSED — 100 keys survived restart

Phase 2: Performance Benchmarks (10000 operations)
───────────────────────────────────────────────
  [WRITE ]  10000 ops in 312.4 ms → 32009 ops/sec
  [READ  ]  10000 ops in 8.1 ms → 1234567 ops/sec
  [SCAN  ]  10000 ops in 14.2 ms → 704225 ops/sec
  [C-READ]  10000 ops in 198.3 ms → 50428 ops/sec
  [C-WRT ]  10000 ops in 89.1 ms → 112233 ops/sec
  [GRP-10]  10000 ops in 41.7 ms → 239808 ops/sec
  [GRP100]  10000 ops in 18.3 ms → 546448 ops/sec

Phase 3: Index Speedup (5000 keys, 500 lookups)
───────────────────────────────────────────────
  [INDEXED]    500 lookups in 0.4 ms
  [FULL SCAN]  500 lookups in 812.0 ms
  [SPEEDUP]    2030x faster with index
```

### CLI

```bash
./build/minidb_cli
```

Commands:

```
put <key> <value>          Store a key-value pair
get <key>                  Retrieve a value
del <key>                  Delete a key
scan <start_key> <end_key> List all keys in a range (inclusive)
exit                       Quit
```

### HTTP server + web UI

```bash
./build/minidb_server
```

Open `http://localhost:8080`. The UI lets you:

- Store, retrieve, delete, and scan keys
- Watch WAL records appear in real time as you write
- Trigger a crash (`SIGKILL`) and restart the server to watch recovery undo uncommitted transactions
- Trigger a mid-transaction crash — write an ACTIVE record without a COMMIT, crash, then observe the recovery manager rolling it back
- Compare indexed lookup latency vs. full disk scan latency on live data
- Run the benchmark suite and see throughput numbers directly in the browser
- Force a checkpoint and watch the WAL truncate

---

## Project structure

```
MiniDB_project/
├── engine/
│   ├── BloomFilter.h / .cpp       Probabilistic key filter
│   ├── BufferPool.h / .cpp        LRU page cache
│   ├── DiskManager.h / .cpp       Raw page I/O and fsync
│   ├── Engine.h / .cpp            Public facade — ties all layers together
│   ├── Index.h / .cpp             In-memory sparse index (std::map)
│   ├── LogManager.h / .cpp        Write-ahead log (WAL)
│   ├── RecoveryManager.h / .cpp   Crash recovery (analysis + redo + undo)
│   ├── cli.cpp                    Interactive command-line shell
│   ├── main.cpp                   Benchmark and regression test harness
│   └── server.cpp                 HTTP server + REST API + web UI backend
├── public/
│   └── index.html                 Single-file web UI
├── CMakeLists.txt
├── Dockerfile
└── .dockerignore
```

---

## Design decisions worth noting

**Fixed-size WAL records.** Variable-length records are faster to write but painful to scan during recovery — you need to parse lengths before you can skip forward. Fixed-size records make the recovery scan trivial and let you jump to any record by offset arithmetic. The 345-byte record size is a natural consequence of the chosen key/value size limits.

**CRC32 on every WAL record.** Partial writes are real. If the process dies mid-write, the last record in the WAL might be half-written. The checksum lets the recovery manager detect and skip corrupted records rather than misinterpreting garbage as valid data.

**Page copying instead of pinning.** The buffer pool hands callers a copy of the page bytes rather than a pointer into the frame. This means the lock only needs to be held while copying, and callers can work with the data lock-free. The tradeoff is a 4 KB memcpy per access, which is acceptable at this scale and avoids an entire class of use-after-eviction bugs.

**Bloom filter before every lookup.** The filter pays for itself the moment you have any non-trivial miss rate. A key that doesn't exist triggers zero I/O. The bit array is small enough (1 KB) that it stays hot in L2 cache.

**Single shared mutex on the Engine.** A finer-grained approach — per-page latches, separate WAL mutex, separate index mutex — would give higher concurrent throughput. The single shared mutex is intentional here: it keeps the locking model easy to reason about and is still correct under concurrent reads, which proceed in parallel. The WAL and buffer pool have their own internal latches for the operations that need them.
