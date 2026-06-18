#include "DiskManager.h"
#include "Engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#else
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#endif

using namespace minidb;
using Clock = std::chrono::high_resolution_clock;

namespace {

constexpr int kPort = 8080;
const std::string kDbPath = "demo.db";
const std::string kWalPath = "demo.wal";

std::string json_escape(const std::string &input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += ' ';
      } else {
        out += c;
      }
    }
  }
  return out;
}

std::string url_decode(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      std::string hex = input.substr(i + 1, 2);
      char *end = nullptr;
      long value = std::strtol(hex.c_str(), &end, 16);
      if (end && *end == '\0') {
        out.push_back(static_cast<char>(value));
        i += 2;
      }
    } else if (input[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(input[i]);
    }
  }
  return out;
}

std::unordered_map<std::string, std::string>
parse_query(const std::string &query) {
  std::unordered_map<std::string, std::string> params;
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    std::string part = query.substr(
        pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = part.find('=');
    if (eq != std::string::npos) {
      params[url_decode(part.substr(0, eq))] = url_decode(part.substr(eq + 1));
    }
    if (amp == std::string::npos)
      break;
    pos = amp + 1;
  }
  return params;
}

size_t skip_ws(const std::string &s, size_t pos) {
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
    ++pos;
  return pos;
}

std::string json_get_string(const std::string &body, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos)
    return "";
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos)
    return "";
  pos = skip_ws(body, pos + 1);
  if (pos >= body.size() || body[pos] != '"')
    return "";
  ++pos;

  std::string out;
  while (pos < body.size()) {
    char c = body[pos++];
    if (c == '"')
      break;
    if (c == '\\' && pos < body.size()) {
      char esc = body[pos++];
      switch (esc) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      default:
        out.push_back(esc);
        break;
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool json_get_bool(const std::string &body, const std::string &key,
                   bool fallback) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos)
    return fallback;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos)
    return fallback;
  pos = skip_ws(body, pos + 1);
  if (body.compare(pos, 4, "true") == 0)
    return true;
  if (body.compare(pos, 5, "false") == 0)
    return false;
  return fallback;
}

std::string pad_number(size_t n) {
  std::ostringstream oss;
  oss << std::setw(8) << std::setfill('0') << n;
  return oss.str();
}

std::string note_prefix(const std::string &session_id) {
  return "note:" + session_id + ":";
}

std::pair<std::string, std::string> parse_disk_page(const char *page) {
  const char *ks = page + PAGE_KEY_OFFSET;
  const char *ke = std::find(ks, ks + LogRecord::KEY_SIZE, '\0');
  const char *vs = page + PAGE_VAL_OFFSET;
  const char *ve = std::find(vs, vs + LogRecord::VAL_SIZE, '\0');
  return {std::string(ks, ke), std::string(vs, ve)};
}

struct LinearScanResult {
  bool found = false;
  std::string value;
  uint32_t pages_scanned = 0;
};

LinearScanResult linear_scan_key(const std::string &key) {
  LinearScanResult result;
  DiskManager disk(kDbPath);
  uint32_t pages = disk.num_pages();
  for (uint32_t pid = 0; pid < pages; ++pid) {
    char page[DISK_PAGE_SIZE] = {};
    disk.read_page(pid, page);
    ++result.pages_scanned;
    auto [stored_key, stored_value] = parse_disk_page(page);
    if (stored_key == key) {
      result.found = true;
      result.value = stored_value;
      return result;
    }
  }
  return result;
}

std::string wal_status(LogRecord::Status status) {
  switch (status) {
  case LogRecord::ACTIVE:
    return "ACTIVE";
  case LogRecord::COMMIT:
    return "COMMIT";
  case LogRecord::CHECKPOINT:
    return "CHECKPOINT";
  }
  return "UNKNOWN";
}

void cleanup_files(const std::vector<std::string> &files) {
  std::error_code ec;
  for (const auto &file : files) {
    std::filesystem::remove(file, ec);
  }
}

