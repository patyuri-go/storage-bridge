#pragma once

#include "job.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sb {

struct JobWorkerStats {
  std::uint64_t completed = 0; // 跑完的任务数（让出的那几轮不算）
  std::uint64_t failed = 0;
  std::uint64_t yields = 0; // 协作式让出次数
};

// 多线程 worker + 双队列。
//
// 队列分成「文件扫描」和「目录扫描」两条：
//   - 取任务时永远优先 files_，目录任务只在没有文件任务时才跑
//   - ScanFolderJob 每向队列塞一个子任务就让出（见 Job::yielded），
//     所以展开大目录不会把 worker 长时间占住，文件扫描能立刻插进来
//
// 队列总长度（files_ + dirs_）上限 max_queue，超了 submit/spawn 返回
// ErrorCode::Busy。让出后重新入队的任务不受此限制（净增长为 0）。
//
// stop() 会先处理完队列再退出。
class JobWorker {
public:
  explicit JobWorker(std::size_t threads = 1, std::size_t max_queue = 1000);
  ~JobWorker();

  JobWorker(const JobWorker&) = delete;
  JobWorker& operator=(const JobWorker&) = delete;

  Status start();

  // 外部提交：stop() 之后拒绝新任务，队列满返回 Busy
  Status submit(std::unique_ptr<Job> job);

  // 任务内部展开子任务：drain（stop 后仍在跑完队列）期间仍然允许
  Status spawn(std::unique_ptr<Job> job);

  // 停止接收外部任务，把队列处理完后 join；未启动时为空操作
  void stop();

  JobWorkerStats stats() const;

private:
  Status push(std::unique_ptr<Job> job, bool internal);
  std::deque<std::unique_ptr<Job>>& queue_for(JobKind kind);
  std::unique_ptr<Job> pop_locked(); // 调用方持锁；files_ 优先
  void loop();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::unique_ptr<Job>> files_;
  std::deque<std::unique_ptr<Job>> dirs_;
  std::vector<std::thread> threads_;

  const std::size_t thread_count_;
  const std::size_t max_queue_;

  bool started_ = false;
  bool accepting_ = false;
  bool stopping_ = false;

  JobWorkerStats stats_;
};

} // namespace sb
