#include "job_worker.hpp"

#include "util.hpp"

#include <exception>
#include <utility>

namespace sb {

JobWorker::JobWorker(std::size_t threads, std::size_t max_queue)
    : thread_count_(threads == 0 ? 1 : threads), max_queue_(max_queue == 0 ? 1 : max_queue) {}

JobWorker::~JobWorker() {
  stop();
}

Status JobWorker::start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) {
    return err(ErrorCode::Conflict, "JobWorker 已经启动");
  }
  started_ = true;
  accepting_ = true;
  stopping_ = false;
  threads_.reserve(thread_count_);
  for (std::size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back(&JobWorker::loop, this);
  }
  return {};
}

Status JobWorker::submit(std::unique_ptr<Job> job) {
  return push(std::move(job), false);
}

Status JobWorker::spawn(std::unique_ptr<Job> job) {
  return push(std::move(job), true);
}

std::deque<std::unique_ptr<Job>>& JobWorker::queue_for(JobKind kind) {
  return kind == JobKind::ScanFile ? files_ : dirs_;
}

Status JobWorker::push(std::unique_ptr<Job> job, bool internal) {
  if (!job) {
    return err(ErrorCode::InvalidArgument, "任务不能为空");
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (threads_.empty()) {
      return err(ErrorCode::Conflict, "JobWorker 尚未启动");
    }
    if (!internal && !accepting_) {
      return err(ErrorCode::Conflict, "JobWorker 已停止接收任务");
    }
    if (files_.size() + dirs_.size() >= max_queue_) {
      return err(ErrorCode::Busy, "任务队列已满", std::to_string(max_queue_));
    }
    queue_for(job->kind()).push_back(std::move(job));
  }
  cv_.notify_one();
  return {};
}

void JobWorker::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    accepting_ = false;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  threads_.clear();
}

JobWorkerStats JobWorker::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

std::unique_ptr<Job> JobWorker::pop_locked() {
  if (!files_.empty()) {
    std::unique_ptr<Job> job = std::move(files_.front());
    files_.pop_front();
    return job;
  }
  if (!dirs_.empty()) {
    std::unique_ptr<Job> job = std::move(dirs_.front());
    dirs_.pop_front();
    return job;
  }
  return nullptr;
}

void JobWorker::loop() {
  for (;;) {
    std::unique_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !files_.empty() || !dirs_.empty(); });
      job = pop_locked();
      if (!job) {
        break; // stopping_ 且两条队列都已清空
      }
    }

    const JobKind kind = job->kind();
    const std::string label = job->label();
    Status result;
    try {
      result = job->run(*this);
    } catch (const std::exception& e) {
      result = err(ErrorCode::Internal, "任务未捕获异常", e.what(), label);
    } catch (...) {
      result = err(ErrorCode::Internal, "任务未捕获未知异常", "", label);
    }

    std::string failure;
    bool requeued = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (result && job && job->yielded()) {
        // 协作式让出：不销毁、不计完成，放回队尾等下次调度。
        // 这里不受 max_queue_ 限制——只是把刚才取出来的那一条放回去。
        queue_for(kind).push_back(std::move(job));
        ++stats_.yields;
        requeued = true;
      } else if (result) {
        ++stats_.completed;
      } else {
        ++stats_.failed;
        failure = result.error().to_string();
      }
    }
    if (requeued) {
      cv_.notify_one();
    }
    if (!failure.empty()) {
      log(LogLevel::Error, cat("job failed: ", failure));
    }
  }
}

} // namespace sb