long long elapsed_us(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
      .count();
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Request {
  std::string method;
  std::string path;
  std::string query;
  std::unordered_map<std::string, std::string> query_params;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

struct Response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body = "{}";
};

std::string reason_phrase(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 500:
    return "Internal Server Error";
  default:
    return "OK";
  }
}

Response json_response(const std::string &body, int status = 200) {
  Response res;
  res.status = status;
  res.content_type = "application/json";
  res.body = body;
  return res;
}

std::string notes_objects_json(const std::vector<std::pair<std::string, std::string>> &notes) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < notes.size(); ++i) {
    if (i)
      oss << ",";
    oss << "{\"key\":\"" << json_escape(notes[i].first) << "\",\"text\":\"" << json_escape(notes[i].second) << "\"}";
  }
  oss << "]";
  return oss.str();
}

class NotesServerState {
public:
  explicit NotesServerState(Engine &db_engine) : engine(db_engine) {}

  Engine &engine;
  std::mutex naive_mutex;
  std::unordered_map<std::string, std::vector<std::string>> naive_store;
  int crash_count = 0;
};

Response handle_api_crash(NotesServerState &state, const Request &req) {
  state.engine.flush();
#ifdef _WIN32
  std::abort();
#else
  ::kill(::getpid(), SIGKILL);
#endif
  return json_response("{\"ok\":true}");
}

Response handle_api_checkpoint(NotesServerState &state, const Request &req) {
  state.engine.force_checkpoint();
  return json_response("{\"ok\":true}");
}

Response handle_api_wipe(NotesServerState &state, const Request &req) {
  state.engine.wipe_database();
  {
      std::lock_guard lock(state.naive_mutex);
      state.naive_store.clear();
  }
  state.crash_count = 0;
  return json_response("{\"ok\":true}");
}

Response handle_api_note(NotesServerState &state, const Request &req) {
  std::string mode = json_get_string(req.body, "mode");
  std::string session_id = json_get_string(req.body, "session_id");
  std::string text = json_get_string(req.body, "text");
  bool sync = json_get_bool(req.body, "sync", true);

  if (mode.empty() || session_id.empty() || text.empty()) {
    return json_response(
        "{\"ok\":false,\"error\":\"mode, session_id, and text are required\"}",
        400);
  }

  if (mode == "naive") {
    std::lock_guard lock(state.naive_mutex);
    auto &notes = state.naive_store[session_id];
    std::string provided_key = json_get_string(req.body, "key");
    if (!provided_key.empty()) {
        try {
            int idx = std::stoi(provided_key);
            if (idx >= 0 && idx < notes.size()) notes[idx] = text;
        } catch(...) {}
    } else {
        notes.push_back(text);
    }
    return json_response("{\"ok\":true,\"mode\":\"naive\",\"count\":" +
                         std::to_string(notes.size()) + "}");
  }

  if (mode == "minidb") {
    const std::string prefix = note_prefix(session_id);
    std::string key = json_get_string(req.body, "key");
    
    if (key.empty()) {
      auto existing = state.engine.scan(prefix, prefix + "~");
      int next_id = 0;
      if (!existing.empty()) {
        std::string last_key = existing.back().first;
        size_t last_colon = last_key.rfind(':');
        if (last_colon != std::string::npos) {
           try { next_id = std::stoi(last_key.substr(last_colon + 1)) + 1; } catch(...) {}
        }
      }
      key = prefix + pad_number(next_id);
    }
    
    state.engine.put(key, text, sync);
    return json_response(
        "{\"ok\":true,\"mode\":\"minidb\",\"key\":\"" + json_escape(key) +
        "\"}");
  }

  return json_response("{\"ok\":false,\"error\":\"unknown mode\"}", 400);
}

