#pragma once

#include "result.hpp"
#include "util.hpp"
#include "webdav.hpp"

#include <functional>
#include <memory>
#include <string>

namespace sb {

struct StorageOptions {
  std::string user; // HTTP Basic
  std::string password;
  std::string user_agent = "storage-bridge/0.1";
  bool ssl_verify = true;
  int timeout = 60; // 连接/停滞超时（秒）
};

// WebDAV 存储端封装（src/webdav.cpp）。
// 本类线程安全：每次请求独立连接，不持有跨请求状态。
class Storage {
public:
  static Result<std::unique_ptr<Storage>> create(const StorageOptions& opts);

  ~Storage();

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  // 列 base 下的对象；回调返回 Status 以便提前终止并向调用方传播错误。
  Status list(const std::string& base, const std::function<Status(const ObjectRef&)>& cb);
  Result<ObjectRef> stat(const std::string& uri);

private:
  friend class Reader;

  Storage() = default;

  webdav::ClientOptions webdav_options() const;
  // 从 URI 解析出带凭据的 base；失败返回 InvalidArgument
  Result<webdav::Url> webdav_base(const std::string& uri) const;

  StorageOptions opts_;
};

// 单个对象的读取器：一个线程、一个对象、一个实例（非线程安全）。
class Reader {
public:
  static Result<std::unique_ptr<Reader>> open(Storage& storage, const std::string& uri);

  ~Reader();

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // 按偏移读，返回实际字节数，0 表示 EOF。
  // 顺序读（offset == 上次结束位置）会复用同一条连接；
  // 跳跃读会用 Range 重新发一次 GET。
  Result<std::size_t> read_at(std::int64_t offset, void* buf, std::size_t len);

private:
  Reader() = default;

  Status open_stream(std::int64_t offset);

  webdav::Url base_;
  std::string path_;
  webdav::ClientOptions opts_;
  webdav::Stream stream_;
  std::int64_t pos_ = 0;  // stream_ 当前位置
  std::int64_t skip_ = 0; // 服务器忽略 Range 时要丢弃的前导字节数
  std::string uri_;
};

} // namespace sb