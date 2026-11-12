#include "util.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <utility>

namespace sb {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_log_mutex;

const char* level_tag(LogLevel lvl) {
  switch (lvl) {
  case LogLevel::Error:
    return "ERROR";
  case LogLevel::Warn:
    return "WARN ";
  case LogLevel::Info:
    return "INFO ";
  case LogLevel::Debug:
    return "DEBUG";
  }
  return "?????";
}
} // namespace

void set_log_level(LogLevel lvl) {
  g_level.store(static_cast<int>(lvl));
}
void log(LogLevel lvl, const std::string& msg) {
  if (static_cast<int>(lvl) > g_level.load()) {
    return;
  }
  auto now = std::chrono::system_clock::now();
  auto tt = std::chrono::system_clock::to_time_t(now);
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&tt, &tm);
  char head[32];
  std::snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(ms));

  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cerr << head << " " << level_tag(lvl) << " " << msg << '\n';
}

std::string normalize_uri(const std::string& in) {
  std::string s = in;
  const auto q = s.find('?');
  if (q != std::string::npos) {
    s.erase(q);
  }
  const auto scheme = s.find("://");
  const std::size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
  std::string out = s.substr(0, start);
  for (std::size_t i = start; i < s.size(); ++i) {
    if (s[i] == '/' && !out.empty() && out.back() == '/') {
      continue;
    }
    out.push_back(s[i]);
  }
  while (out.size() > start && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

std::string join_uri(const std::string& base, const std::string& name) {
  std::string b = normalize_uri(base);
  if (name.empty()) {
    return b;
  }
  if (!b.empty() && b.back() != '/') {
    b.push_back('/');
  }
  return normalize_uri(b + name);
}

std::string redact_uri(const std::string& in) {
  const auto q = in.find('?');
  if (q == std::string::npos) {
    return in;
  }
  std::string out = in.substr(0, q);
  std::string query = in.substr(q + 1);
  out += "?...";
  // 只保留参数名，值全部打码；避免把签名写进日志/数据库
  std::size_t pos = 0;
  bool first = true;
  while (pos <= query.size()) {
    const auto amp = query.find('&', pos);
    std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const auto eq = kv.find('=');
    const std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
    if (!key.empty()) {
      if (!first) {
        out += "&";
      }
      out += key + "=***";
      first = false;
    }
    if (amp == std::string::npos) {
      break;
    }
    pos = amp + 1;
  }
  return out;
}

std::string basename_of(const std::string& uri) {
  const auto q = uri.find('?');
  std::string s = (q == std::string::npos) ? uri : uri.substr(0, q);
  while (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  const auto slash = s.find_last_of('/');
  return (slash == std::string::npos) ? s : s.substr(slash + 1);
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

std::string to_lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

timespec make_timespec(int seconds) {
  timespec ts{};
  ts.tv_sec = seconds;
  ts.tv_nsec = 0;
  return ts;
}

std::string mime_type_for(const std::string& path) {
  // 只按扩展名做小映射：表里有的走标准 mime；表里没有的直接返回小写扩展名本身。
  static const std::pair<const char*, const char*> kTable[] = {
      {"flac", "audio/flac"},
      {"mp3", "audio/mpeg"},
      {"m4a", "audio/mp4"},
      {"aac", "audio/aac"},
      {"ogg", "audio/ogg"},
      {"opus", "audio/opus"},
      {"wav", "audio/wav"},
      {"ape", "audio/x-ape"},
      {"wv", "audio/x-wavpack"},
      {"dsf", "audio/x-dsf"},
      {"dff", "audio/x-dff"},
      {"cue", "application/x-cue"},
      {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},
      {"png", "image/png"},
      {"gif", "image/gif"},
      {"webp", "image/webp"},
      {"bmp", "image/bmp"},
      {"tif", "image/tiff"},
      {"tiff", "image/tiff"},
      {"mp4", "video/mp4"},
      {"mkv", "video/x-matroska"},
      {"avi", "video/x-msvideo"},
      {"webm", "video/webm"},
      {"pdf", "application/pdf"},
      {"epub", "application/epub+zip"},
      {"zip", "application/zip"},
      {"7z", "application/x-7z-compressed"},
      {"rar", "application/vnd.rar"},
      {"gz", "application/gzip"},
      {"txt", "text/plain"},
      {"log", "text/plain"},
      {"json", "application/json"},
      {"xml", "application/xml"},
      {"html", "text/html"},
      {"htm", "text/html"},
      {"css", "text/css"},
      {"js", "text/javascript"},
  };

  const std::size_t slash = path.find_last_of('/');
  const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) {
    return "application/octet-stream";
  }

  const std::string ext = to_lower(name.substr(dot + 1));
  for (const auto& [key, mime] : kTable) {
    if (ext == key) {
      return mime;
    }
  }
  // 表里没有的类型：直接把小写扩展名当类型用（如 "typ" / "jsonl" / "srt"）
  return ext;
}

} // namespace sb