Response handle_api_note_delete(NotesServerState &state, const Request &req) {
  std::string mode = json_get_string(req.body, "mode");
  std::string session_id = json_get_string(req.body, "session_id");
  std::string key = json_get_string(req.body, "key");
  bool sync = json_get_bool(req.body, "sync", true);

  if (mode.empty() || session_id.empty() || key.empty()) {
    return json_response("{\"ok\":false,\"error\":\"mode, session_id, and key are required\"}", 400);
  }

  if (mode == "minidb") {
    state.engine.del(key, sync);
  } else if (mode == "naive") {
    std::lock_guard lock(state.naive_mutex);
    auto &notes = state.naive_store[session_id];
    try {
        int idx = std::stoi(key);
        if (idx >= 0 && idx < notes.size()) {
            notes.erase(notes.begin() + idx);
        }
    } catch(...) {}
  }

  return json_response("{\"ok\":true}");
}

Response handle_api_notes(NotesServerState &state, const Request &req) {
  std::string mode =
      req.query_params.count("mode") ? req.query_params.at("mode") : "";
  std::string session_id = req.query_params.count("session_id")
                               ? req.query_params.at("session_id")
                               : "";
  if (mode.empty() || session_id.empty()) {
    return json_response(
        "{\"ok\":false,\"error\":\"mode and session_id are required\"}", 400);
  }

  std::vector<std::pair<std::string, std::string>> notes;
  if (mode == "naive") {
    std::lock_guard lock(state.naive_mutex);
    auto it = state.naive_store.find(session_id);
    if (it != state.naive_store.end()) {
      for (size_t i = 0; i < it->second.size(); ++i) {
        notes.push_back({std::to_string(i), it->second[i]});
      }
    }
  } else if (mode == "minidb") {
    const std::string prefix = note_prefix(session_id);
    notes = state.engine.scan(prefix, prefix + "~");
  } else {
    return json_response("{\"ok\":false,\"error\":\"unknown mode\"}", 400);
  }

  return json_response("{\"ok\":true,\"notes\":" + notes_objects_json(notes) + "}");
}

Response handle_api_search(NotesServerState &state, const Request &req) {
  std::string key =
      req.query_params.count("key") ? req.query_params.at("key") : "";
  bool use_index = true;
  if (req.query_params.count("use_index")) {
    use_index = req.query_params.at("use_index") != "false";
  }
  if (key.empty()) {
    return json_response("{\"ok\":false,\"error\":\"key is required\"}", 400);
  }

  bool bloom_rejected = !state.engine.bloom_check(key);
  if (bloom_rejected) {
     return json_response("{\"ok\":true,\"found\":false,\"value\":\"\",\"bloom_rejected\":true}");
  }

  auto start = Clock::now();
  bool found = false;
  std::string value;
  uint32_t pages_scanned = 0;

  if (use_index) {
    value = state.engine.get(key);
    found = !value.empty();
  } else {
    state.engine.flush();
    auto scan = linear_scan_key(key);
    found = scan.found;
    value = scan.value;
    pages_scanned = scan.pages_scanned;
  }

  auto us = elapsed_us(start, Clock::now());
  std::ostringstream oss;
  oss << "{\"ok\":true,\"found\":" << (found ? "true" : "false")
      << ",\"use_index\":" << (use_index ? "true" : "false")
      << ",\"microseconds\":" << us << ",\"pages_scanned\":" << pages_scanned
      << ",\"bloom_rejected\":false"
      << ",\"value\":\"" << json_escape(value) << "\"}";
  return json_response(oss.str());
}

