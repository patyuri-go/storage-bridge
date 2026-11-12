#pragma once

#include "result.hpp"

#include <string>

namespace sb {

class JobWorker;

enum class JobKind {
  ScanFolder,
  ScanFile,
};

// 任务基类：run() 只由 JobWorker 的消费线程调用。
// 任务内部需要展开子任务时调用 worker.spawn()，外部提交走 worker.submit()。
class Job {
public:
  virtual ~Job() = default;

  virtual JobKind kind() const = 0;

  // 日志/错误上下文用的短标签，例如 "scan-folder http://host/dav/a"
  virtual std::string label() const = 0;

  virtual Status run(JobWorker& worker) = 0;

  // 协作式让出：run() 里把它置 true，表示"这一轮只推进了一步，请把我放回队尾
  // 等下次调度"。worker 见到 true 不销毁任务、不计 completed，只重新入队。
  // 目前只有 ScanFolderJob 用：每提交一个子任务就让出，保证文件扫描不被饿死。
  virtual bool yielded() const {
    return false;
  }
};

} // namespace sb
