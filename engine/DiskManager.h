#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace minidb {

// ─────────────────────────────────────────────
//  Constants (shared with BufferPool.h)
// ─────────────────────────────────────────────
// PAGE_SIZE and page_id_t are defined in BufferPool.h.
// We forward-declare them here to avoid circular includes.
// In a larger project, these would live in a shared "types.h".

static constexpr size_t DISK_PAGE_SIZE = 4096;

// ─────────────────────────────────────────────
//  DiskManager — persistent page-level I/O
// ─────────────────────────────────────────────
//
// Provides raw read/write access to a flat binary file (engine.db).
// The file is organized as a sequence of fixed-size pages:
//
//   Byte offset:  0        4096     8192     12288    ...
//   Page ID:      [  0  ]  [  1  ]  [  2  ]  [  3  ]  ...
//
// Each page occupies exactly PAGE_SIZE (4096) bytes.
// Page N starts at byte offset N * 4096.
//
// This class owns the file handle and provides:
//   - read_page(id, dest):  Load page from disk into a memory buffer
//   - write_page(id, src):  Write a memory buffer to the page's disk slot
//   - sync():               Force OS kernel buffers to physical media
//
// Real-world equivalent: PostgreSQL's smgr (storage manager) layer,
// which abstracts the OS filesystem interface for the buffer manager.

class DiskManager {
public:
    // Opens (or creates) the database file at the given path.
    // The file is opened in binary read+write mode.
    explicit DiskManager(const std::string& db_path);

    // Flushes and closes the file.
    ~DiskManager();

    // Non-copyable, non-movable (owns file handle).
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // ── Core I/O ─────────────────────────────

    // Read page `page_id` from disk into `dest`.
    // `dest` must point to a buffer of at least PAGE_SIZE bytes.
    // If the page has never been written (file too small), `dest`
    // is zero-filled — this represents an "empty" new page.
    void read_page(uint32_t page_id, char* dest);

    // Write PAGE_SIZE bytes from `src` to page `page_id`'s slot on disk.
    // Automatically extends the file if `page_id` is beyond the current end.
    void write_page(uint32_t page_id, const char* src);

    // Force all buffered writes to physical storage.
    // Calls fstream::flush() + platform-specific fsync/_commit.
    void sync();

    // Truncates the database file to zero bytes.
    void truncate();

    // ── Accessors ────────────────────────────

    // Returns the number of pages the file currently holds.
    [[nodiscard]] uint32_t num_pages() const;

    // Returns the file path.
    [[nodiscard]] const std::string& file_path() const { return db_path_; }

private:
    std::string db_path_;
    std::fstream file_;

    // Cached file size in bytes (updated on write_page if file grows).
    size_t file_size_bytes_;

    // Calculate the byte offset for a given page ID.
    static constexpr size_t page_offset(uint32_t page_id) {
        return static_cast<size_t>(page_id) * DISK_PAGE_SIZE;
    }

    // Determine the current file size on disk (used at construction).
    size_t measure_file_size();

    // ── Concurrency (Step 5) ─────────────────
    // Protects file_ (seekg/seekp + read/write must be atomic).
    mutable std::mutex io_mutex_;
};

} // namespace minidb
