#pragma once

#include "result.hpp"

#include <gmock/gmock.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sb {
class IDatabase;
class JobWorker;
class DbPool;
} // namespace sb

namespace sb::test {

class MockDatabase;
class MockWebDav;

// 进程内测试运行时：整个测试二进制只启动一次 Drogon 服务。
// DB / WebDAV 都是 gMock 注入，每个用例通过 reset_mocks() 重置期望和默认动作。
class TestRuntime {
public:
  static TestRuntime& instance();
  ~TestRuntime();

  // 启动服务并等到 /api/health 返回 200；失败返回错误（调用方负责退出）。
  Status start();
  void stop();

  MockDatabase& db();
  MockWebDav& webdav();
  int port() const { return port_; }

  // 验证并清空上个用例的 EXPECT_CALL，再安装安全的默认动作。
  void reset_mocks();

private:
  TestRuntime() = default;

  void install_defaults();

  MockDatabase* db_ = nullptr;
  std::shared_ptr<MockWebDav> webdav_;
  std::shared_ptr<DbPool> pool_;
  std::unique_ptr<JobWorker> jobs_;
  std::thread server_thread_;
  std::mutex serve_mu_;
  bool serve_returned_ = false;
  std::string serve_error_;
  int port_ = 0;
  bool started_ = false;
};

} // namespace sb::test