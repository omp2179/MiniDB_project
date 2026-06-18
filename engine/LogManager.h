#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace minidb {

// ─────────────────────────────────────────────
//  LogRecord — fixed-size binary WAL entry
// ─────────────────────────────────────────────
//
// Every WAL record is a fixed-size binary struct written atomically
// to the end of the WAL file. Fixed-size records make scanning
// trivial: record N starts at byte offset N * sizeof(LogRecord).
//
// Design improvement over spec: added LSN for ordering and CRC32
// for detecting partial/corrupt writes during recovery.

#pragma pack(push, 1)
struct LogRecord {
  static constexpr size_t KEY_SIZE = 64;
  static constexpr size_t VAL_SIZE = 128;

  // Record status — determines how recovery treats this record.
  enum Status : uint8_t {
    ACTIVE = 0,    // Data modification (transaction in progress)
    COMMIT = 1,    // Transaction committed successfully
    CHECKPOINT = 2 // Recovery checkpoint marker (Step 4)
  };

  uint64_t lsn;           // Log Sequence Number (monotonic)
  uint64_t txn_id;        // Transaction identifier
  uint32_t page_id;       // Page that was modified
  char key[KEY_SIZE];     // Key (zero-padded to 64 bytes)
  char old_val[VAL_SIZE]; // Previous value — for UNDO
  char new_val[VAL_SIZE]; // New value — for REDO
  Status status;          // Record type (ACTIVE/COMMIT/CHECKPOINT)
  uint32_t checksum;      // CRC32 over all preceding fields

  // ── Helpers ──────────────────────────────

  // Compute CRC32 over all fields except checksum itself.
  [[nodiscard]] uint32_t compute_checksum() const;

  // Verify the record's integrity.
  [[nodiscard]] bool verify_checksum() const;

  // Extract the key as a std::string (strips trailing zeros).
  [[nodiscard]] std::string key_str() const;

  // Extract old_val as a std::string.
  [[nodiscard]] std::string old_val_str() const;

  // Extract new_val as a std::string.
  [[nodiscard]] std::string new_val_str() const;
};
#pragma pack(pop)

// Verify the packed struct is exactly 345 bytes:
// 8 (lsn) + 8 (txn_id) + 4 (page_id) + 64 (key) + 128 (old_val)
// + 128 (new_val) + 1 (status) + 4 (checksum) = 345
static_assert(sizeof(LogRecord) == 345,
              "LogRecord must be exactly 345 bytes (packed)");

// ─────────────────────────────────────────────
//  LogManager — Write-Ahead Log manager
// ─────────────────────────────────────────────
//
// Manages the append-only WAL file (engine.wal). Every mutation
// to the database is recorded here BEFORE the in-memory buffer
// is modified. After writing a record, the WAL is forced to
// stable storage via fsync/_commit.
//
// This guarantees: if the system crashes at any point, the WAL
// contains a complete history of all durable operations. The
// RecoveryManager (Step 4) uses this to restore consistency.
//
// Real-world equivalent: PostgreSQL's XLog (WAL) manager,
// SQLite's WAL mode journal.

class LogManager {
public:
  // Opens (or creates) the WAL file at the given path.
  // Scans existing records to determine the next LSN.
  explicit LogManager(const std::string &wal_path);

  // Flushes and closes the WAL file.
  ~LogManager();

  // Non-copyable, non-movable (owns file handle).
  LogManager(const LogManager &) = delete;
  LogManager &operator=(const LogManager &) = delete;

  // ── Write API (called BEFORE buffer pool modification) ──

  // Log a data modification. Returns the assigned LSN.
  // Must be called BEFORE the buffer pool frame is updated.
  uint64_t log_write(uint64_t txn_id, uint32_t page_id, const std::string &key,
                     const std::string &old_val, const std::string &new_val,
                     bool sync = true);

  // Log a transaction commit. Returns the assigned LSN.
  uint64_t log_commit(uint64_t txn_id, bool sync = true);

  // Log a checkpoint marker (used in Step 4 recovery).
  uint64_t log_checkpoint();

  // ── Read API (for recovery) ──────────────

  // Read all valid records from the WAL file.
  // Skips records with invalid checksums (partial writes).
  [[nodiscard]] std::vector<LogRecord> read_all_records() const;

  // Return the last `count` records for the web WAL visualizer.
  [[nodiscard]] std::vector<LogRecord> get_tail_records(int count) const;

  // ── Maintenance ──────────────────────────

  // Truncate the WAL file (called after successful checkpoint).
  void truncate();

  // ── Accessors ────────────────────────────

  [[nodiscard]] uint64_t next_lsn() const { return next_lsn_; }
  [[nodiscard]] const std::string &file_path() const { return wal_path_; }

  // Force WAL data from OS buffers to physical storage.
  // fflush() → pushes C buffers to OS kernel
  // fsync()/_commit() → pushes OS kernel to disk platters/NAND
  void force_sync();

private:
  std::string wal_path_;
  FILE *file_;        // C-style FILE for fsync access
  uint64_t next_lsn_; // Next LSN to assign

  // Append a record: compute checksum, write, and optionally force to disk.
  void append_record(LogRecord &record, bool sync);

  // Caller must already hold wal_mutex_.
  void force_sync_unlocked();

  // ── Concurrency (Step 5) ─────────────────
  // Protects file_ and next_lsn_ during append.
  // LSN assignment + fwrite + fsync must be atomic.
  mutable std::mutex wal_mutex_;
};

} // namespace minidb
