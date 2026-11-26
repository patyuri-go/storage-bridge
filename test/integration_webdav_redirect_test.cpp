#include "webdav.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

namespace sb::webdav {
namespace {

// 只实现 302 -> 200 和单段 Range 的最小 HTTP 服务，用来测真实 openStream。
class RedirectServer {
public:
  RedirectServer() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        ::listen(listen_fd_, 8) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { accept_loop(); });
  }

  ~RedirectServer() { stop(); }

  int port() const { return port_; }

  void stop() {
    if (stop_.exchange(true)) {
      return;
    }
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  void accept_loop() {
    while (!stop_.load()) {
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        break;
      }
      handle(fd);
      ::close(fd);
    }
  }

  static void send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
      if (n <= 0) {
        return;
      }
      off += static_cast<std::size_t>(n);
    }
  }

  static void handle(int fd) {
    std::string request;
    char buf[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        return;
      }
      request.append(buf, static_cast<std::size_t>(n));
      if (request.size() > 65536) {
        return;
      }
    }

    std::string target;
    const std::size_t line_end = request.find("\r\n");
    if (line_end != std::string::npos) {
      const std::string line = request.substr(0, line_end);
      const std::size_t first = line.find(' ');
      const std::size_t second = line.find(' ', first + 1);
      if (first != std::string::npos && second != std::string::npos) {
        target = line.substr(first + 1, second - first - 1);
      }
    }

    if (target == "/redirect") {
      send_all(fd,
               "HTTP/1.1 302 Found\r\n"
               "Location: /file\r\n"
               "Content-Length: 5\r\n"
               "Connection: close\r\n"
               "\r\n"
               "HELLO");
      return;
    }

    std::string lower = request;
    for (char& c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const bool range = lower.find("range: bytes=4-") != std::string::npos;
    if (target == "/file" && range) {
      send_all(fd,
               "HTTP/1.1 206 Partial Content\r\n"
               "Content-Range: bytes 4-9/10\r\n"
               "Content-Length: 6\r\n"
               "Connection: close\r\n"
               "\r\n"
               "456789");
      return;
    }
    if (target == "/file") {
      send_all(fd,
               "HTTP/1.1 200 OK\r\n"
               "Content-Length: 10\r\n"
               "Connection: close\r\n"
               "\r\n"
               "0123456789");
      return;
    }
    send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  }

  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

std::string read_all(Stream& stream) {
  std::string out;
  char buf[1024];
  while (true) {
    const long long n = stream.read(buf, sizeof(buf));
    if (n < 0) {
      ADD_FAILURE() << "读取流失败";
      break;
    }
    if (n == 0) {
      break;
    }
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

TEST(IntegrationWebdavRedirect, Follows302AndDropsIntermediateBody) {
  RedirectServer server;
  ASSERT_GT(server.port(), 0);

  Url base;
  ASSERT_TRUE(parseUrl("http://127.0.0.1:" + std::to_string(server.port()), base));
  ClientOptions opts;
  opts.timeout = 5;

  Stream stream = openStream(base, "/redirect", {}, opts);
  ASSERT_EQ(stream.status, 200);
  EXPECT_EQ(read_all(stream), "0123456789");
  stream.reset();
}

TEST(IntegrationWebdavRedirect, SupportsRangeAfterDirect200) {
  RedirectServer server;
  ASSERT_GT(server.port(), 0);

  Url base;
  ASSERT_TRUE(parseUrl("http://127.0.0.1:" + std::to_string(server.port()), base));
  ClientOptions opts;
  opts.timeout = 5;

  Stream full = openStream(base, "/file", {}, opts);
  ASSERT_EQ(full.status, 200);
  EXPECT_EQ(read_all(full), "0123456789");
  full.reset();

  Stream part = openStream(base, "/file", {"Range: bytes=4-"}, opts);
  ASSERT_EQ(part.status, 206);
  EXPECT_EQ(part.headers["content-range"], "bytes 4-9/10");
  EXPECT_EQ(read_all(part), "456789");
  part.reset();
}

} // namespace
} // namespace sb::webdav