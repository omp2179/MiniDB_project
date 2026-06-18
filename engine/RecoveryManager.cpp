#include "RecoveryManager.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace minidb {

// ─────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────

RecoveryManager::RecoveryManager(DiskManager &disk, LogManager &wal)
    : disk_(disk), wal_(wal) {}

// ─────────────────────────────────────────────
//  recover — the main recovery entry point
// ─────────────────────────────────────────────
//
// This method implements a simplified ARIES-style recovery
// with three phases:
//
//   Phase 1 — ANALYSIS
//     Read all WAL records. Build the set of committed txn_ids.
//     Find the last CHECKPOINT to skip already-recovered data.
//     Any txn that has ACTIVE records but no COMMIT is "loser"
//     (uncommitted) and must be undone.
//
//   Phase 2 — REDO (forward scan)
//     Scan records FORWARD from the checkpoint. For each data
//     record (ACTIVE) whose txn committed: apply new_val to the
//     page on disk. This guarantees that all committed writes are
//     present in the .db file, even if the dirty page was never
//     flushed (was still in the buffer pool at crash time).
//
//     Why forward? REDO replays history in the order it happened.
//     If a txn wrote to page 5 twice, the second write's new_val
//     is the final correct value — forward order naturally gives us
//     the latest version.
//
//   Phase 3 — UNDO (backward scan)
//     Scan records BACKWARD from the end. For each data record
//     whose txn did NOT commit: apply old_val to the page on disk.
//     This reverses any uncommitted changes that may have been
//     flushed to disk by the buffer pool before the crash.
//
//     Why backward? If a txn wrote to the same page multiple times:
//       Write 1: old_val="",      new_val="hello"
//       Write 2: old_val="hello", new_val="world"
//     Backward undo:
//       Undo Write 2: restore "hello"  ← correct intermediate state
//       Undo Write 1: restore ""       ← correct original state ✅
//     Forward undo:
//       Undo Write 1: restore ""       ← skips over "hello"
//       Undo Write 2: restore "hello"  ← WRONG! Original was "" ❌
//
//   Phase 4 — CHECKPOINT
//     Write a CHECKPOINT record so the next boot can skip these
//     already-recovered records, reducing recovery time from
//     O(total log size) to O(log since last checkpoint).

RecoveryResult RecoveryManager::recover() {
  RecoveryResult result;

  // ── Phase 1: ANALYSIS ────────────────────

  auto records = wal_.read_all_records();
  if (records.empty()) {
    result.recovery_log_str = "[RECOVERY] No WAL records found";
    return result; // Nothing to recover.
  }

  result.records_scanned = static_cast<int>(records.size());

  // Find the last CHECKPOINT — we only need to process records after it.
  // Records before the checkpoint were already recovered in a prior boot.
  int start_idx = 0;
  for (int i = static_cast<int>(records.size()) - 1; i >= 0; --i) {
    if (records[i].status == LogRecord::CHECKPOINT) {
      start_idx = i + 1; // Start processing AFTER the checkpoint.
      break;
    }
  }

  // Build the set of committed transaction IDs.
  // A txn is committed if ANY record with its txn_id has status=COMMIT.
  std::unordered_set<uint64_t> committed_txns;
  // Also track all txns that have data (ACTIVE) records.
  std::unordered_set<uint64_t> active_txns;

  for (size_t i = start_idx; i < records.size(); ++i) {
    const auto &rec = records[i];
    if (rec.status == LogRecord::COMMIT) {
      committed_txns.insert(rec.txn_id);
    } else if (rec.status == LogRecord::ACTIVE) {
      active_txns.insert(rec.txn_id);
    }
  }

  // Determine "loser" (uncommitted) transactions.
  std::unordered_set<uint64_t> loser_txns;
  for (uint64_t txn_id : active_txns) {
    if (committed_txns.find(txn_id) == committed_txns.end()) {
      loser_txns.insert(txn_id);
    }
  }

  result.committed_txns = static_cast<int>(committed_txns.size());
  result.aborted_txns = static_cast<int>(loser_txns.size());

  // ── Phase 2: REDO (forward scan) ─────────
  // Replay committed transactions to ensure all their changes
  // are present on disk.

  for (size_t i = start_idx; i < records.size(); ++i) {
    const auto &rec = records[i];
    if (rec.status == LogRecord::ACTIVE && committed_txns.count(rec.txn_id)) {
      redo(rec);
      result.redo_count++;
    }
  }

  // ── Phase 3: UNDO (backward scan) ────────
  // Reverse uncommitted transactions to remove their changes
  // from disk. Backward scan ensures correct ordering.

  for (int i = static_cast<int>(records.size()) - 1; i >= start_idx; --i) {
    const auto &rec = records[i];
    if (rec.status == LogRecord::ACTIVE && loser_txns.count(rec.txn_id)) {
      undo(rec);
      result.undo_count++;
    }
  }

  // ── Phase 4: CHECKPOINT ──────────────────
  // Sync all recovered pages to disk, then write a checkpoint
  // so future recovery can skip these records.

  disk_.sync();
  wal_.log_checkpoint();

  std::ostringstream oss;
  oss << "[RECOVERY] Complete: " << result.redo_count << " REDO, "
      << result.undo_count << " UNDO, " << result.committed_txns
      << " committed, " << result.aborted_txns << " aborted";
  result.recovery_log_str = oss.str();
  std::cout << result.recovery_log_str << "\n";

  return result;
}

