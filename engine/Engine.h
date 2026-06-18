#pragma once

#include "BufferPool.h"
#include "DiskManager.h"
#include "Index.h"
#include "LogManager.h"
#include "RecoveryManager.h"
#include "BloomFilter.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <string>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

struct BufferPoolStats {
  size_t size = 0;
  size_t capacity = 0;
  uint64_t total_reads = 0;
  uint64_t cache_hits = 0;
  double hit_rate = 0.0;
  std::vector<FrameState> frames;
};

// Engine is the public facade for the key-value database.
// It wires together WAL, recovery, buffer pool, disk, and index.
class Engine {
public:
  Engine(const std::string &db_path, const std::string &wal_path,
         size_t buffer_pool_size);

  ~Engine();

  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  // Insert or update a key-value pair.
  // If sync is false, the WAL is NOT forced to disk (fast but unsafe unless
  // sync_wal() is called later).
  void put(const std::string &key, const std::string &value, bool sync = true);

  // Delete a key-value pair, freeing the page for reuse.
  void del(const std::string &key, bool sync = true);

  [[nodiscard]] std::string get(const std::string &key);

  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  scan(const std::string &start_key, const std::string &end_key);

  // Run crash recovery.
  RecoveryResult recover();

  // Test hook: write an ACTIVE log record, apply the page, flush it,
  // then crash before COMMIT so recovery must UNDO it.
  void test_put_and_crash_before_commit(const std::string &key,
                                        const std::string &value);

  // Force all WAL records to physical storage. Used for group commit.
  void sync_wal();

  // Flush dirty buffer-pool pages to disk. Used by the no-index scan demo.
  void flush();

  // Expose Bloom Filter for the API layer to block full scans
  [[nodiscard]] bool bloom_check(const std::string &key) const;

  // Flush buffer pool and disk, write CHECKPOINT to WAL
  void force_checkpoint();

  // Wipe the database
  void wipe_database();

  // Returns the number of keys in the index.
  [[nodiscard]] size_t index_size() const;

  [[nodiscard]] size_t get_wal_size() const;
  [[nodiscard]] BufferPoolStats get_buffer_pool_stats() const;
  [[nodiscard]] std::vector<LogRecord> get_wal_tail_records(int count) const;
  [[nodiscard]] std::string recovery_log() const;

  // Concurrency Demo Methods
  std::string slow_read_demo(const std::string &key);
  void slow_write_demo(const std::string &key, const std::string &value);

private:
  DiskManager disk_;
  BufferPool buffer_pool_;
  LogManager log_;
  RecoveryManager recovery_;
  SparseIndex index_;
  BloomFilter bloom_;
  RecoveryResult last_recovery_result_;

  uint64_t next_txn_id_;
  page_id_t next_page_id_;
  std::queue<page_id_t> free_pages_;

  mutable std::shared_mutex engine_mutex_;

  page_id_t allocate_page();

  void rebuild_index_from_disk();

  [[nodiscard]] std::string read_value_from_page(page_id_t page_id);

  [[nodiscard]] static std::array<char, PAGE_SIZE>
  make_page(const std::string &key, const std::string &value);

  [[nodiscard]] static std::pair<std::string, std::string>
  parse_page(const std::array<char, PAGE_SIZE> &page);
};

} // namespace minidb
