#include "Engine.h"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#ifndef _WIN32
#include <unistd.h>
#endif
#include <thread>

namespace minidb {

namespace {

void copy_fixed(char *dest, size_t capacity, const std::string &text) {
  const size_t len = std::min(text.size(), capacity - 1);
  std::memcpy(dest, text.data(), len);
}

std::string fixed_string(const char *begin, size_t capacity) {
  const char *end = std::find(begin, begin + capacity, '\0');
  return std::string(begin, end);
}

} // namespace

Engine::Engine(const std::string &db_path, const std::string &wal_path,
               size_t buffer_pool_size)
    : disk_(db_path), buffer_pool_(buffer_pool_size), log_(wal_path),
      recovery_(disk_, log_), index_(), next_txn_id_(1),
      next_page_id_(disk_.num_pages()) {
  buffer_pool_.set_flush_callback([this](page_id_t page_id, const char *data) {
    disk_.write_page(page_id, data);
  });
  buffer_pool_.set_load_callback([this](page_id_t page_id, char *dest) {
    disk_.read_page(page_id, dest);
  });

  // Recovery runs automatically on boot (before accepting any requests).
  // This ensures the .db file is in a consistent state before we
  // rebuild the in-memory index from it.
  last_recovery_result_ = recovery_.recover();
  rebuild_index_from_disk();
}

Engine::~Engine() { buffer_pool_.flush_all(); }

void Engine::put(const std::string &key, const std::string &value, bool sync) {
  if (key.empty()) {
    throw std::invalid_argument("Engine::put: key must not be empty");
  }

  std::unique_lock lock(engine_mutex_);

  auto existing_page = index_.get(key);
  page_id_t page_id =
      existing_page ? static_cast<page_id_t>(*existing_page) : allocate_page();

  const std::string old_value =
      existing_page ? read_value_from_page(page_id) : "";

  const uint64_t txn_id = next_txn_id_++;

  // WAL rule: durable log record first, then modify the buffer pool.
  log_.log_write(txn_id, page_id, key, old_value, value,
                 false); // Don't sync individual data writes
  buffer_pool_.write_page_copy(page_id, make_page(key, value));
  index_.put(key, page_id);
  bloom_.add(key); // Add to Bloom Filter
  log_.log_commit(txn_id, sync); // Sync at commit if requested
}

std::string Engine::get(const std::string &key) {
  std::shared_lock lock(engine_mutex_);

  auto page_id = index_.get(key);
  if (!page_id) {
    return "";
  }

  return read_value_from_page(static_cast<page_id_t>(*page_id));
}

void Engine::del(const std::string &key, bool sync) {
  if (key.empty()) {
    throw std::invalid_argument("Engine::del: key must not be empty");
  }

  std::unique_lock lock(engine_mutex_);

  auto existing_page = index_.get(key);
  if (!existing_page) return;

  page_id_t page_id = static_cast<page_id_t>(*existing_page);
  const std::string old_value = read_value_from_page(page_id);

  const uint64_t txn_id = next_txn_id_++;

  // Log delete: new_val is empty, key is stored for safe UNDO.
  log_.log_write(txn_id, page_id, key, old_value, "", false);
  
  // Zero out page contents
  std::array<char, PAGE_SIZE> zero_page{};
  buffer_pool_.write_page_copy(page_id, zero_page);
  
  index_.remove(key);
  free_pages_.push(page_id);

  log_.log_commit(txn_id, sync);
}

std::vector<std::pair<std::string, std::string>>
Engine::scan(const std::string &start_key, const std::string &end_key) {
  std::shared_lock lock(engine_mutex_);

  std::vector<std::pair<std::string, std::string>> result;
  for (const auto &[key, page_id] : index_.scan_range(start_key, end_key)) {
    result.push_back(
        {key, read_value_from_page(static_cast<page_id_t>(page_id))});
  }

  return result;
}

RecoveryResult Engine::recover() {
  std::unique_lock lock(engine_mutex_);
  last_recovery_result_ = recovery_.recover();
  rebuild_index_from_disk();
  return last_recovery_result_;
}

void Engine::test_put_and_crash_before_commit(const std::string &key,
                                              const std::string &value) {
  if (key.empty()) {
    throw std::invalid_argument(
        "Engine::test_put_and_crash_before_commit: key must not be empty");
  }

  std::unique_lock lock(engine_mutex_);

  auto existing_page = index_.get(key);
  page_id_t page_id =
      existing_page ? static_cast<page_id_t>(*existing_page) : allocate_page();

  const std::string old_value =
      existing_page ? read_value_from_page(page_id) : "";

  const uint64_t txn_id = next_txn_id_++;

  log_.log_write(txn_id, page_id, key, old_value, value, true);
  buffer_pool_.write_page_copy(page_id, make_page(key, value));
  index_.put(key, page_id);

  // Force the uncommitted page to disk so the next boot visibly UNDOs it.
  // This demonstrates the immediate-update/steal recovery case.
  buffer_pool_.flush_all();
  disk_.sync();

#ifdef _WIN32
  std::abort();
#else
  ::kill(::getpid(), SIGKILL);
#endif
}

void Engine::sync_wal() {
  std::unique_lock lock(engine_mutex_);
  log_.force_sync();
}

void Engine::flush() {
  std::unique_lock lock(engine_mutex_);
  buffer_pool_.flush_all();
  disk_.sync();
}

bool Engine::bloom_check(const std::string &key) const {
  std::shared_lock lock(engine_mutex_);
  return bloom_.might_contain(key);
}

void Engine::force_checkpoint() {
    std::unique_lock lock(engine_mutex_);
    buffer_pool_.flush_all();
    disk_.sync();
    log_.log_checkpoint();
    
    // Rebuild Bloom Filter to remove stale (deleted) keys
    bloom_.clear();
    for (const auto& key : index_.get_all_keys()) {
        bloom_.add(key);
    }
}

void Engine::wipe_database() {
    std::unique_lock lock(engine_mutex_);
    buffer_pool_.clear();
    disk_.truncate();
    log_.truncate();
    index_.clear();
    bloom_.clear();
    std::queue<page_id_t> empty_q;
    std::swap(free_pages_, empty_q);
    next_txn_id_ = 1;
    next_page_id_ = 0;
    last_recovery_result_ = RecoveryResult{0, 0, 0, 0, 0, "[WIPED] Database factory reset."};
}

size_t Engine::index_size() const {
  std::shared_lock lock(engine_mutex_);
  return index_.size();
}

size_t Engine::get_wal_size() const {
  std::shared_lock lock(engine_mutex_);
  std::error_code ec;
  auto path = log_.file_path();
  if (!std::filesystem::exists(path, ec)) {
    return 0;
  }
  auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return 0;
  }
  return static_cast<size_t>(size);
}

