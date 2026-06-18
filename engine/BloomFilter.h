#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace minidb {

// A probabilistic data structure that tells us if a key DEFINITELY does not exist.
// Used to protect the storage layer from I/O starvation during invalid queries.
class BloomFilter {
public:
    // Initialize a Bloom Filter with a specific number of bits.
    // For this demo, 8192 bits (1 KB) is plenty.
    explicit BloomFilter(size_t num_bits = 8192);

    // Add a key to the filter (hashes it and sets the bits)
    void add(const std::string& key);

    // Returns true if the key MIGHT exist.
    // Returns false if the key DEFINITELY does NOT exist.
    [[nodiscard]] bool might_contain(const std::string& key) const;

    // Clear the filter (used during database wipe)
    void clear();

private:
    std::vector<bool> bits_;

    // Generate 3 distinct hashes for a key
    [[nodiscard]] std::vector<size_t> hash(const std::string& key) const;
};

} // namespace minidb
