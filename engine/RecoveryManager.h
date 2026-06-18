#pragma once

#include "DiskManager.h"
#include "LogManager.h"

#include <cstdint>
#include <string>

namespace minidb {

// ─────────────────────────────────────────────
//  Page Layout for Key-Value Storage
// ─────────────────────────────────────────────
//
// Each 4 KB page stores a single key-value pair at fixed offsets:
//
//   Offset 0:    key   (64 bytes, zero-padded)
//   Offset 64:   value (128 bytes, zero-padded)
//   Offset 192:  unused (3904 bytes, zero)
//
// This layout matches LogRecord::KEY_SIZE and LogRecord::VAL_SIZE
// so that recovery can directly copy key/val from log records
// into pages. In Step 6, the Engine facade will use this same
// layout for all put/get operations.

static constexpr size_t PAGE_KEY_OFFSET = 0;
static constexpr size_t PAGE_VAL_OFFSET = LogRecord::KEY_SIZE; // 64

// ─────────────────────────────────────────────
//  RecoveryResult — statistics from recovery
// ─────────────────────────────────────────────

struct RecoveryResult {
  int records_scanned = 0;      // Total WAL records read
  int redo_count = 0;           // Committed records replayed
  int undo_count = 0;           // Uncommitted records reversed
  int committed_txns = 0;       // Number of committed transactions
  int aborted_txns = 0;         // Number of uncommitted transactions
  std::string recovery_log_str; // Human-readable recovery summary for UI
};

// ─────────────────────────────────────────────
//  RecoveryManager — crash recovery state machine
// ─────────────────────────────────────────────
//
// Runs on every engine boot BEFORE accepting any requests.
// Scans the WAL and restores the database to a consistent state:
//
//   Phase 1 — ANALYSIS:
//     Scan all records. Identify committed vs. uncommitted txns.
//     Find the last CHECKPOINT to skip already-recovered records.
//
//   Phase 2 — REDO (forward scan):
//     For every ACTIVE record belonging to a committed txn:
//     apply new_val to the data page. This ensures committed
//     changes survive even if the dirty page wasn't flushed.
//
//   Phase 3 — UNDO (backward scan):
//     For every ACTIVE record belonging to an uncommitted txn:
//     apply old_val to the data page. This reverses changes
//     that were flushed to disk before the txn committed.
//     Backward scan ensures correct ordering when a txn made
//     multiple writes to the same page.
//
//   Phase 4 — CHECKPOINT:
//     Write a CHECKPOINT record to the WAL so future recovery
//     can skip these already-processed records.
//
// Real-world equivalent: PostgreSQL's startup recovery,
// InnoDB crash recovery, SQLite's rollback journal replay.
// The gold standard is ARIES (Algorithm for Recovery and
// Isolation Exploiting Semantics), used by IBM DB2 and
// SQL Server. Our approach is a simplified ARIES variant.

class RecoveryManager {
public:
  // Construct with references to the disk and WAL managers.
  // These must outlive the RecoveryManager.
  RecoveryManager(DiskManager &disk, LogManager &wal);

  // Non-copyable (holds references).
  RecoveryManager(const RecoveryManager &) = delete;
  RecoveryManager &operator=(const RecoveryManager &) = delete;

  // ── Core API ─────────────────────────────

  // Run the full recovery sequence.
  // Must be called ONCE on engine boot, before any user operations.
  // Returns statistics about what was recovered.
  RecoveryResult recover();

private:
  DiskManager &disk_;
  LogManager &wal_;

  // Apply a committed write: write key + new_val to the page.
  void redo(const LogRecord &record);

  // Reverse an uncommitted write: restore old_val (or zero page for new
  // inserts).
  void undo(const LogRecord &record);
};

} // namespace minidb
