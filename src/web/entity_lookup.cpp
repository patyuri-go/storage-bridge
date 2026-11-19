#include "web/entity_lookup.hpp"

#include "scan_job.hpp"
#include "storage.hpp"
#include "util.hpp"

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sb::web {
namespace {

constexpr int kLookupThreads = 8;
constexpr std::chrono::seconds kLookupDeadline{10};
constexpr std::size_t kMaxErrors = 20;

// 输入路径要先 stat 才知道是文件还是目录；子项的类型由上层 PROPFIND 直接给出
struct Task {
  bool is_stat = false;
  std::string rel;
  std::string url;
};

struct WalkState {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Task> queue;
  std::size_t active = 0;
  std::set<std::string> files;
  std::vector<std::pair<std::string, std::string>> errors;
  bool stop = false;
  bool input_ok = false;
};

std::string error_text(const Error& e) {
  if (e.detail.empty()) {
    return e.message;
  }
  return cat(e.message, " (", e.detail, ")");
}

// 拼到 base_path 下的完整 URL；不加尾斜杠（文件带尾斜杠有些服务端会 404）
std::string input_url(const StorageTarget& target, const std::string& rel) {
  if (rel == "/" || rel.empty()) {
    return cat(target.root_url, target.base_path);
  }
  return cat(target.root_url, target.base_path, rel.substr(1));
}

// 调用方必须持有 state.mu
void add_error(WalkState& st, const std::string& rel, const Error& e) {
  st.errors.emplace_back(rel, error_text(e));
}

void run_task(const std::shared_ptr<Storage>& storage, const std::shared_ptr<WalkState>& state,
              const Task& task) {
  if (task.is_stat) {
    auto info = storage->stat(task.url);
    if (!info) {
      // 有的服务端要求目录 URL 带尾斜杠，失败后补一个再试
      info = storage->stat(cat(task.url, "/"));
      if (!info) {
        std::lock_guard<std::mutex> lock(state->mu);
        add_error(*state, task.rel, info.error());
        return;
      }
    }

    std::lock_guard<std::mutex> lock(state->mu);
    state->input_ok = true;
    if (!info->is_dir) {
      state->files.insert(task.rel); // 输入本身就是文件
      return;
    }
    state->queue.push_back(Task{false, task.rel, task.url});
    return;
  }

  std::vector<Task> dirs;
  std::set<std::string> files;
  Status listed = storage->list(task.url, [&](const ObjectRef& ref) -> Status {
    if (ref.name.empty()) {
      return {};
    }
    const std::string key = join_key(task.rel, ref.name);
    if (ref.is_dir) {
      dirs.push_back(Task{false, key, ref.uri});
    } else {
      files.insert(key);
    }
    return {};
  });

  std::lock_guard<std::mutex> lock(state->mu);
  state->files.insert(files.begin(), files.end());
  for (auto& dir : dirs) {
    state->queue.push_back(std::move(dir));
  }
  if (!listed) {
    add_error(*state, task.rel, listed.error());
  }
}

// 空闲判定：队列空且没有在跑的任务。push 都发生在 active-- 之前，
// 所以不会出现「队列空、active==0，但还有活没入队」的窗口。
void worker_loop(const std::shared_ptr<Storage>& storage, const std::shared_ptr<WalkState>& state) {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(state->mu);
      state->cv.wait(lock, [&] {
        return state->stop || !state->queue.empty() || state->active == 0;
      });
      if (state->stop || state->queue.empty()) {
        return;
      }
      task = std::move(state->queue.front());
      state->queue.pop_front();
      ++state->active;
    }

    try {
      run_task(storage, state, task);
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(state->mu);
      add_error(*state, task.rel, err(ErrorCode::Internal, "遍历异常", e.what()));
    } catch (...) {
      std::lock_guard<std::mutex> lock(state->mu);
      add_error(*state, task.rel, err(ErrorCode::Internal, "遍历异常"));
    }

    {
      std::lock_guard<std::mutex> lock(state->mu);
      --state->active;
    }
    state->cv.notify_all();
  }
}

} // namespace

LookupWalk walk_storage_paths(const StorageTarget& target,
                              const std::vector<std::string>& rel_paths) {
  LookupWalk out;

  StorageOptions opts = storage_options_for(target);
  // 兜底：卡住的 PROPFIND 最多再拖 10 秒（默认是 60 秒）
  opts.timeout = static_cast<int>(kLookupDeadline.count());

  auto storage = Storage::create(opts);
  if (!storage) {
    out.errors.emplace_back("/", error_text(storage.error()));
    out.error_count = 1;
    return out;
  }
  std::shared_ptr<Storage> shared(std::move(*storage));

  auto state = std::make_shared<WalkState>();
  for (const auto& rel : rel_paths) {
    state->queue.push_back(Task{true, rel, input_url(target, rel)});
  }

  std::vector<std::thread> workers;
  workers.reserve(kLookupThreads);
  for (int i = 0; i < kLookupThreads; ++i) {
    try {
      workers.emplace_back(worker_loop, shared, state);
    } catch (const std::exception& e) {
      LOG_WARN << cat("lookup worker 创建失败: ", e.what());
    }
  }
  if (workers.empty()) {
    out.truncated = true;
    out.errors.emplace_back("/", "无法创建遍历线程");
    out.error_count = 1;
    return out;
  }

  bool finished = false;
  {
    std::unique_lock<std::mutex> lock(state->mu);
    finished = state->cv.wait_until(lock, std::chrono::steady_clock::now() + kLookupDeadline, [&] {
      return state->queue.empty() && state->active == 0;
    });
    state->stop = true;
    out.any_input_ok = state->input_ok;
    out.truncated = !finished;
    out.files.assign(state->files.begin(), state->files.end());
    out.errors.assign(state->errors.begin(), state->errors.end());
  }
  state->cv.notify_all();

  // 到点不等在途的 PROPFIND：线程自己会退出（state/storage 由 shared_ptr 续命）
  for (auto& thread : workers) {
    if (thread.joinable()) {
      thread.detach();
    }
  }

  std::sort(out.errors.begin(), out.errors.end());
  out.error_count = out.errors.size();
  if (out.errors.size() > kMaxErrors) {
    out.errors.resize(kMaxErrors);
  }
  return out;
}

} // namespace sb::web
