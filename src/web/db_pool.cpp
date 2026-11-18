#include "web/db_pool.hpp"

#include "pg_store.hpp"
#include "util.hpp"

#include <trantor/utils/Logger.h>

#include <utility>

namespace sb {

thread_local bool t_in_pool_task = false;

Result<std::shared_ptr<DbPool>> DbPool::create(const DbConfig& cfg, int threads) {
  if (threads < 1) {
    threads = 1;
  }

  // 先建满所有连接再启动线程：任何一条连不上都在监听端口前失败
  std::vector<std::unique_ptr<PgStore>> stores;
  stores.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    auto store = PgStore::connect(cfg);
    if (!store) {
      return store.error();
    }
    stores.push_back(std::move(*store));
  }

  std::shared_ptr<DbPool> self(new DbPool());
  self->stores_ = std::move(stores);
  self->start();
  LOG_INFO << cat("db pool started: ", std::to_string(threads), " connections");
  return self;
}

DbPool::~DbPool() { stop(); }

void DbPool::start() {
  threads_.reserve(stores_.size());
  for (std::size_t i = 0; i < stores_.size(); ++i) {
    threads_.emplace_back([this, i] { worker_main(i); });
  }
}

bool DbPool::enqueue(std::function<void(PgStore&)> task) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) {
      return false;
    }
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void DbPool::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ && threads_.empty()) {
      return;
    }
    stopping_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
}

void DbPool::worker_main(std::size_t index) {
  t_in_pool_task = true;
  for (;;) {
    std::function<void(PgStore&)> task;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) {
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      task(*stores_[index]);
    } catch (const std::exception& e) {
      LOG_ERROR << cat("db pool task failed: ", e.what());
    }
  }
}

} // namespace sb