#include "Engine.h"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <iomanip>
#include <filesystem>

using namespace minidb;

void print_help() {
    std::cout << "\nAvailable commands:\n"
              << "  put <key> <val>       - Write key-value pair (durable, calls sync)\n"
              << "  put_async <key> <val> - Write key-value pair asynchronously (fast, not synced immediately)\n"
              << "  get <key>             - Retrieve value of key\n"
              << "  scan <start> <end>    - Scan keys in range [start, end]\n"
              << "  status                - View engine status & metrics\n"
              << "  sync                  - Manually force WAL to disk\n"
              << "  crash                 - Simulate sudden process crash (abrupt exit, no cleanup)\n"
              << "  exit                  - Graceful exit (flushes buffers, deletes WAL)\n"
              << "  help                  - Show this help menu\n\n";
}

int main() {
    std::cout << "=================================================\n";
    std::cout << "  __  __ _       _ _____  ____  \n";
    std::cout << " |  \\/  (_)     (_)  __ \\|  _ \\ \n";
    std::cout << " | \\  / |_ _ __  _| |  | | |_) |\n";
    std::cout << " | |\\/| | | '_ \\| | |  | |  _ < \n";
    std::cout << " | |  | | | | | | | |__| | |_) |\n";
    std::cout << " |_|  |_|_|_| |_|_|_____/|____/  Interactive CLI\n";
    std::cout << "=================================================\n\n";

    std::string db_path = "demo.db";
    std::string wal_path = "demo.wal";
    size_t bp_size = 10;

    std::cout << "Initializing Engine...\n";
    std::cout << "  Database File:   " << db_path << "\n";
    std::cout << "  WAL File:        " << wal_path << "\n";
    std::cout << "  Buffer Pool:     " << bp_size << " frames (40 KB total)\n";
    std::cout << "-------------------------------------------------\n";

    try {
        Engine engine(db_path, wal_path, bp_size);
        std::cout << "Engine ready! Type 'help' for command list.\n\n";

        std::string line;
        while (true) {
            std::cout << "minidb> ";
            std::cout.flush();
            if (!std::getline(std::cin, line)) {
                break;
            }

            if (line.empty()) continue;

            std::stringstream ss(line);
            std::string cmd;
            ss >> cmd;

            if (cmd == "help") {
                print_help();
            } else if (cmd == "put") {
                std::string key, val;
                ss >> key;
                std::getline(ss, val);
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    val = val.substr(start);
                }
                if (key.empty() || val.empty()) {
                    std::cout << "Usage: put <key> <value>\n";
                    continue;
                }
                try {
                    engine.put(key, val, true);
                    std::cout << "OK (Durable put successful)\n";
                } catch (const std::exception& e) {
                    std::cout << "Error: " << e.what() << "\n";
                }
            } else if (cmd == "put_async") {
                std::string key, val;
                ss >> key;
                std::getline(ss, val);
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    val = val.substr(start);
                }
                if (key.empty() || val.empty()) {
                    std::cout << "Usage: put_async <key> <value>\n";
                    continue;
                }
                try {
                    engine.put(key, val, false);
                    std::cout << "OK (Asynchronous put queued in buffer pool)\n";
                } catch (const std::exception& e) {
                    std::cout << "Error: " << e.what() << "\n";
                }
            } else if (cmd == "get") {
                std::string key;
                ss >> key;
                if (key.empty()) {
                    std::cout << "Usage: get <key>\n";
                    continue;
                }
                std::string val = engine.get(key);
                if (val.empty()) {
                    std::cout << "(Key not found)\n";
                } else {
                    std::cout << "Value: \"" << val << "\"\n";
                }
            } else if (cmd == "scan") {
                std::string start_key, end_key;
                ss >> start_key >> end_key;
                if (start_key.empty() || end_key.empty()) {
                    std::cout << "Usage: scan <start_key> <end_key>\n";
                    continue;
                }
                auto results = engine.scan(start_key, end_key);
                std::cout << "Found " << results.size() << " records:\n";
                for (const auto& [k, v] : results) {
                    std::cout << "  " << k << " => \"" << v << "\"\n";
                }
            } else if (cmd == "status") {
                std::cout << "Engine Status:\n";
                std::cout << "  Keys in Index:        " << engine.index_size() << "\n";
                std::cout << "  Buffer Pool Capacity: " << bp_size << " frames\n";
                
                namespace fs = std::filesystem;
                if (fs::exists(db_path)) {
                    std::cout << "  Database file size:   " << fs::file_size(db_path) << " bytes ("
                              << (fs::file_size(db_path) / 4096) << " pages)\n";
                } else {
                    std::cout << "  Database file:        Not found on disk\n";
                }
                if (fs::exists(wal_path)) {
                    std::cout << "  WAL file size:        " << fs::file_size(wal_path) << " bytes\n";
                } else {
                    std::cout << "  WAL file:             Not found on disk (no active log)\n";
                }
            } else if (cmd == "sync") {
                engine.sync_wal();
                std::cout << "WAL synced to disk.\n";
            } else if (cmd == "crash") {
                std::cout << "💥 Simulating sudden process crash...\n";
                std::cout << "Exiting immediately via std::_Exit(0). No destructors called. No pages flushed.\n";
                std::cout.flush();
                std::_Exit(0);
            } else if (cmd == "exit") {
                std::cout << "Gracefully shutting down engine...\n";
                break;
            } else {
                std::cout << "Unknown command: '" << cmd << "'. Type 'help' for command list.\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Engine Initialization Failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Goodbye!\n";
    return 0;
}
