#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

// SparseIndex keeps search keys sorted and maps each key to the page
// that currently stores its value.
class SparseIndex {
public:
    void put(const std::string& key, uint64_t page_id);

    [[nodiscard]] std::optional<uint64_t> get(const std::string& key) const;

    [[nodiscard]] std::vector<std::pair<std::string, uint64_t>>
    scan_range(const std::string& start_key, const std::string& end_key) const;

    void remove(const std::string& key);

    void clear();

    [[nodiscard]] std::vector<std::string> get_all_keys() const;

    [[nodiscard]] size_t size() const;

private:
    std::map<std::string, uint64_t> entries_;
};

} // namespace minidb
