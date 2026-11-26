#include "web/storage_target.hpp"

#include "pg_store.hpp"
#include "util.hpp"

#include <json/json.h>

#include <cctype>
#include <memory>

namespace sb::web {

bool is_uuid(const std::string& s) {
  if (s.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::string strip_endpoint(std::string endpoint) {
  const auto scheme = endpoint.find("://");
  if (scheme != std::string::npos) {
    endpoint = endpoint.substr(scheme + 3);
  }
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  return endpoint;
}

Result<std::string> normalize_endpoint(std::string endpoint) {
  std::string v = strip_endpoint(trim(std::move(endpoint)));
  if (v.empty()) {
    return err(ErrorCode::InvalidArgument, "endpoint 不能为空（应为 ip:port 或域名）");
  }
  if (v.find('/') != std::string::npos) {
    return err(ErrorCode::InvalidArgument,
               "endpoint 只放 ip:port 或域名，路径请放在 config.base_path");
  }
  if (v.find(' ') != std::string::npos) {
    return err(ErrorCode::InvalidArgument, "endpoint 不能包含空格");
  }
  return v;
}

Result<std::string> normalize_base_path(std::string path) {
  std::string v = trim(std::move(path));
  if (v.empty()) {
    return err(ErrorCode::InvalidArgument, "config.base_path 不能为空（例如 /dav/tian_yi/My/BT/）");
  }
  if (v.front() != '/') {
    v.insert(v.begin(), '/');
  }
  std::string collapsed;
  collapsed.reserve(v.size() + 1);
  for (char c : v) {
    if (c == '/' && !collapsed.empty() && collapsed.back() == '/') {
      continue;
    }
    collapsed.push_back(c);
  }
  if (collapsed.back() != '/') {
    collapsed.push_back('/');
  }
  return collapsed;
}

Result<std::string> normalize_rel_path(std::string rel) {
  std::string v = trim(std::move(rel));
  if (v.empty() || v == "/") {
    return std::string("/");
  }
  if (v.front() != '/') {
    v.insert(v.begin(), '/');
  }
  std::string out;
  std::size_t i = 1;
  while (i < v.size()) {
    const std::size_t j = v.find('/', i);
    const std::size_t end = (j == std::string::npos) ? v.size() : j;
    const std::string seg = v.substr(i, end - i);
    i = end + 1;
    if (seg.empty() || seg == ".") {
      continue;
    }
    if (seg == "..") {
      return err(ErrorCode::InvalidArgument, "path 不能包含 ..");
    }
    out += '/';
    out += seg;
  }
  return out.empty() ? std::string("/") : out;
}

Result<std::string> build_storage_path(const std::string& base_path, const std::string& rel) {
  if (rel.empty() || rel == "/") {
    return base_path;
  }
  return cat(base_path, rel.substr(1), "/");
}

Error map_conflict(Error e) {
  if (e.detail == "23505") {
    e.code = ErrorCode::Conflict;
    e.message = "name 已存在（唯一约束冲突）";
  }
  return e;
}

Result<StorageTarget> load_storage_target(IDatabase& store, const std::string& storage_id) {
  auto node = store.get_storage(storage_id);
  if (!node) {
    return node.error();
  }
  return make_storage_target(node->storage_id, node->name, node->type, node->endpoint,
                             node->config_json);
}

Result<StorageTarget> make_storage_target(const std::string& storage_id, const std::string& name,
                                          const std::string& type, const std::string& endpoint,
                                          const std::string& config_json) {
  if (type != "webdav") {
    return err(ErrorCode::InvalidArgument, "目前只支持 type=\"webdav\"", "", storage_id);
  }

  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value cfg;
  std::string errs;
  if (!reader->parse(config_json.data(), config_json.data() + config_json.size(), &cfg, &errs)) {
    return err(ErrorCode::Internal, "storage.config 不是合法 JSON", trim(errs), storage_id);
  }
  for (const char* key : {"username", "password", "base_path"}) {
    if (!cfg.isMember(key) || !cfg[key].isString()) {
      return err(ErrorCode::Internal, cat("storage.config 缺少 ", key), "", storage_id);
    }
  }

  auto base_path = normalize_base_path(cfg["base_path"].asString());
  if (!base_path) {
    return base_path.error();
  }

  StorageTarget out;
  out.name = name;
  // webdav 默认走 http://（表里没有 protocol 字段）
  out.root_url = cat("http://", strip_endpoint(endpoint));
  out.base_path = *base_path;
  out.username = cfg["username"].asString();
  out.password = cfg["password"].asString();
  return out;
}

StorageOptions storage_options_for(const StorageTarget& target) {
  StorageOptions opts;
  opts.user = target.username;
  opts.password = target.password;
  opts.user_agent = "storage-bridge/0.1";
  return opts;
}

} // namespace sb::web