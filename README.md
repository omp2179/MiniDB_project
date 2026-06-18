# MiniDB

Every time you open Instagram, a key-value store like Amazon DynamoDB serves your feed in single-digit milliseconds. When Discord delivers a message to millions of concurrent users, ScyllaDB handles the fan-out. RocksDB sits at the heart of Meta's distributed infrastructure, handling trillions of queries a day across services like Messenger and the social graph. SQLite is embedded in every smartphone on the planet — your contacts, your browser history, your text messages — all stored in a page-oriented database with a write-ahead log.

Ever wondered what actually happens inside these systems when you call `put("user:1234", data)`? How does the data survive a power failure halfway through a write? How does the database figure out which changes to keep and which to throw away after a crash? How does a page cache decide what stays in memory and what gets evicted back to disk?

MiniDB is a key-value storage engine built in C++17 that answers these questions with working code. It implements the same fundamental architecture that real databases use — disk-backed pages, an LRU buffer pool, a write-ahead log with checksummed records, ARIES-style crash recovery, a sparse index for O(log n) lookups, a Bloom filter to short-circuit unnecessary I/O, and a concurrency model that lets readers proceed in parallel while writers hold exclusive access.

---

## What it does

MiniDB is a persistent, crash-safe key-value store. You give it a key and a value, it records the write to a durable log, caches the page in memory, and guarantees that the data is recoverable even if the process is killed mid-operation.

It ships with three executables:

- **`minidb`** — a benchmark and regression test harness that validates correctness and measures throughput across write, read, scan, cold read, concurrent write, and group commit scenarios
- **`minidb_cli`** — an interactive command-line shell with put, get, delete, scan, status, manual WAL sync, and a crash simulator
- **`minidb_server`** — an HTTP server on port 8080 that powers a live web dashboard where you can operate the database, watch internals in real time, trigger crashes, observe recovery, and run benchmarks — all from the browser

---

## ACID guarantees

MiniDB provides the four classical ACID properties for every operation:

**Atomicity** — Each put/delete is wrapped in its own transaction. The engine writes an ACTIVE record to the WAL, modifies the buffer pool page, then writes a COMMIT record. If the process crashes between ACTIVE and COMMIT, the recovery manager treats the entire operation as if it never happened — it scans the WAL, identifies transactions without a matching COMMIT record, and rolls back their changes using the `old_val` stored in the ACTIVE record.

**Consistency** — The index is rebuilt from the on-disk page contents every time the engine boots, after recovery has already ensured the pages are correct. This means the in-memory state always reflects a consistent snapshot of the data file. Deleted keys are tracked in a free-page queue so their disk slots get reclaimed by future writes.

**Isolation** — A `std::shared_mutex` at the engine level enforces a multiple-readers/single-writer discipline. Multiple concurrent `get()` and `scan()` calls acquire the shared lock and proceed in parallel. Any `put()` or `del()` acquires the exclusive lock, blocking other writers and readers until it finishes. This is equivalent to a serializable isolation level for single-key operations.

**Durability** — The WAL is fsynced to physical storage before the COMMIT record is written (when `sync=true`). This means that once `put()` returns, the data has reached the disk platter (or NAND flash) — not just the OS kernel buffer. Even if the machine loses power at that exact moment, the data is recoverable from the WAL on the next startup.

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                    Engine (facade)                     │
│     put · get · del · scan · recover · checkpoint     │
│         std::shared_mutex (readers ‖ writer ✕)        │
├────────────────┬─────────────────────────────────────┤
│  SparseIndex   │           BloomFilter                │
│  std::map      │     8192-bit · 3 hash functions      │
│  O(log n) get  │     definite-no / maybe-yes          │
├────────────────┴─────────────────────────────────────┤
│                  BufferPool (LRU)                      │
│   fixed frames · dirty tracking · LRU eviction        │
│   std::shared_mutex (read-shared / write-exclusive)   │
├──────────────────────────────────────────────────────┤
│                   DiskManager                         │
│    flat binary file · 4 KB pages · fsync/_commit      │
│           std::mutex (serialize all I/O)              │
├──────────────────────────────────────────────────────┤
│                LogManager (WAL)                       │
│   345-byte fixed records · CRC32 · append-only        │
│        std::mutex (LSN assign + write + sync)         │
├──────────────────────────────────────────────────────┤
│                RecoveryManager                        │
│   analysis → redo (forward) → undo (backward)         │
│           → checkpoint → index rebuild                │
└──────────────────────────────────────────────────────┘
```

### How a write flows through the system

When you call `engine.put("price:apple", "1.50")`, here is every step that happens, in order:

```
1. Engine acquires exclusive lock (std::unique_lock on shared_mutex)
2. Index lookup — does "price:apple" already exist?
   ├── Yes → reuse its existing page_id, read old_val from buffer pool
   └── No  → allocate a new page (from free list, or bump next_page_id)
