#include "BloomFilter.h"

#include <functional>

namespace minidb {

BloomFilter::BloomFilter(size_t num_bits) : bits_(num_bits, false) {}

void BloomFilter::add(const std::string& key) {
    if (bits_.empty()) return;
    auto hashes = hash(key);
    for (size_t h : hashes) {
        bits_[h % bits_.size()] = true;
    }
}

bool BloomFilter::might_contain(const std::string& key) const {
    if (bits_.empty()) return false;
    auto hashes = hash(key);
    for (size_t h : hashes) {
        if (!bits_[h % bits_.size()]) {
            return false; // DEFINITELY DOES NOT EXIST
        }
    }
    return true; // MIGHT EXIST
}

void BloomFilter::clear() {
    std::fill(bits_.begin(), bits_.end(), false);
}

std::vector<size_t> BloomFilter::hash(const std::string& key) const {
    // We simulate 3 different hash functions by appending salt to the key
    std::hash<std::string> hasher;
    std::vector<size_t> hashes;
    hashes.reserve(3);
    hashes.push_back(hasher(key + "_salt1"));
    hashes.push_back(hasher(key + "_salt2"));
    hashes.push_back(hasher(key + "_salt3"));
    return hashes;
}

} // namespace minidb
