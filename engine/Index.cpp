#include "Index.h"

namespace minidb {

void SparseIndex::put(const std::string& key, uint64_t page_id) {
    entries_[key] = page_id;
}

std::optional<uint64_t> SparseIndex::get(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::pair<std::string, uint64_t>>
SparseIndex::scan_range(const std::string& start_key,
                        const std::string& end_key) const {
    std::vector<std::pair<std::string, uint64_t>> result;

    auto it = entries_.lower_bound(start_key);
    auto end = entries_.upper_bound(end_key);
    for (; it != end; ++it) {
        result.push_back(*it);
    }

    return result;
}

void SparseIndex::remove(const std::string& key) {
    entries_.erase(key);
}

void SparseIndex::clear() {
    entries_.clear();
}

std::vector<std::string> SparseIndex::get_all_keys() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& [key, page_id] : entries_) {
        keys.push_back(key);
    }
    return keys;
}

size_t SparseIndex::size() const {
    return entries_.size();
}

} // namespace minidb
