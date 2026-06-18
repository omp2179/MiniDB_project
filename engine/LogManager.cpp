#include "LogManager.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

// Platform-specific includes for fsync / _commit.
#ifdef _WIN32
#include <cstdio> // _fileno
#include <io.h>   // _fileno, _commit
#else
#include <unistd.h> // fsync, fileno
#endif

namespace minidb {

// ═══════════════════════════════════════════════
//  CRC32 — integrity checksum for WAL records
// ═══════════════════════════════════════════════
//
// We use the standard CRC32 algorithm (polynomial 0xEDB88320,
// the bit-reversed form of 0x04C11DB7) used by Ethernet, gzip,
// PNG, and most database systems. It detects:
//   - All single-bit errors
//   - All double-bit errors
//   - All burst errors up to 32 bits
//
// For a 341-byte WAL record, this provides excellent protection
// against partial writes and bit-flip corruption.

namespace {

// Pre-computed CRC32 lookup table (256 entries).
// Computed once on first use via ensure_crc32_table().
uint32_t g_crc32_table[256];
bool g_crc32_initialized = false;

void ensure_crc32_table() {
  if (g_crc32_initialized)
    return;
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int j = 0; j < 8; ++j) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320u;
      else
        crc >>= 1;
    }
    g_crc32_table[i] = crc;
  }
  g_crc32_initialized = true;
}

uint32_t compute_crc32(const uint8_t *data, size_t length) {
  ensure_crc32_table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc = (crc >> 8) ^ g_crc32_table[(crc ^ data[i]) & 0xFF];
  }
  return crc ^ 0xFFFFFFFFu;
}

} // anonymous namespace

// ─────────────────────────────────────────────
//  LogRecord helpers
// ─────────────────────────────────────────────

uint32_t LogRecord::compute_checksum() const {
  // Checksum covers everything except the checksum field itself.
  // Since checksum is the last 4 bytes (packed struct), we
  // compute over sizeof(LogRecord) - sizeof(checksum) = 341 bytes.
  constexpr size_t data_size = sizeof(LogRecord) - sizeof(checksum);
  return compute_crc32(reinterpret_cast<const uint8_t *>(this), data_size);
}

bool LogRecord::verify_checksum() const {
  return checksum == compute_checksum();
}

std::string LogRecord::key_str() const {
  // Find the first null byte or use KEY_SIZE as the length.
  const char *end = std::find(key, key + KEY_SIZE, '\0');
  return std::string(key, end);
}

std::string LogRecord::old_val_str() const {
  const char *end = std::find(old_val, old_val + VAL_SIZE, '\0');
  return std::string(old_val, end);
}

std::string LogRecord::new_val_str() const {
  const char *end = std::find(new_val, new_val + VAL_SIZE, '\0');
  return std::string(new_val, end);
}

// ─────────────────────────────────────────────
//  Construction & Destruction
// ─────────────────────────────────────────────
//
// On construction:
//   1. Scan existing WAL records to determine the next LSN.
//      This handles the case where we're recovering after a crash.
//   2. Open the file in append-binary mode for writing new records.

LogManager::LogManager(const std::string &wal_path)
    : wal_path_(wal_path), file_(nullptr), next_lsn_(1) {
  // Phase 1: Read existing records to find the highest LSN.
  // (read_all_records opens its own file handle for reading.)
  auto existing = read_all_records();
  if (!existing.empty()) {
    next_lsn_ = existing.back().lsn + 1;
  }

  // Phase 2: Open in append-binary mode for writing new records.
  // "ab" = append + binary:
  //   - All writes go to the end of the file (append)
  //   - No newline translation (binary)
  //   - Creates the file if it doesn't exist
  file_ = std::fopen(wal_path_.c_str(), "ab");
  if (!file_) {
    throw std::runtime_error("LogManager: failed to open WAL file: " +
                             wal_path_);
  }
}