Response handle_api_status(NotesServerState &state) {
  auto stats = state.engine.get_buffer_pool_stats();
  auto wal_tail = state.engine.get_wal_tail_records(5);

  std::ostringstream tail;
  tail << "[";
  for (size_t i = 0; i < wal_tail.size(); ++i) {
    const auto &r = wal_tail[i];
    if (i) tail << ",";
    
    std::string op = "META";
    if (r.status == LogRecord::ACTIVE) {
        if (r.old_val_str().empty()) op = "INSERT";
        else if (r.new_val_str().empty()) op = "DELETE";
        else op = "UPDATE";
    }

    tail << "{\"lsn\":" << r.lsn << ",\"txn_id\":" << r.txn_id
         << ",\"page_id\":" << r.page_id << ",\"status\":\""
         << wal_status(r.status) << "\",\"op\":\"" << op << "\"";
    if (r.key[0] != '\0') {
      tail << ",\"key\":\"" << json_escape(r.key_str()) << "\"";
    }
    tail << "}";
  }
  tail << "]";

  std::ostringstream bp_frames;
  bp_frames << "[";
  for (size_t i = 0; i < stats.frames.size(); ++i) {
    if (i) bp_frames << ",";
    bp_frames << "{\"page_id\":" << stats.frames[i].page_id 
              << ",\"dirty\":" << (stats.frames[i].dirty ? "true" : "false") 
              << ",\"valid\":" << (stats.frames[i].valid ? "true" : "false") << "}";
  }
  bp_frames << "]";

  std::string recovery_log = state.engine.recovery_log();
  std::string escaped_recovery =
      recovery_log.empty() ? "null" : ("\"" + json_escape(recovery_log) + "\"");

  std::ostringstream oss;
  oss << "{\"crash_count\":" << state.crash_count << ",\"buffer_pool\":{"
      << "\"size\":" << stats.size << ",\"capacity\":" << stats.capacity
      << ",\"total_reads\":" << stats.total_reads
      << ",\"cache_hits\":" << stats.cache_hits
      << ",\"hit_rate\":" << stats.hit_rate
      << ",\"frames\":" << bp_frames.str() << "},\"wal\":{"
      << "\"size\":" << state.engine.get_wal_size() << ",\"tail\":" << tail.str()
      << "},\"recovery_log\":" << escaped_recovery << "}";
  return json_response(oss.str());
}

Response handle_api_benchmark(const Request &req) {
  int ops = 10000;
  if (req.query_params.count("ops")) {
    ops = std::max(100, std::min(10000, std::stoi(req.query_params.at("ops"))));
  }

  cleanup_files({"bench_api_sync.db", "bench_api_sync.wal",
                 "bench_api_batch.db", "bench_api_batch.wal",
                 "bench_api_cache.db", "bench_api_cache.wal"});

  long long sync_us = 0;
  long long batch_us = 0;
  long long warm_read_us = 0;
  long long cold_read_us = 0;

  {
    Engine engine("bench_api_sync.db", "bench_api_sync.wal", 128);
    auto start = Clock::now();
    for (int i = 0; i < ops; ++i) {
      engine.put("k" + pad_number(i), "v" + std::to_string(i), true);
    }
    sync_us = elapsed_us(start, Clock::now());
  }

  {
    Engine engine("bench_api_batch.db", "bench_api_batch.wal", 128);
    auto start = Clock::now();
    for (int i = 0; i < ops; ++i) {
      engine.put("k" + pad_number(i), "v" + std::to_string(i), false);
      if ((i + 1) % 100 == 0)
        engine.sync_wal();
    }
    engine.sync_wal();
    batch_us = elapsed_us(start, Clock::now());
  }

  {
    Engine engine("bench_api_cache.db", "bench_api_cache.wal", 256);
    for (int i = 0; i < ops; ++i) {
      engine.put("k" + pad_number(i), "v" + std::to_string(i), false);
    }
    engine.sync_wal();

    auto start = Clock::now();
    for (int i = 0; i < ops; ++i) {
      auto val = engine.get("k" + pad_number(i));
      if (val.empty())
        throw std::runtime_error("benchmark warm read failed");
    }
    warm_read_us = elapsed_us(start, Clock::now());
  }

  {
    Engine engine("bench_api_cache.db", "bench_api_cache.wal", 8);
    auto start = Clock::now();
    for (int i = 0; i < ops; ++i) {
      int target = (i * 101) % ops;
      auto val = engine.get("k" + pad_number(target));
      if (val.empty())
        throw std::runtime_error("benchmark cold read failed");
    }
    cold_read_us = elapsed_us(start, Clock::now());
  }

  cleanup_files({"bench_api_sync.db", "bench_api_sync.wal",
                 "bench_api_batch.db", "bench_api_batch.wal",
                 "bench_api_cache.db", "bench_api_cache.wal"});

  std::ostringstream oss;
  oss << "{\"ok\":true,\"ops\":" << ops << ",\"wal_sync_us\":" << sync_us
      << ",\"wal_batch_us\":" << batch_us
      << ",\"warm_read_us\":" << warm_read_us
      << ",\"cold_read_us\":" << cold_read_us << "}";
  return json_response(oss.str());
}