3. Assign txn_id (monotonic counter, e.g. txn_id=42)
4. WAL: append ACTIVE record
   ├── lsn=N, txn_id=42, page_id=7
   ├── key="price:apple", old_val="", new_val="1.50"
   ├── compute CRC32 over all fields, write to WAL file
   └── (sync=false for the data record — we sync at commit)
5. Buffer pool: write page
   ├── Is page 7 already cached? → promote to MRU
   ├── Not cached, free frame available? → claim it, load page from disk
   └── Not cached, pool full? → evict LRU victim (flush if dirty), reuse frame
   ├── Copy key+value into the 4 KB page at fixed offsets (0 and 64)
   └── Mark frame as dirty
6. Index: insert "price:apple" → page_id=7
7. Bloom filter: set the 3 hash bits for "price:apple"
8. WAL: append COMMIT record
   ├── lsn=N+1, txn_id=42, status=COMMIT
   ├── fflush() → push C buffer to OS kernel
   └── fsync()/_commit() → push OS kernel buffer to physical media
9. Engine releases exclusive lock
```

After step 8, the data is durable. The buffer pool page might still be in memory (not yet flushed to the .db file), but that doesn't matter — if the process crashes, the recovery manager will replay the committed WAL record and write the page to disk during REDO.

### How a read flows through the system

```
1. Engine acquires shared lock (std::shared_lock — parallel with other readers)
2. Bloom filter check: might "price:apple" exist?
   ├── Definitely no → return "" immediately (zero I/O, zero lock contention)
   └── Maybe yes → continue
3. Index lookup: O(log n) search in std::map
   ├── Not found → return ""
   └── Found → page_id=7
4. Buffer pool: read page 7
   ├── Cache hit → copy 4 KB from frame, promote to MRU, return
   └── Cache miss → load from disk via DiskManager::read_page()
       ├── Free frame? → claim and load
       └── Pool full? → evict LRU, flush if dirty, load into freed frame
5. Parse page: extract value from fixed offset 64
6. Engine releases shared lock
7. Return "1.50"
```

### How crash recovery works

The recovery manager runs automatically on every engine startup, before any user request is accepted. It implements a simplified ARIES (Algorithm for Recovery and Isolation Exploiting Semantics) protocol — the same recovery framework used by IBM DB2, SQL Server, and InnoDB.

```
Phase 1 — ANALYSIS (single backward scan for checkpoint, then forward scan)
├── Find the last CHECKPOINT record — skip everything before it
├── Build set of committed txn_ids (any txn with a COMMIT record)
├── Build set of active txn_ids (any txn with an ACTIVE record)
└── Identify "loser" txns: active but not committed → must be undone

Phase 2 — REDO (forward scan from checkpoint)
├── For every ACTIVE record belonging to a committed transaction:
│   read the disk page, write key + new_val, write page back
├── This handles the case where the buffer pool had a dirty page
│   that was never flushed to disk before the crash
└── REDO is idempotent — applying it twice produces the same result

Phase 3 — UNDO (backward scan from end of WAL)
├── For every ACTIVE record belonging to a loser transaction:
│   ├── old_val is empty → was a new insert → zero the entire page
│   └── old_val is non-empty → was an update → restore old_val
├── Backward scan is critical for correctness:
│   If txn wrote to page 5 twice (old="" → "hello", then "hello" → "world")
│   Backward: undo "world"→"hello", then undo "hello"→"" ← correct ✅
│   Forward:  undo ""→"hello" first, then undo "hello"→"world" ← wrong ❌
└── UNDO is also idempotent