LogManager::~LogManager() {
  if (file_) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

// ─────────────────────────────────────────────
//  log_write — record a data modification
// ─────────────────────────────────────────────
//
// This MUST be called BEFORE the buffer pool frame is modified.
// The WAL protocol requires:
//   1. Construct the log record with old_val and new_val
//   2. Append it to the WAL file
//   3. fsync the WAL to stable storage
//   4. ONLY THEN modify the buffer pool frame
//
// If we crash between steps 3 and 4, recovery can REDO the write
// (the log has new_val but the buffer/disk may not).
// If we crash before step 3, the record isn't durable and the
// buffer wasn't modified — consistent state, nothing to fix.

uint64_t LogManager::log_write(uint64_t txn_id, uint32_t page_id,
                               const std::string &key,
                               const std::string &old_val,
                               const std::string &new_val, bool sync) {
  std::lock_guard lock(wal_mutex_); // Thread-safe: LSN + fwrite + fsync atomic.
  LogRecord record{};               // Zero-initialize all fields.

  record.lsn = next_lsn_;
  record.txn_id = txn_id;
  record.page_id = page_id;
  record.status = LogRecord::ACTIVE;

  // Copy key into fixed-size field (truncate if too long, zero-pad).
  size_t key_len =
      std::min(key.size(), static_cast<size_t>(LogRecord::KEY_SIZE - 1));
  std::memcpy(record.key, key.data(), key_len);
  // Remaining bytes are already zero from zero-initialization.

  // Copy old and new values into fixed-size fields.
  size_t old_len =
      std::min(old_val.size(), static_cast<size_t>(LogRecord::VAL_SIZE - 1));
  std::memcpy(record.old_val, old_val.data(), old_len);

  size_t new_len =
      std::min(new_val.size(), static_cast<size_t>(LogRecord::VAL_SIZE - 1));
  std::memcpy(record.new_val, new_val.data(), new_len);

  append_record(record, sync);
  return record.lsn;
}

// ─────────────────────────────────────────────
//  log_commit — mark a transaction as committed
// ─────────────────────────────────────────────
//
// A commit record means: all modifications for this txn_id are
// final and must survive crashes. During recovery:
//   - If a COMMIT record exists → REDO all the txn's writes
//   - If no COMMIT record       → UNDO all the txn's writes

uint64_t LogManager::log_commit(uint64_t txn_id, bool sync) {
  std::lock_guard lock(wal_mutex_); // Thread-safe.
  LogRecord record{};
  record.lsn = next_lsn_;
  record.txn_id = txn_id;
  record.status = LogRecord::COMMIT;
  // page_id, key, old_val, new_val are zeros (not used for COMMIT).

  append_record(record, sync);
  return record.lsn;
}

// ─────────────────────────────────────────────
//  log_checkpoint — mark a recovery checkpoint
// ─────────────────────────────────────────────

uint64_t LogManager::log_checkpoint() {
  std::lock_guard lock(wal_mutex_); // Thread-safe.
  LogRecord record{};
  record.lsn = next_lsn_;
  record.txn_id = 0;
  record.status = LogRecord::CHECKPOINT;

  append_record(record, true);
  return record.lsn;
}

// ─────────────────────────────────────────────
//  read_all_records — scan the WAL for recovery
// ─────────────────────────────────────────────
//
// Opens a separate read-only file handle and reads records
// sequentially. Records with invalid CRC32 are skipped —
// they represent partial writes from a crash mid-record.
//
// This is a const method: it does not modify the LogManager state.

std::vector<LogRecord> LogManager::read_all_records() const {
  FILE *rf = std::fopen(wal_path_.c_str(), "rb");
  if (!rf) {
    // File doesn't exist yet — no records.
    return {};
  }

  std::vector<LogRecord> records;
  LogRecord record;

  while (std::fread(&record, sizeof(LogRecord), 1, rf) == 1) {
    if (record.verify_checksum()) {
      records.push_back(record);
    }
    // If checksum is invalid, the record was a partial write
    // (crash happened mid-write). We stop here because all
    // subsequent data is unreliable.
    // Note: we don't break — we continue scanning in case the
    // invalid record was a single corruption in an otherwise
    // intact log. A production system would stop at the first
    // bad record (the log is append-only, so anything after a
    // bad record is suspect).
  }

  std::fclose(rf);
  return records;
}

// ─────────────────────────────────────────────
//  get_tail_records — read the newest WAL records
// ─────────────────────────────────────────────

std::vector<LogRecord> LogManager::get_tail_records(int count) const {
  std::lock_guard lock(wal_mutex_);
  if (count <= 0) {
    return {};
  }
  if (file_) {
    std::fflush(file_);
  }

  auto records = read_all_records();
  if (static_cast<int>(records.size()) <= count) {
    return records;
  }

  return std::vector<LogRecord>(records.end() - count, records.end());
}

// ─────────────────────────────────────────────
//  truncate — clear the WAL after checkpoint
// ─────────────────────────────────────────────

void LogManager::truncate() {
  std::lock_guard lock(wal_mutex_); // Thread-safe.
  // Close the current append handle.
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }

  // Reopen in "wb" mode which truncates the file to zero length.
  FILE *trunc = std::fopen(wal_path_.c_str(), "wb");
  if (trunc) {
    std::fclose(trunc);
  }

  // Reopen in append mode for future writes.
  file_ = std::fopen(wal_path_.c_str(), "ab");
  next_lsn_ = 1;
}

