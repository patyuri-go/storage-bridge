#pragma once

#include <map>
#include <string>
#include <vector>

namespace sb::test {

struct HttpResponse {
  long status = 0; // 0 = 连接/传输失败
  std::string body;
  std::map<std::string, std::string> headers; // key 小写
};

// 测试用最小 HTTP 客户端：不走 Drogon，避免和被测服务共用事件循环。
HttpResponse http_request(int port, const std::string& method, const std::string& path,
                          const std::string& body = {},
                          const std::vector<std::string>& headers = {});

inline HttpResponse http_get(int port, const std::string& path,
                             const std::vector<std::string>& headers = {}) {
  return http_request(port, "GET", path, {}, headers);
}

inline HttpResponse http_post_json(int port, const std::string& path, const std::string& body) {
  return http_request(port, "POST", path, body, {"Content-Type: application/json"});
}

} // namespace sb::test