Phase 4 — CHECKPOINT
├── fsync all recovered pages
├── Write a CHECKPOINT record to the WAL
└── Future recovery starts after this marker → faster startup
```

After recovery, the engine rebuilds the in-memory index by scanning every page header in the .db file. Pages with a non-empty key are inserted into the index; pages with an empty key are added to the free-page queue.

---

## Component deep dive

### DiskManager

The storage backend. The database file is a flat sequence of 4 KB pages — page N lives at byte offset `N × 4096`. Reads and writes go through `std::fstream` with a `std::mutex` serializing all I/O so that concurrent threads don't interleave seek and read/write operations. Every `sync()` call issues `fflush()` followed by `fsync()` (POSIX) or `_commit()` (Windows) to guarantee data has reached physical storage, not just the OS page cache.

The disk manager also handles file extension — when a page_id beyond the current file end is written, the file grows automatically. And `truncate()` resets the file to zero bytes during a database wipe.

### BufferPool

An LRU page cache that sits between the engine and the disk. It pre-allocates a fixed number of `Frame` objects at construction (each frame is a `std::array<char, 4096>` plus a page_id and dirty bit — no heap allocation at runtime).

**Cache hit path:** Hash lookup in `page_table_` (an `unordered_map<page_id_t, frame_index>`) → O(1). The page is promoted to the front of the LRU list via `std::list::splice` — also O(1), no allocation.

**Cache miss path:** If the free list has a slot, claim it. Otherwise, evict the LRU victim (back of the list). If the victim's dirty bit is set, the flush callback writes it to disk via `DiskManager::write_page()` before the frame is reclaimed. Then the requested page is loaded from disk into the freed frame.

**Concurrency:** The buffer pool has its own `std::shared_mutex`. Read-only queries (`contains`, `get_page_copy`) acquire the shared lock. Operations that modify LRU state or page bytes (`read_page_copy`, `write_page_copy`, `flush_all`) acquire the exclusive lock. Hit/miss counters use `std::atomic<uint64_t>` with relaxed ordering — they're observability metrics, not correctness state.

**Safe-copy design:** Callers receive a `std::array<char, 4096>` by value, not a pointer into the frame. The lock is held only during the copy. This eliminates an entire class of bugs where a caller holds a reference to a frame that gets evicted out from under it.

### LogManager (WAL)

The write-ahead log. Every mutation is appended here *before* the buffer pool is touched — this is the WAL protocol that guarantees durability.

Each `LogRecord` is a fixed-size 345-byte packed struct:

| Field | Size | Purpose |
|-------|------|---------|
| `lsn` | 8 bytes | Log Sequence Number — monotonically increasing, uniquely identifies the record |
| `txn_id` | 8 bytes | Transaction identifier — ties ACTIVE and COMMIT records together |
| `page_id` | 4 bytes | Which disk page this write modifies |
| `key` | 64 bytes | The key being written (zero-padded) |
| `old_val` | 128 bytes | Previous value — used by UNDO during recovery |
| `new_val` | 128 bytes | New value — used by REDO during recovery |
| `status` | 1 byte | ACTIVE (0), COMMIT (1), or CHECKPOINT (2) |
| `checksum` | 4 bytes | CRC32 over all preceding fields — detects partial/corrupt writes |

Fixed-size records make WAL scanning trivial: record N starts at byte `N × 345`. No length-prefix parsing, no variable-length headers.

**Group commit:** By default, `put()` fsyncs the WAL at the COMMIT record. But you can pass `sync=false` to skip the fsync and call `sync_wal()` explicitly after a batch. This amortizes the cost of fsync — one of the most expensive operations in database I/O — across many writes. The benchmark harness measures both modes: per-write sync vs. batched fsync with batch sizes of 10 and 100.

**Concurrency:** A `std::mutex` protects the WAL file handle and the LSN counter. The sequence `assign LSN → fwrite → fsync` must be atomic to prevent interleaved records from different threads.

### RecoveryManager

Implements the crash recovery protocol described in the architecture section above. The key properties:

- **Both REDO and UNDO are idempotent** — applying them twice produces the same result. This is critical because the recovery manager doesn't know whether a page was flushed to disk before the crash or not. If it was, REDO overwrites with the same data (harmless no-op). If it wasn't, REDO applies the missing change.
- **UNDO scans backward** — this ensures correct ordering when a single transaction modified the same page multiple times. Forward undo would restore intermediate states in the wrong order.
- **Checkpoint optimization** — each recovery pass writes a CHECKPOINT record at the end. Future boots skip everything before the checkpoint, reducing recovery time from O(total WAL size) to O(records since last checkpoint).

### SparseIndex

An in-memory `std::map<string, uint64_t>` mapping every live key to its page_id on disk. `get()` is O(log n) via balanced BST lookup. Range scans use `lower_bound` and iterate in sorted key order — `scan("apple", "cherry")` returns all keys in that range inclusive, using the natural ordering of the tree.

The index is transient — it's rebuilt from the disk file on every startup (after recovery). Deleted keys are removed from the index and their page slots are pushed onto a FIFO free-page queue so that future writes can reclaim the disk space without growing the file.

### BloomFilter

A probabilistic data structure that sits in front of the index. When a `get()` or search is performed, the Bloom filter is checked first. If it says the key *definitely does not exist*, the engine returns immediately — no index lookup, no buffer pool access, no disk I/O. If it says the key *might exist*, the normal lookup path proceeds.

The filter uses an 8192-bit array (1 KB of memory) with 3 independent hash functions per key. This is enough for the workload sizes MiniDB targets. The filter is rebuilt from the index during startup and during checkpoints (which also clears stale entries from deleted keys).

### Concurrency model

Concurrency is handled at three levels:

| Level | Lock type | Protects | Behavior |
|-------|-----------|----------|----------|
| **Engine** | `std::shared_mutex` | Transaction lifecycle, index, Bloom filter | Multiple readers in parallel; one exclusive writer |
| **BufferPool** | `std::shared_mutex` | Frame storage, page table, LRU list | Shared for residency checks; exclusive for loads/evictions |
| **DiskManager** | `std::mutex` | File handle (seek + read/write) | Serialized — disk I/O is inherently sequential |
| **LogManager** | `std::mutex` | WAL file handle, LSN counter | Serialized — LSN assignment must be monotonic |

Reads (`get`, `scan`) take the engine's shared lock and can run concurrently with each other. Writes (`put`, `del`) take the engine's exclusive lock and run one at a time. This is a deliberate tradeoff — a finer-grained locking scheme (per-page latches, lock-free index) would increase throughput but also complexity. The current model is correct, straightforward to reason about, and still delivers solid performance for the intended workload.

---

## Building

**Requirements:** CMake 3.16+, a C++17 compiler (GCC 8+, Clang 7+, or MSVC 2017+), pthreads (Linux/macOS) or Winsock2 (Windows, for the server).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds three executables: `minidb`, `minidb_cli`, and `minidb_server`.

To build a specific target:

```bash
cmake --build build --target minidb_server
```

### Docker

```bash
docker build -t minidb .
docker run -p 8080:8080 minidb
```

Open `http://localhost:8080` in your browser.

