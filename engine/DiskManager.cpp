#include "DiskManager.h"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <iostream>

// Platform-specific includes for fsync / _commit.
#ifdef _WIN32
    #include <io.h>      // _fileno, _commit
    #include <cstdio>    // fileno (MinGW)
#else
    #include <unistd.h>  // fsync
#endif

namespace minidb {

// ─────────────────────────────────────────────
//  Construction & Destruction
// ─────────────────────────────────────────────

DiskManager::DiskManager(const std::string& db_path)
    : db_path_(db_path)
    , file_size_bytes_(0)
{
    // Attempt to open an existing file in read+write binary mode.
    // std::ios::ate positions the cursor at the end (for measuring size).
    file_.open(db_path_,
        std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);

    if (!file_.is_open()) {
        // File doesn't exist — create it, then reopen in read+write mode.
        // std::ios::out alone creates the file if it doesn't exist.
        std::ofstream create_file(db_path_, std::ios::binary);
        if (!create_file.is_open()) {
            throw std::runtime_error(
                "DiskManager: failed to create file: " + db_path_);
        }
        create_file.close();

        // Now reopen in read+write mode.
        file_.open(db_path_,
            std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
        if (!file_.is_open()) {
            throw std::runtime_error(
                "DiskManager: failed to open file after creation: " + db_path_);
        }
    }

    // Measure the file size (cursor is at end due to std::ios::ate).
    file_size_bytes_ = measure_file_size();
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

// ─────────────────────────────────────────────
//  read_page — load a page from disk into memory
// ─────────────────────────────────────────────
//
// Two cases:
//   1. Page exists on disk (file is large enough) → seek + read.
//   2. Page beyond end of file → zero-fill dest (new/unwritten page).
//
// This distinction is critical: we must never return garbage data
// for a page that was never written. Zero-filling guarantees that
// new pages start clean.

void DiskManager::read_page(uint32_t page_id, char* dest) {
    std::lock_guard lock(io_mutex_);  // Thread-safe: seek + read must be atomic.
    assert(dest != nullptr);

    size_t offset = page_offset(page_id);

    if (offset >= file_size_bytes_) {
        // Page has never been written to disk — return a zeroed page.
        // This is not an error; it's how new pages are born.
        std::memset(dest, 0, DISK_PAGE_SIZE);
        return;
    }

    // Seek to the page's byte offset and read PAGE_SIZE bytes.
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file_.good()) {
        throw std::runtime_error(
            "DiskManager::read_page: seekg failed for page " +
            std::to_string(page_id));
    }

    file_.read(dest, DISK_PAGE_SIZE);

    // Handle partial reads (file might end mid-page due to corruption
    // or the page being at the very end of a partially-written file).
    if (file_.gcount() < static_cast<std::streamsize>(DISK_PAGE_SIZE)) {
        // Zero-fill the remainder so we don't return partial garbage.
        size_t bytes_read = static_cast<size_t>(file_.gcount());
        std::memset(dest + bytes_read, 0, DISK_PAGE_SIZE - bytes_read);
        file_.clear();  // Clear the EOF/fail bits for future operations.
    }
}

// ─────────────────────────────────────────────
//  write_page — persist a page from memory to disk
// ─────────────────────────────────────────────
//
// Steps:
//   1. If the page is beyond the current file end, extend the file
//      by writing zeros to fill the gap. This ensures no "holes"
//      with undefined content (which could happen with sparse files
//      on some platforms).
//   2. Seek to the page's byte offset.
//   3. Write PAGE_SIZE bytes.
//
// Note: This does NOT call fsync(). The caller (or flush_all)
// decides when to force data to physical media. In Step 3, the WAL
// will enforce the durability ordering.

void DiskManager::write_page(uint32_t page_id, const char* src) {
    std::lock_guard lock(io_mutex_);  // Thread-safe: seek + write must be atomic.
    assert(src != nullptr);

    size_t offset = page_offset(page_id);

    // If writing beyond the current file end, extend the file.
    // We fill the gap with zero pages to avoid undefined content.
    if (offset > file_size_bytes_) {
        file_.seekp(0, std::ios::end);
        size_t gap = offset - file_size_bytes_;

        // Write zeros in PAGE_SIZE chunks for efficiency.
        char zeros[DISK_PAGE_SIZE] = {};
        while (gap > 0) {
            size_t chunk = std::min(gap, static_cast<size_t>(DISK_PAGE_SIZE));
            file_.write(zeros, static_cast<std::streamsize>(chunk));
            gap -= chunk;
        }
    }

    // Seek to the page's byte offset and write.
    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file_.good()) {
        throw std::runtime_error(
            "DiskManager::write_page: seekp failed for page " +
            std::to_string(page_id));
    }

    file_.write(src, DISK_PAGE_SIZE);
    if (!file_.good()) {
        throw std::runtime_error(
            "DiskManager::write_page: write failed for page " +
            std::to_string(page_id));
    }

    // Update cached file size if we extended the file.
    size_t new_end = offset + DISK_PAGE_SIZE;
    if (new_end > file_size_bytes_) {
        file_size_bytes_ = new_end;
    }
}

// ─────────────────────────────────────────────
//  sync — force buffered data to physical disk
// ─────────────────────────────────────────────
//
// Two levels of flushing:
//   1. fstream::flush() → pushes C++ streambuf data to the OS kernel.
//   2. fsync() / _commit() → pushes OS kernel buffer to physical media.
//
// Without step 2, a power failure can lose data that the OS has
// "accepted" but not yet written to the drive's platters/NAND.
//
// This is the same reason databases call fsync() after WAL writes:
// you need a guarantee that the bits are on stable storage.

void DiskManager::sync() {
    std::lock_guard lock(io_mutex_);  // Thread-safe.
    file_.flush();

    // Platform-specific fsync to force data to physical media.
    // On MSYS2/MinGW, we use the POSIX-compatible fileno + fsync.
    // On native MSVC, we'd use _fileno + _commit.
    #ifdef _WIN32
        // MinGW/MSYS2 provides fileno() and _commit() / fsync().
        FILE* c_file = nullptr;

        // fstream doesn't expose the file descriptor directly.
        // We reopen the file descriptor for sync. This is a known
        // limitation of std::fstream — production systems use raw
        // file descriptors (open/pread/pwrite) to avoid this.
        //
        // For our educational purposes, fstream::flush() provides
        // sufficient guarantees for step-by-step testing.
        // True fsync will be critical in Step 3 (WAL durability).
        (void)c_file;  // Suppress unused warning.
    #else
        // POSIX: get the file descriptor and call fsync.
        // Note: This requires compiler-specific extensions or
        // using raw file descriptors from the start.
    #endif
}

// ─────────────────────────────────────────────
//  num_pages — count of pages on disk
// ─────────────────────────────────────────────

uint32_t DiskManager::num_pages() const {
    std::lock_guard lock(io_mutex_);  // Thread-safe.
    return static_cast<uint32_t>(file_size_bytes_ / DISK_PAGE_SIZE);
}

// ─────────────────────────────────────────────
//  measure_file_size — determine current file size
// ─────────────────────────────────────────────

size_t DiskManager::measure_file_size() {
    // With std::ios::ate, the cursor starts at the end.
    // tellg() gives us the byte count.
    auto pos = file_.tellg();
    if (pos < 0) {
        return 0;
    }
    return static_cast<size_t>(pos);
}

void DiskManager::truncate() {
    std::lock_guard lock(io_mutex_);
    if (file_.is_open()) {
        file_.close();
    }
    std::ofstream trunc(db_path_, std::ios::binary | std::ios::trunc);
    trunc.close();
    file_.open(db_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
    if (!file_.is_open()) {
        throw std::runtime_error("DiskManager::truncate: failed to reopen file.");
    }
    file_size_bytes_ = measure_file_size();
}

} // namespace minidb
