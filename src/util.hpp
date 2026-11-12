#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace sb {

// -----------------------------------------------------------------------------
// logging
enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void set_log_level(LogLevel lvl);
void log(LogLevel lvl, const std::string& msg);

template <typename... Args> inline std::string cat(Args&&... args) {
  std::string out;
  (out += ... += std::string(args));
  return out;
}

// -----------------------------------------------------------------------------
// types
struct ObjectRef {
  std::string uri;  // normalized, no query string
  std::string name; // last path segment (basename)
  std::uint64_t size = 0;
  std::int64_t mtime = 0; // epoch seconds, 0 = unknown
  bool is_dir = false;    // WebDAV collection / S3 "directory" 前缀
};

// -----------------------------------------------------------------------------
// helpers
// 去掉 query string、折叠重复斜杠、去尾部斜杠；对象存储签名参数不能进数据库。
std::string normalize_uri(const std::string& in);
std::string join_uri(const std::string& base, const std::string& name);
// 仅用于日志：把 X-Amz-Signature / Credential / token 之类的值打码。
std::string redact_uri(const std::string& in);
std::string basename_of(const std::string& uri);

// 按扩展名取 MIME：常见类型走映射表；表里没有的直接返回小写扩展名本身
// （如 "typ" / "jsonl"）；完全没有扩展名才返回 application/octet-stream
std::string mime_type_for(const std::string& path);

// 去掉首尾空白（空格 / 制表符 / 换行）
std::string trim(std::string s);

// 逐字符转小写（只处理 ASCII）
std::string to_lower(std::string s);

timespec make_timespec(int seconds);

} // namespace sb
