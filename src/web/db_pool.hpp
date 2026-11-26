#pragma once

#include "config.hpp"
#include "result.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sb {

class IDatabase;

// 供 submit() 检测「池内任务再等池」的自死锁
extern thread_local bool t_in_pool_task;

// 固定大小的 PostgreSQL 连接池：每个 worker 线程独占一条 SOCI 连接。
// 扫描线程与 Web 请求都通过它访问数据库。
//
// 用法：submit(lambda) 里的 lambda 形参是 IDatabase&，返回值由 future 携带。
// 注意：池内任务里不要 submit(...).get()，否则会把池耗尽并死锁。
class DbPool {
public:
  static Result<std::shared_ptr<DbPool>> create(const DbConfig& cfg, int threads);

  // 测试/自定义后端用：直接接管一组已构造好的连接（每个线程一个）。
  static Result<std::shared_ptr<DbPool>> create_with_stores(
      std::vector<std::unique_ptr<IDatabase>> stores);

  ~DbPool();

  DbPool(const DbPool&) = delete;
  DbPool& operator=(const DbPool&) = delete;

  // 停止接收新任务，丢弃队列中未开始的任务，等待正在执行的任务结束
  void stop();

  template <typename F>
  auto submit(F&& fn) -> std::future<std::invoke_result_t<F, IDatabase&>> {
    if (t_in_pool_task) {
      throw std::logic_error("DbPool::submit 不能在池内任务中调用");
    }
    using R = std::invoke_result_t<F, IDatabase&>;
    auto task = std::make_shared<std::packaged_task<R(IDatabase&)>>(std::forward<F>(fn));
    std::future<R> future = task->get_future();
    if (!enqueue([task](IDatabase& db) { (*task)(db); })) {
      throw std::runtime_error("DbPool 已停止");
    }
    return future;
  }

private:
  DbPool() = default;

  void start();
  // 返回 false 表示池已停止
  bool enqueue(std::function<void(IDatabase&)> task);
  void worker_main(std::size_t index);

  std::vector<std::unique_ptr<IDatabase>> stores_;
  std::deque<std::function<void(IDatabase&)>> queue_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::thread> threads_;
  bool stopping_ = false;
};

} // namespace sb