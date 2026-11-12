#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// 自写 WebDAV 客户端（asio + OpenSSL，HTTP/1.1，Basic 认证）。
//
// 为什么不用 davix：davix 0.8.5 的 PROPFIND body 是源码里硬编码的常量
// （src/fileops/davmeta.cpp 的 stat_listing，含
// <D:resourcetype><D:collection/></D:resourcetype>），没有任何 API 可定制；
// 而 openlist 对带这个结构的 body 直接返回 404。davix 的
// setClientLoginPassword 在 libcurl 后端上也完全无效（既不预先发
// Authorization 也不做 401 重试）。因此 WebDAV 一侧自实现，davix 只保留给 S3。
// 详见 docs/feasibility.md「openlist 兼容性」。

namespace sb::webdav {

// 解析后的 WebDAV URL（支持在配置里指定子路径作为媒体库根）
struct Url {
  bool https = false;
  std::string host;
  std::string port;
  std::string user;
  std::string pass;
  std::string path; // 服务器绝对路径，以 '/' 开头，保持百分号编码
};

// 解析 http(s)://[user:pass@]host[:port][/path]
bool parseUrl(const std::string& url, Url& out);

// 判断字符串是否为 http/https URL
bool isWebdavUrl(const std::string& url);

// 百分号解码（用于把服务器返回的 href 还原成可读名字）
std::string percentDecode(const std::string& text);

// 由 base 的 scheme/host/port 与已编码路径拼出完整 URL
std::string buildUrl(const Url& base, const std::string& encoded_path);

// RFC 1123 时间（Wed, 24 Jun 2026 03:17:54 GMT）→ epoch 秒；失败返回 0
std::int64_t httpDateToEpoch(const std::string& text);

struct ClientOptions {
  int timeout = 30; // 单次请求的收发超时（秒）
  bool ssl_verify = true;
  std::string user_agent = "storage-bridge/0.1";
};

struct Response {
  int status = 0; // 0 = 连接/传输失败
  std::string body;
  std::map<std::string, std::string> headers; // key 已转小写
  std::string header(const std::string& key) const;
};

// 流式读取句柄：保持与远端的连接，逐块读响应体（不整块进内存）
struct Stream {
  std::function<long long(char* buf, std::size_t n)> read;
  std::function<void()> close;
  int status = 0;
  std::map<std::string, std::string> headers;

  void reset() {
    if (close) {
      close();
    }
    read = nullptr;
    close = nullptr;
    status = 0;
    headers.clear();
  }
};

// 打开一个 GET 流（透传 Range 等额外头，自动跟随 302 到签名 URL）
Stream openStream(const Url& base, const std::string& path, const std::vector<std::string>& extra,
                  const ClientOptions& opts = {});

// 同步 HTTP/1.1 客户端：一次请求一条连接（Connection: close）
class Client {
public:
  Client(Url base, ClientOptions opts = {}) : base_(std::move(base)), opts_(std::move(opts)) {}

  Response request(const std::string& method, const std::string& path,
                   const std::vector<std::string>& extra, const std::string& body);

  // PROPFIND；body 固定为最小集合（resourcetype + getcontentlength + getlastmodified）
  Response propfind(const std::string& path, int depth);

private:
  Url base_;
  ClientOptions opts_;
};

struct Resource {
  std::string path; // 服务器绝对路径，保持百分号编码，可直接用于 Client 请求
  bool collection = false;
  std::int64_t size = 0;
  std::int64_t mtime = 0; // epoch 秒，0 = 未知
};

// 解析 PROPFIND 207 XML 响应
std::vector<Resource> parsePropfind(const std::string& xml);

} // namespace sb::webdav
