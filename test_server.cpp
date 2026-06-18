#include "engine/BloomFilter.h"
#include <iostream>
int main() {
    minidb::BloomFilter bloom;
    bloom.add("note:test:0000");
    std::cout << "Might contain: " << bloom.might_contain("note:test:0000") << std::endl;
    return 0;
}