---

## Usage

### Benchmark harness

```bash
./build/minidb
```

Runs functional regression tests (put/get/delete/scan, persistence across restart), then a suite of performance benchmarks:

```
═══════════════════════════════════════════════
 MiniDB — Step 7: Benchmarking & Metrics
═══════════════════════════════════════════════

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

- **WRITE** — end-to-end: WAL log + fsync + buffer pool write + index update + commit
- **READ** — warm buffer pool reads (cache hits)
- **C-READ** — cold reads with a tiny buffer pool (16 frames for 10000 keys), forcing ~99.8% cache misses
- **C-WRT** — 4-thread concurrent writes
- **GRP-10 / GRP-100** — group commit with batch fsync every 10 or 100 writes
- **SPEEDUP** — indexed O(log n) lookup vs. brute-force sequential page scan

### CLI

```bash
./build/minidb_cli
```

```
minidb> put city Tokyo
OK (Durable put successful)

minidb> get city
Value: "Tokyo"

minidb> put country Japan
OK (Durable put successful)

minidb> scan a z
Found 2 records:
  city => "Tokyo"
  country => "Japan"

minidb> status
Engine Status:
  Keys in Index:        2
  Buffer Pool Capacity: 10 frames
  Database file size:   8192 bytes (2 pages)
  WAL file size:        1380 bytes

minidb> crash
💥 Simulating sudden process crash...
```

Additional commands: `put_async` (write without fsync — fast but unsafe unless followed by `sync`), `sync` (force WAL to disk), `help`.

### HTTP server + web UI

```bash
./build/minidb_server
```

The web UI at `http://localhost:8080` provides:

- **Note-taking demo** — write notes that persist through intentional crashes, comparing MiniDB-backed storage (crash-safe) against a naive in-memory store (lost on crash)
- **WAL visualizer** — watch ACTIVE, COMMIT, and CHECKPOINT records appear in real time as you write data
- **Buffer pool monitor** — see which pages are cached, their dirty bits, and the cache hit rate
- **Crash + recovery demo** — trigger a process crash (`SIGKILL`), restart the server, and observe the recovery manager's REDO/UNDO log showing exactly which transactions were replayed and which were rolled back
- **Mid-transaction crash** — write an ACTIVE record without a COMMIT, flush the dirty page to disk, then crash. On restart, the recovery manager detects the uncommitted transaction and undoes the change — proving the atomicity guarantee
- **Index vs. scan comparison** — search for a key using the index (O(log n)) and again using a brute-force sequential disk scan, and see the latency difference side by side
- **Bloom filter demo** — search for a key that doesn't exist and see it get rejected by the Bloom filter before any I/O happens
- **Live benchmarks** — run the throughput benchmark suite from the browser and see per-operation latency numbers

