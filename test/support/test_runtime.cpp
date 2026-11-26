#include "support/test_runtime.hpp"

#include "job_worker.hpp"
#include "pg_store.hpp"
#include "support/http_client.hpp"
#include "support/mock_database.hpp"
#include "support/mock_webdav.hpp"
#include "util.hpp"
#include "web/app_context.hpp"
#include "web/bootstrap.hpp"
#include "web/db_pool.hpp"

#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sb::test {

namespace {

int reserve_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  ::close(fd);
  return ntohs(addr.sin_port);
}

} // namespace

TestRuntime& TestRuntime::instance() {
  static TestRuntime runtime;
  return runtime;
}

TestRuntime::~TestRuntime() = default;

void TestRuntime::install_defaults() {
  using ::testing::_;
  using ::testing::Return;

  ON_CALL(*db_, ping()).WillByDefault(Return(Status{}));
  ON_CALL(*db_, upsert_storage(_))
      .WillByDefault(Return(Result<StorageNodeResult>(err(ErrorCode::Internal, "未配置"))));
  ON_CALL(*db_, get_storage(_))
      .WillByDefault(Return(Result<StorageNodeRecord>(err(ErrorCode::NotFound, "storage 不存在"))));
  ON_CALL(*db_, list_storages())
      .WillByDefault(Return(Result<std::vector<StorageNodeRecord>>(std::vector<StorageNodeRecord>{})));
  ON_CALL(*db_, record_file(_, _, _, _, _))
      .WillByDefault(Return(Result<FileRecordOutcome>(err(ErrorCode::Internal, "未配置"))));
  ON_CALL(*db_, location_matches_size(_, _, _))
      .WillByDefault(Return(Result<bool>(false)));
  ON_CALL(*db_, list_entity_locations(_))
      .WillByDefault(Return(Result<std::vector<EntityLocation>>(std::vector<EntityLocation>{})));
  ON_CALL(*db_, lookup_locations(_, _))
      .WillByDefault(Return((Result<std::map<std::string, std::string>>(
          std::map<std::string, std::string>{}))));
  ON_CALL(*db_, mark_location_ok(_, _, _)).WillByDefault(Return(Status{}));
  ON_CALL(*db_, mark_location_failed(_, _, _)).WillByDefault(Return(Status{}));

  ON_CALL(*webdav_, propfind(_, _, _, _)).WillByDefault(Return(webdav::Response{}));
  ON_CALL(*webdav_, open_stream(_, _, _, _)).WillByDefault(Return(webdav::Stream{}));
}

void TestRuntime::reset_mocks() {
  if (db_ != nullptr) {
    testing::Mock::VerifyAndClear(db_);
  }
  if (webdav_ != nullptr) {
    testing::Mock::VerifyAndClear(webdav_.get());
  }
  install_defaults();
}

Status TestRuntime::start() {
  if (started_) {
    return {};
  }

  auto database = std::make_unique<testing::NiceMock<MockDatabase>>();
  db_ = database.get();
  webdav_ = std::make_shared<testing::NiceMock<MockWebDav>>();
  install_defaults();

  std::vector<std::unique_ptr<IDatabase>> stores;
  stores.push_back(std::move(database));
  auto pool = DbPool::create_with_stores(std::move(stores));
  if (!pool) {
    db_ = nullptr;
    return pool.error();
  }
  pool_ = *pool;

  jobs_ = std::make_unique<JobWorker>(1, 1000);
  if (Status st = jobs_->start(); !st) {
    jobs_.reset();
    pool_->stop();
    pool_.reset();
    db_ = nullptr;
    return st.error();
  }

  port_ = reserve_port();
  if (port_ == 0) {
    jobs_->stop();
    jobs_.reset();
    pool_->stop();
    pool_.reset();
    db_ = nullptr;
    return err(ErrorCode::Network, "无法分配测试服务端口");
  }

  web::ServerOptions opts;
  opts.bind = "127.0.0.1";
  opts.port = port_;
  opts.web_root = std::string(SB_SOURCE_DIR) + "/web";
  opts.install_signal_handlers = false;

  web::AppContext::instance().webdav = webdav_;

  serve_returned_ = false;
  serve_error_.clear();
  server_thread_ = std::thread([this, opts, pool = pool_]() {
    const Status st = web::serve(opts, pool, *jobs_);
    std::lock_guard<std::mutex> lock(serve_mu_);
    serve_returned_ = true;
    if (!st) {
      serve_error_ = st.error().to_string();
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    bool returned = false;
    std::string detail;
    {
      std::lock_guard<std::mutex> lock(serve_mu_);
      returned = serve_returned_;
      detail = serve_error_;
    }
    if (returned) {
      stop();
      return err(ErrorCode::Io, "测试服务启动后立即退出", detail);
    }

    if (http_get(port_, "/api/health").status == 200) {
      started_ = true;
      return {};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  stop();
  return err(ErrorCode::Network, "测试服务启动超时",
             cat("127.0.0.1:", std::to_string(port_)));
}

void TestRuntime::stop() {
  if (server_thread_.joinable()) {
    drogon::app().quit();
    server_thread_.join();
  }
  if (jobs_ != nullptr) {
    jobs_->stop();
    jobs_.reset();
  }
  if (pool_ != nullptr) {
    pool_->stop();
    pool_.reset();
  }
  web::AppContext::instance().db.reset();
  web::AppContext::instance().jobs = nullptr;
  web::AppContext::instance().webdav.reset();
  webdav_.reset();
  db_ = nullptr;
  started_ = false;
}

MockDatabase& TestRuntime::db() { return *db_; }

MockWebDav& TestRuntime::webdav() { return *webdav_; }

} // namespace sb::test