// ─────────────────────────────────────────────
//  redo — apply a committed write to disk
// ─────────────────────────────────────────────
//
// Reads the page, writes key + new_val, writes back.
//
// REDO is idempotent: applying the same record twice produces
// the same result. This is essential because we don't know
// whether the dirty page was flushed before the crash or not.
// If it was, REDO overwrites with the same data (no-op effect).
// If it wasn't, REDO applies the missing change.

void RecoveryManager::redo(const LogRecord &record) {
  char page[DISK_PAGE_SIZE] = {};
  disk_.read_page(record.page_id, page);

  std::string new_val = record.new_val_str();
  if (new_val.empty()) {
    // Was a delete -> zero out the entire page
    std::memset(page, 0, DISK_PAGE_SIZE);
  } else {
    // Write the key at offset 0 and new_val at offset KEY_SIZE.
    std::memcpy(page + PAGE_KEY_OFFSET, record.key, LogRecord::KEY_SIZE);
    std::memcpy(page + PAGE_VAL_OFFSET, record.new_val, LogRecord::VAL_SIZE);
  }

  disk_.write_page(record.page_id, page);
}

// ─────────────────────────────────────────────
//  undo — reverse an uncommitted write on disk
// ─────────────────────────────────────────────
//
// Two cases:
//   1. old_val is empty → this was a NEW INSERT that never committed.
//      The entire page should be zeroed (entry removed).
//   2. old_val is non-empty → this was an UPDATE. Restore old_val.
//
// Like REDO, UNDO is idempotent: applying it twice is safe.

void RecoveryManager::undo(const LogRecord &record) {
  char page[DISK_PAGE_SIZE] = {};
  disk_.read_page(record.page_id, page);

  std::string old_val = record.old_val_str();

  if (old_val.empty()) {
    // Was a new insert → delete by zeroing the entire page.
    // The key never existed before this transaction.
    std::memset(page, 0, DISK_PAGE_SIZE);
  } else {
    // Was an update or a delete → restore the previous key and value.
    std::memset(page + PAGE_KEY_OFFSET, 0, LogRecord::KEY_SIZE);
    std::memcpy(page + PAGE_KEY_OFFSET, record.key, LogRecord::KEY_SIZE);
    std::memset(page + PAGE_VAL_OFFSET, 0, LogRecord::VAL_SIZE);
    std::memcpy(page + PAGE_VAL_OFFSET, record.old_val, LogRecord::VAL_SIZE);
  }

  disk_.write_page(record.page_id, page);
}

} // namespace minidb
