#pragma once

#include "result.hpp"
#include "storage.hpp"

#include <string>

namespace sb {

class IDatabase;

namespace web {

bool is_uuid(const std::string& s);

// 去掉 scheme 前缀和尾部斜杠：存储表里的 endpoint 只保留 host[:port]
std::string strip_endpoint(std::string endpoint);
// endpoint 只接受 "ip:port" 或域名；路径一律放 config.base_path
Result<std::string> normalize_endpoint(std::string endpoint);
// WebDAV 的起始目录：确保以 / 开头、以 / 结尾、折叠重复斜杠
Result<std::string> normalize_base_path(std::string path);
// 规范化请求里的相对路径："" / "/" -> "/"，"/aaa/" -> "/aaa"；拒绝 ..
Result<std::string> normalize_rel_path(std::string rel);
// 把已规范化的相对路径拼到 base_path 后面
Result<std::string> build_storage_path(const std::string& base_path, const std::string& rel);
// 唯一约束冲突（name/storage_id）映射成 409
Error map_conflict(Error e);

// storage_nodes 的一行 → 能直接拼 URL 的目标（含凭据）。目前只支持 webdav
struct StorageTarget {
  std::string name;
  std::string root_url;  // 例如 http://10.0.0.1:5244
  std::string base_path; // 规范化后的 base_path，首尾都带 /
  std::string username;
  std::string password;
};

Result<StorageTarget> load_storage_target(IDatabase& store, const std::string& storage_id);
// 已经拿到 storage_nodes 各字段时直接构造（下载竞速用，避免再查一次库）
Result<StorageTarget> make_storage_target(const std::string& storage_id, const std::string& name,
                                          const std::string& type, const std::string& endpoint,
                                          const std::string& config_json);
StorageOptions storage_options_for(const StorageTarget& target);

} // namespace web
} // namespace sb