Response handle_api_demo_slow_read(NotesServerState &state, const Request &req) {
  auto start = Clock::now();
  std::string val = state.engine.slow_read_demo("demo_key");
  auto ms = elapsed_us(start, Clock::now()) / 1000;
  return json_response("{\"ok\":true,\"val\":\"" + json_escape(val) + "\",\"ms\":" + std::to_string(ms) + "}");
}

Response handle_api_demo_slow_write(NotesServerState &state, const Request &req) {
  auto start = Clock::now();
  state.engine.slow_write_demo("demo_key", "demo_val_" + std::to_string(start.time_since_epoch().count()));
  auto ms = elapsed_us(start, Clock::now()) / 1000;
  return json_response("{\"ok\":true,\"ms\":" + std::to_string(ms) + "}");
}

Response route_request(NotesServerState &state, const Request &req) {
  try {
    if (req.method == "OPTIONS") {
      Response res;
      res.status = 204;
      res.body = "";
      return res;
    }
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
      std::string html = read_file("public/index.html");
      if (html.empty())
        return json_response(
            "{\"ok\":false,\"error\":\"public/index.html not found\"}", 404);
      Response res;
      res.content_type = "text/html; charset=utf-8";
      res.body = html;
      return res;
    }
    if (req.method == "POST" && req.path == "/api/crash")
      return handle_api_crash(state, req);
    if (req.method == "POST" && req.path == "/api/crash_mid_txn") {
      Engine *engine = &state.engine;
      std::thread([engine]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine->test_put_and_crash_before_commit("note:mid_txn_demo:00000000",
                                                 "UNCOMMITTED_NOTE");
      }).detach();
      return json_response(
          "{\"ok\":true,\"message\":\"mid-transaction crash scheduled\"}");
    }
    if (req.method == "POST" && req.path == "/api/checkpoint")
      return handle_api_checkpoint(state, req);
    if (req.method == "POST" && req.path == "/api/wipe")
      return handle_api_wipe(state, req);
    if (req.method == "POST" && req.path == "/api/note")
      return handle_api_note(state, req);
    if (req.method == "DELETE" && req.path == "/api/note")
      return handle_api_note_delete(state, req);
    if (req.method == "GET" && req.path == "/api/notes")
      return handle_api_notes(state, req);
    if (req.method == "GET" && req.path == "/api/search")
      return handle_api_search(state, req);
    if (req.method == "GET" && req.path == "/api/status")
      return handle_api_status(state);
    if (req.method == "GET" && req.path == "/api/benchmark")
      return handle_api_benchmark(req);
    if (req.method == "POST" && req.path == "/api/demo/slow_read")
      return handle_api_demo_slow_read(state, req);
    if (req.method == "POST" && req.path == "/api/demo/slow_write")
      return handle_api_demo_slow_write(state, req);
    if (req.method == "POST" && req.path == "/api/crash") {
      std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
#ifndef _WIN32
        ::kill(::getpid(), SIGKILL);
#else
        std::abort();
#endif
      }).detach();
      return json_response(
          "{\"ok\":true,\"message\":\"process will crash now\"}");
    }
    if (req.method == "POST" && req.path == "/api/crash_mid_txn") {
      Engine *engine = &state.engine;
      std::thread([engine]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine->test_put_and_crash_before_commit("note:mid_txn_demo:00000000",
                                                 "UNCOMMITTED_NOTE");
      }).detach();
      return json_response(
          "{\"ok\":true,\"message\":\"mid-transaction crash scheduled\"}");
    }
    return json_response("{\"ok\":false,\"error\":\"not found\"}", 404);
  } catch (const std::exception &e) {
    return json_response(
        "{\"ok\":false,\"error\":\"" + json_escape(e.what()) + "\"}", 500);
  }
}

