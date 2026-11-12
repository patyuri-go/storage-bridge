#pragma once

#include "result.hpp"
#include "util.hpp"
#include "webdav.hpp"

#include <davix.hpp>

#include <functional>
#include <memory>
#include <string>

namespace sb {

struct StorageOptions {
  std::string user; // HTTP Basic
  std::string password;
  std::string cert_key; // PEM private key（与 cert 相同时可只填一个）
  std::string cert;     // PEM 证书 / 拼接式 proxy
  std::string cert_password;
  std::string bearer_token; // 直接塞 Authorization: Bearer ...
  std::string s3_access_key;
  std::string s3_secret_key;
  std::string s3_region; // 必填才能走 SigV4（否则 fallback SigV2）
  std::string s3_token;  // 临时凭据
  std::string user_agent = "storage-bridge/0.1";
  bool ssl_verify = true;
  int connect_timeout = 15;
  int operation_timeout = 60;
  int retries = 2;
  int retry_delay = 2;
};

// 对象存储封装，按 URI 的 scheme 分流：
//   http://  https://  → 自写 WebDAV 客户端（src/webdav.cpp）
//   其它（s3:// 等）   → davix
//
// WebDAV 一侧不用 davix 的原因见 src/webdav.hpp 顶部注释。
// 本类线程安全：WebDAV 路径每次请求独立连接，davix 路径的 Context 内部有连接池。
class Storage {
public:
  // 证书加载失败等构造期错误通过工厂返回，不抛异常
  static Result<std::unique_ptr<Storage>> create(const StorageOptions& opts);

  ~Storage();

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  // 列 base 下的对象；回调返回 Result 以便提前终止并向调用方传播错误。
  Status list(const std::string& base, const std::function<Status(const ObjectRef&)>& cb);
  Result<ObjectRef> stat(const std::string& uri);

  // 判断某个 URI 是否走 WebDAV 分支
  static bool is_webdav(const std::string& uri);

  Davix::Context& context() {
    return ctx_;
  }
  Davix::RequestParams& params() {
    return params_;
  }

private:
  friend class Reader;

  Storage() = default;

  webdav::ClientOptions webdav_options() const;
  // 从 URI 解析出带凭据的 base；失败返回 InvalidArgument
  Result<webdav::Url> webdav_base(const std::string& uri) const;

  Status list_webdav(const std::string& root, const std::function<Status(const ObjectRef&)>& cb);
  Result<ObjectRef> stat_webdav(const std::string& uri);

  Davix::Context ctx_;
  Davix::RequestParams params_;
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

  Storage* storage_ = nullptr;

  // davix 分支
  std::unique_ptr<Davix::File> file_;

  // webdav 分支
  bool webdav_ = false;
  webdav::Url base_;
  std::string path_;
  webdav::ClientOptions opts_;
  webdav::Stream stream_;
  std::int64_t pos_ = 0;  // stream_ 当前位置
  std::int64_t skip_ = 0; // 服务器忽略 Range 时要丢弃的前导字节数
  std::string uri_;
};

} // namespace sb