### REST API

The server also exposes a JSON API for programmatic access:

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/api/note` | Store a note (params: `mode`, `session_id`, `text`, `sync`) |
| `DELETE` | `/api/note` | Delete a note |
| `GET` | `/api/notes` | List notes for a session |
| `GET` | `/api/search` | Search by key (params: `key`, `use_index`) |
| `GET` | `/api/status` | Buffer pool stats, WAL tail, recovery log |
| `GET` | `/api/benchmark` | Run benchmark suite, return JSON results |
| `POST` | `/api/checkpoint` | Force a WAL checkpoint |
| `POST` | `/api/crash` | Kill the process (for testing recovery) |
| `POST` | `/api/crash_mid_txn` | Write + flush without commit, then crash |
| `POST` | `/api/wipe` | Factory reset the database |

---

## Project structure

```
MiniDB_project/
├── engine/
│   ├── DiskManager.h / .cpp       Page-level I/O, fsync, file management
│   ├── BufferPool.h / .cpp        LRU page cache with dirty tracking
│   ├── LogManager.h / .cpp        Write-ahead log with CRC32 checksums
│   ├── RecoveryManager.h / .cpp   ARIES-style crash recovery
│   ├── Engine.h / .cpp            Public facade, ACID transaction logic
│   ├── Index.h / .cpp             Sparse index (std::map)
│   ├── BloomFilter.h / .cpp       Probabilistic key existence filter
│   ├── cli.cpp                    Interactive command-line interface
│   ├── main.cpp                   Benchmark and regression test harness
│   └── server.cpp                 HTTP server, REST API, web UI backend
├── public/
│   └── index.html                 Single-file web dashboard
├── CMakeLists.txt                 Build configuration (3 targets)
├── Dockerfile                     Single-stage Docker build
└── .dockerignore
```

---

## Design decisions

**Fixed-size WAL records (345 bytes).** Variable-length records are more space-efficient but require length-prefix parsing during recovery scans. Fixed-size records let you jump to any record by byte offset (`N × 345`) and make the recovery scan a simple sequential read with no branching on record boundaries. The 345-byte size follows naturally from the key (64B), old_val (128B), new_val (128B), and metadata fields.

**CRC32 on every WAL record.** If the process crashes mid-write, the last record in the WAL could be partially written — half-filled bytes that look like valid data but aren't. The checksum detects this during recovery and skips the corrupted record instead of misinterpreting it. This is the same approach used by PostgreSQL's WAL and RocksDB's log format.

**Page copying instead of page pinning.** The buffer pool returns a full `std::array<char, 4096>` by value, not a pointer into the frame. This means the caller can safely read the data after releasing the lock, and the buffer pool can freely evict and reuse frames without worrying about dangling references. The cost is a 4 KB memcpy per access, which is well within L1/L2 cache bandwidth and eliminates use-after-eviction bugs entirely.

**Bloom filter before the index.** The filter sits between the engine's public API and the index/buffer pool. For keys that don't exist, it returns in nanoseconds — no lock escalation, no tree traversal, no disk I/O. The 1 KB bit array stays resident in CPU cache across calls. False positives are harmless (the index just returns "not found"); false negatives never happen.

**Single engine-level shared_mutex.** Finer-grained locking (per-page latches, lock-free concurrent index, separate WAL mutex) would unlock higher concurrent throughput. The single shared_mutex is a deliberate choice for clarity: readers proceed in parallel, writers are exclusive, and the locking invariants are easy to verify. The WAL and buffer pool still have their own internal latches for operations that require them.

**Free-page reclamation via FIFO queue.** When a key is deleted, its page slot is pushed onto a `std::queue<page_id_t>`. Future writes pop from this queue before allocating a new page, reusing the disk space without compaction or garbage collection passes.

**Index rebuilt on startup, not persisted.** The index is an in-memory `std::map` that is reconstructed by scanning every page header in the .db file after recovery. This avoids the complexity of persisting and maintaining a durable index structure (B-tree on disk, WAL entries for index changes), at the cost of a startup scan that is linear in the number of pages. For the dataset sizes MiniDB targets, this scan completes in milliseconds.