Request parse_request(const std::string &raw) {
  Request req;
  size_t header_end = raw.find("\r\n\r\n");
  std::string header = raw.substr(0, header_end);
  req.body = header_end == std::string::npos ? "" : raw.substr(header_end + 4);

  std::istringstream hs(header);
  std::string line;
  if (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::istringstream first(line);
    std::string url;
    first >> req.method >> url;
    size_t q = url.find('?');
    req.path = q == std::string::npos ? url : url.substr(0, q);
    req.query = q == std::string::npos ? "" : url.substr(q + 1);
    req.query_params = parse_query(req.query);
  }

  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      value = value.substr(skip_ws(value, 0));
      req.headers[key] = value;
    }
  }
  return req;
}

std::string serialize_response(const Response &res) {
  std::ostringstream out;
  out << "HTTP/1.1 " << res.status << " " << reason_phrase(res.status) << "\r\n"
      << "Content-Type: " << res.content_type << "\r\n"
      << "Content-Length: " << res.body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      << "Access-Control-Allow-Headers: Content-Type\r\n"
      << "Connection: close\r\n\r\n"
      << res.body;
  return out.str();
}

void handle_client(socket_t client_fd, NotesServerState &state) {
  std::string raw;
  char buffer[4096];
  int content_length = 0;

  while (true) {
    ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      break;
    raw.append(buffer, static_cast<size_t>(n));

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      std::string header = raw.substr(0, header_end);
      std::string marker = "Content-Length:";
      size_t pos = header.find(marker);
      if (pos != std::string::npos) {
        pos += marker.size();
        content_length = std::stoi(header.substr(skip_ws(header, pos)));
      }
      if (raw.size() >= header_end + 4 + static_cast<size_t>(content_length))
        break;
    }
  }

  Response response;
  if (raw.empty()) {
    response = json_response("{\"ok\":false,\"error\":\"empty request\"}", 400);
  } else {
    Request req = parse_request(raw);
    response = route_request(state, req);
  }

  std::string payload = serialize_response(response);
  send(client_fd, payload.data(), payload.size(), 0);
#ifndef _WIN32
  close(client_fd);
#else
  closesocket(client_fd);
#endif
}

void serve(NotesServerState &state) {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    throw std::runtime_error("WSAStartup failed");
  }
#endif

  socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
    throw std::runtime_error("socket failed");

  int opt = 1;
#ifdef _WIN32
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(kPort);

  if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    throw std::runtime_error("bind failed on port 8080");
  }
  if (listen(server_fd, 64) < 0)
    throw std::runtime_error("listen failed");

  std::cout << "MiniDB Notes Vault server running at http://localhost:" << kPort
            << "\n";
  while (true) {
    socket_t client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0)
      continue;
    std::thread(handle_client, client_fd, std::ref(state)).detach();
  }
}

} // namespace

int main() {
  try {
    Engine engine(kDbPath, kWalPath, 10);

    std::string crash_count_str = engine.get("system_crash_count");
    int crash_count = crash_count_str.empty() ? 0 : std::stoi(crash_count_str);
    ++crash_count;
    engine.put("system_crash_count", std::to_string(crash_count), true);

    NotesServerState state(engine);
    state.crash_count = crash_count;
    serve(state);
  } catch (const std::exception &e) {
    std::cerr << "Server failed: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