// ─────────────────────────────────────────────
//  append_record — the atomic write path
// ─────────────────────────────────────────────
//
// Steps:
//   1. Assign LSN and compute CRC32 checksum.
//   2. Write the entire record with a single fwrite() call.
//   3. Force to stable storage with fflush() + fsync().
//   4. Increment the LSN counter.
//
// Why a single fwrite()? The record is 345 bytes, which is smaller
// than the filesystem block size (4096 bytes). A single write of
// this size is typically atomic at the filesystem level — it either
// fully succeeds or doesn't appear at all. The CRC32 checksum is
// our second line of defense: even if the OS writes a partial record,
// recovery will detect the bad checksum and discard it.

void LogManager::append_record(LogRecord &record, bool sync) {
  assert(file_ != nullptr);

  // Compute the integrity checksum over all fields except checksum.
  record.checksum = record.compute_checksum();

  // Write the entire record in one call.
  size_t written = std::fwrite(&record, sizeof(LogRecord), 1, file_);
  if (written != 1) {
    throw std::runtime_error("LogManager::append_record: fwrite failed");
  }

  // Force to stable storage if requested — this is the critical durability
  // step.
  if (sync) {
    force_sync_unlocked();
  }

  // Advance the LSN for the next record.
  ++next_lsn_;
}

// ─────────────────────────────────────────────
//  force_sync — fsync the WAL to physical media
// ─────────────────────────────────────────────
//
// Two-phase flush:
//   1. fflush(): C library → OS kernel buffer
//   2. fsync()/_commit(): OS kernel → physical disk
//
// Without fsync, a power failure can lose data that the OS has
// "accepted" (sitting in the kernel's page cache) but not yet
// written to the physical drive.

void LogManager::force_sync() {
  std::lock_guard lock(wal_mutex_);
  force_sync_unlocked();
}

void LogManager::force_sync_unlocked() {
  if (!file_)
    return;

  // Phase 1: Flush C library buffers to the OS.
  std::fflush(file_);

  // Phase 2: Force OS kernel buffers to physical storage.
#ifdef _WIN32
  // Windows / MinGW: _commit() is the fsync equivalent.
  int fd = _fileno(file_);
  if (fd >= 0) {
    _commit(fd);
  }
#else
  // POSIX: fsync() forces data to disk.
  int fd = fileno(file_);
  if (fd >= 0) {
    fsync(fd);
  }
#endif
}

} // namespace minidb
