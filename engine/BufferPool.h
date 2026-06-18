#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace minidb {

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────

static constexpr size_t PAGE_SIZE = 4096;
using page_id_t = uint32_t;
static constexpr page_id_t INVALID_PAGE_ID =
    std::numeric_limits<page_id_t>::max();

struct FrameState {
  page_id_t page_id;
  bool dirty;
  bool valid;
};

// ─────────────────────────────────────────────
//  Frame — one slot in the buffer pool
// ─────────────────────────────────────────────
// A Frame represents a single fixed-size memory slot that can hold
// one disk page at a time. Metadata (page_id, dirty bit) is stored
// alongside the raw page bytes for simplicity at this scale.

struct Frame {
  page_id_t page_id = INVALID_PAGE_ID;
  bool dirty = false;

  // Fixed-size page buffer — no heap allocation, RAII-safe.
  // Zero-initialized on construction.
  std::array<char, PAGE_SIZE> data{};

  // Reset frame to empty state (e.g., after eviction flush).
  void reset() {
    page_id = INVALID_PAGE_ID;
    dirty = false;
    data.fill(0);
  }
};

// ─────────────────────────────────────────────
//  BufferPool — LRU-managed page cache
// ─────────────────────────────────────────────
// Manages a fixed number of Frames. On a page miss:
//   1. If a free frame exists -> claim it and load the page.
//   2. Otherwise             -> evict the LRU victim and reuse it.
//
// Step 5 safety rule:
//   Page bytes are copied in/out while the buffer pool lock is held.
//   Callers never keep a raw pointer into a frame after the lock is
//   released, so concurrent threads cannot race on frame memory.

class BufferPool {
public:
  // Type alias for the eviction callback.
  // Called with (page_id, data_ptr) when a dirty frame is evicted.
  using FlushCallback = std::function<void(page_id_t, const char *)>;

  // Type alias for the page-load callback.
  // Called with (page_id, dest_ptr) on a cache miss to load from disk.
  using LoadCallback = std::function<void(page_id_t, char *)>;

  // Constructs a buffer pool with `capacity` frames.
  // All frames are pre-allocated; no further heap allocation occurs.
  explicit BufferPool(size_t capacity);

  // Non-copyable, non-movable (owns complex internal state).
  BufferPool(const BufferPool &) = delete;
  BufferPool &operator=(const BufferPool &) = delete;

  // ── Core API ──────────────────────────────

  // Read a page safely by returning a copy of its bytes.
  // If the page is not cached, it is loaded from disk first.
  [[nodiscard]] std::array<char, PAGE_SIZE> read_page_copy(page_id_t page_id);

  // Check whether a page is currently resident in the buffer pool.
  [[nodiscard]] bool contains(page_id_t page_id) const;

  // Read a cached page safely without loading from disk.
  // Returns std::nullopt if the page is not currently resident.
  [[nodiscard]] std::optional<std::array<char, PAGE_SIZE>>
  get_page_copy(page_id_t page_id) const;

  // Replace a page with a full 4KB copy and mark it dirty.
  // If the page is not cached, it is loaded first so the LRU state
  // and eviction rules stay consistent.
  void write_page_copy(page_id_t page_id,
                       const std::array<char, PAGE_SIZE> &page_data);

  // Flush all dirty pages via the flush callback.
  void flush_all();

  // Wipe the buffer pool back to its initial empty state.
  void clear();

  // ── Configuration ─────────────────────────

  // Set the callback invoked when a dirty page is evicted.
  // Wired to DiskManager::write_page().
  void set_flush_callback(FlushCallback cb);

  // Set the callback invoked on a cache miss to load a page from disk.
  // Wired to DiskManager::read_page().
  void set_load_callback(LoadCallback cb);

  // ── Accessors ─────────────────────────────

  [[nodiscard]] size_t capacity() const { return capacity_; }
  [[nodiscard]] size_t size() const;
  [[nodiscard]] size_t get_capacity() const { return capacity_; }
  [[nodiscard]] size_t get_size() const { return size(); }
  [[nodiscard]] uint64_t total_reads() const { return total_reads_.load(); }
  [[nodiscard]] uint64_t cache_hits() const { return cache_hits_.load(); }
  [[nodiscard]] std::vector<FrameState> get_frame_states() const;

private:
  // Ensure a page is loaded and return its frame index.
  // Caller must hold pool_latch_ exclusively.
  size_t load_page_locked(page_id_t page_id);

  // Evict the LRU victim. Returns the freed frame index.
  // Calls flush_callback_ if the victim frame is dirty.
  size_t evict_lru();

  // Promote a page to Most Recently Used (front of LRU list).
  void promote(page_id_t page_id);

  // ── Data members ──────────────────────────

  size_t capacity_;

  // Pre-allocated frame storage. Index into this vector = "frame_id".
  std::vector<Frame> frames_;

  // Free list: indices of frames not currently holding a page.
  // Populated at construction, drained as pages are fetched.
  std::queue<size_t> free_list_;

  // Page table: maps page_id -> frame index in frames_.
  // This is the "directory" that answers "is this page cached?"
  std::unordered_map<page_id_t, size_t> page_table_;

  // LRU list: front = Most Recently Used, back = Least Recently Used.
  // Contains page_ids of all cached pages.
  std::list<page_id_t> lru_list_;

  // LRU map: maps page_id -> iterator into lru_list_ for O(1) splice.
  std::unordered_map<page_id_t, std::list<page_id_t>::iterator> lru_map_;

  // Callback invoked when a dirty frame is evicted.
  FlushCallback flush_callback_;

  // Callback invoked on cache miss to load page data from disk.
  LoadCallback load_callback_;

  // ── Concurrency (Step 5) ─────────────────
  // Reader-writer latch protecting all internal state and page bytes.
  // Shared lock: contains(), get_page_copy(), size()
  // Exclusive lock: read_page_copy() when it touches LRU state,
  // write_page_copy(), flush_all(), callback configuration.
  mutable std::shared_mutex pool_latch_;

  // Metrics used by the web demo. Relaxed atomics are enough because
  // these counters are approximate observability values, not correctness state.
  mutable std::atomic<uint64_t> total_reads_{0};
  mutable std::atomic<uint64_t> cache_hits_{0};
};

} // namespace minidb