BufferPoolStats Engine::get_buffer_pool_stats() const {
  std::shared_lock lock(engine_mutex_);
  BufferPoolStats stats;
  stats.size = buffer_pool_.get_size();
  stats.capacity = buffer_pool_.get_capacity();
  stats.total_reads = buffer_pool_.total_reads();
  stats.cache_hits = buffer_pool_.cache_hits();
  stats.hit_rate = stats.total_reads == 0
                       ? 0.0
                       : (static_cast<double>(stats.cache_hits) /
                          static_cast<double>(stats.total_reads));
  stats.frames = buffer_pool_.get_frame_states();
  return stats;
}

std::vector<LogRecord> Engine::get_wal_tail_records(int count) const {
  std::shared_lock lock(engine_mutex_);
  return log_.get_tail_records(count);
}

std::string Engine::recovery_log() const {
  std::shared_lock lock(engine_mutex_);
  return last_recovery_result_.recovery_log_str;
}

std::string Engine::slow_read_demo(const std::string &key) {
  std::shared_lock lock(engine_mutex_);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  auto page_id = index_.get(key);
  if (!page_id) return "";
  return read_value_from_page(static_cast<page_id_t>(*page_id));
}

void Engine::slow_write_demo(const std::string &key, const std::string &value) {
  std::unique_lock lock(engine_mutex_);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  
  auto existing_page = index_.get(key);
  page_id_t page_id = existing_page ? static_cast<page_id_t>(*existing_page) : allocate_page();
  const std::string old_value = existing_page ? read_value_from_page(page_id) : "";
  const uint64_t txn_id = next_txn_id_++;
  
  log_.log_write(txn_id, page_id, key, old_value, value, false);
  buffer_pool_.write_page_copy(page_id, make_page(key, value));
  index_.put(key, page_id);
  bloom_.add(key);
  log_.log_commit(txn_id, true);
}

page_id_t Engine::allocate_page() {
  if (!free_pages_.empty()) {
    page_id_t pid = free_pages_.front();
    free_pages_.pop();
    return pid;
  }
  return next_page_id_++;
}

void Engine::rebuild_index_from_disk() {
  index_.clear();

  const uint32_t pages = disk_.num_pages();
  next_page_id_ = pages;

  for (uint32_t i = 0; i < disk_.num_pages(); ++i) {
    std::array<char, PAGE_SIZE> page_data{};
    disk_.read_page(i, page_data.data());
    auto [key, value] = parse_page(page_data);

    if (!key.empty() && key[0] != '\0') {
      index_.put(key, i);
      bloom_.add(key);
    } else {
      free_pages_.push(i);
    }
  }
}

std::string Engine::read_value_from_page(page_id_t page_id) {
  auto page = buffer_pool_.read_page_copy(page_id);
  return parse_page(page).second;
}

std::array<char, PAGE_SIZE> Engine::make_page(const std::string &key,
                                              const std::string &value) {
  std::array<char, PAGE_SIZE> page{};
  copy_fixed(page.data() + PAGE_KEY_OFFSET, LogRecord::KEY_SIZE, key);
  copy_fixed(page.data() + PAGE_VAL_OFFSET, LogRecord::VAL_SIZE, value);
  return page;
}

std::pair<std::string, std::string>
Engine::parse_page(const std::array<char, PAGE_SIZE> &page) {
  const std::string key =
      fixed_string(page.data() + PAGE_KEY_OFFSET, LogRecord::KEY_SIZE);
  const std::string value =
      fixed_string(page.data() + PAGE_VAL_OFFSET, LogRecord::VAL_SIZE);
  return {key, value};
}

} // namespace minidb
