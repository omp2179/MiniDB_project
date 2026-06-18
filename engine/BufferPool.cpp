#include "BufferPool.h"

#include <cassert>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace minidb {

// ─────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────

BufferPool::BufferPool(size_t capacity)
    : capacity_(capacity),
      frames_(capacity) // Pre-allocate all frames (zero-initialized).
      ,
      flush_callback_(nullptr), load_callback_(nullptr) {
  if (capacity == 0) {
    throw std::invalid_argument("BufferPool capacity must be > 0");
  }

  // Populate the free list with all frame indices.
  // After this, every frame is available for use.
  for (size_t i = 0; i < capacity; ++i) {
    free_list_.push(i);
  }
}

// ─────────────────────────────────────────────
//  read_page_copy — safe read with demand paging
// ─────────────────────────────────────────────
//
// A normal pointer-returning fetch is unsafe in a multithreaded buffer
// pool because the caller can keep using the pointer after the lock is
// released. Returning a copy keeps the protected frame memory private.

std::array<char, PAGE_SIZE> BufferPool::read_page_copy(page_id_t page_id) {
  total_reads_.fetch_add(1, std::memory_order_relaxed);

  std::unique_lock lock(pool_latch_); // May load, evict, and update LRU.
  const bool hit = page_table_.count(page_id) > 0;
  size_t frame_idx = load_page_locked(page_id);
  if (hit) {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
  }
  return frames_[frame_idx].data;
}

// ─────────────────────────────────────────────
//  contains — check residency
// ─────────────────────────────────────────────

bool BufferPool::contains(page_id_t page_id) const {
  std::shared_lock lock(pool_latch_); // Shared: read-only query.
  return page_table_.count(page_id) > 0;
}

// ─────────────────────────────────────────────
//  get_page_copy — safe resident-page read
// ─────────────────────────────────────────────

std::optional<std::array<char, PAGE_SIZE>>
BufferPool::get_page_copy(page_id_t page_id) const {
  total_reads_.fetch_add(1, std::memory_order_relaxed);

  std::shared_lock lock(pool_latch_); // Shared: copies stable bytes.
  if (auto it = page_table_.find(page_id); it != page_table_.end()) {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
    return frames_[it->second].data;
  }
  return std::nullopt;
}

// ─────────────────────────────────────────────
//  write_page_copy — safe page replacement
// ─────────────────────────────────────────────

void BufferPool::write_page_copy(page_id_t page_id,
                                 const std::array<char, PAGE_SIZE> &page_data) {
  std::unique_lock lock(pool_latch_); // Exclusive: modifies page bytes.
  size_t frame_idx = load_page_locked(page_id);
  frames_[frame_idx].data = page_data;
  frames_[frame_idx].dirty = true;
}

// ─────────────────────────────────────────────
//  flush_all — write all dirty pages to disk
// ─────────────────────────────────────────────

void BufferPool::flush_all() {
  std::unique_lock lock(
      pool_latch_); // Exclusive: reads page bytes, clears dirty.
  for (auto &frame : frames_) {
    if (frame.page_id != INVALID_PAGE_ID && frame.dirty) {
      if (flush_callback_) {
        flush_callback_(frame.page_id, frame.data.data());
      }
      frame.dirty = false;
    }
  }
}

// ─────────────────────────────────────────────
//  set_flush_callback
// ─────────────────────────────────────────────

void BufferPool::set_flush_callback(FlushCallback cb) {
  std::unique_lock lock(pool_latch_); // Exclusive: configuration.
  flush_callback_ = std::move(cb);
}

void BufferPool::clear() {
  std::unique_lock lock(pool_latch_);
  for (auto &frame : frames_) {
      frame.reset();
  }
  std::queue<size_t> empty_queue;
  std::swap(free_list_, empty_queue);
  for (size_t i = 0; i < capacity_; ++i) {
      free_list_.push(i);
  }
  page_table_.clear();
  lru_list_.clear();
  lru_map_.clear();
  total_reads_.store(0, std::memory_order_relaxed);
  cache_hits_.store(0, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────
//  set_load_callback
// ─────────────────────────────────────────────

void BufferPool::set_load_callback(LoadCallback cb) {
  std::unique_lock lock(pool_latch_); // Exclusive: configuration.
  load_callback_ = std::move(cb);
}

// ─────────────────────────────────────────────
//  size
// ─────────────────────────────────────────────

size_t BufferPool::size() const {
  std::shared_lock lock(pool_latch_);
  return page_table_.size();
}

std::vector<FrameState> BufferPool::get_frame_states() const {
  std::shared_lock lock(pool_latch_);
  std::vector<FrameState> states;
  states.reserve(capacity_);
  for (const auto &frame : frames_) {
    states.push_back({frame.page_id, frame.dirty, frame.page_id != INVALID_PAGE_ID});
  }
  return states;
}

// ─────────────────────────────────────────────
//  load_page_locked — the page fault handler
// ─────────────────────────────────────────────
//
// Control flow:
//   1. Cache hit  -> promote to MRU, return frame index.
//   2. Free frame -> claim from free list, insert into LRU + page table.
//   3. No free    -> evict LRU victim, reclaim its frame.

size_t BufferPool::load_page_locked(page_id_t page_id) {
  // Case 1: Page is already in the buffer pool (cache hit).
  if (auto it = page_table_.find(page_id); it != page_table_.end()) {
    promote(page_id);
    return it->second;
  }

  // Case 2 & 3: Page fault — need a frame.
  size_t frame_idx;
  if (!free_list_.empty()) {
    frame_idx = free_list_.front();
    free_list_.pop();
  } else {
    frame_idx = evict_lru();
  }

  Frame &frame = frames_[frame_idx];
  frame.reset();
  frame.page_id = page_id;

  if (load_callback_) {
    load_callback_(page_id, frame.data.data());
  }

  page_table_[page_id] = frame_idx;
  lru_list_.push_front(page_id);
  lru_map_[page_id] = lru_list_.begin();

  return frame_idx;
}

// ─────────────────────────────────────────────
//  evict_lru — reclaim the least recently used frame
// ─────────────────────────────────────────────
//
// 1. Identify the LRU victim (back of the list).
// 2. If dirty, invoke the flush callback to write to disk.
// 3. Remove from page table, LRU list, and LRU map.
// 4. Return the freed frame index for reuse.

size_t BufferPool::evict_lru() {
  if (lru_list_.empty()) {
    throw std::runtime_error("BufferPool::evict_lru: no pages to evict");
  }

  page_id_t victim_id = lru_list_.back();

  auto table_it = page_table_.find(victim_id);
  assert(table_it != page_table_.end());
  size_t frame_idx = table_it->second;

  Frame &victim_frame = frames_[frame_idx];
  if (victim_frame.dirty && flush_callback_) {
    flush_callback_(victim_id, victim_frame.data.data());
  }

  lru_list_.pop_back();
  lru_map_.erase(victim_id);
  page_table_.erase(table_it);

  return frame_idx;
}

// ─────────────────────────────────────────────
//  promote — move a page to Most Recently Used
// ─────────────────────────────────────────────
//
// Uses std::list::splice to move the node in O(1)
// without any allocation or deallocation.

void BufferPool::promote(page_id_t page_id) {
  auto map_it = lru_map_.find(page_id);
  if (map_it == lru_map_.end())
    return;

  lru_list_.splice(lru_list_.begin(), lru_list_, map_it->second);
}

} // namespace minidb
