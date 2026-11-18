#pragma once

#include <string>

namespace sb {

class DbPool;
class JobWorker;

namespace web {

// 进程级共享状态：控制器从 AppContext 取依赖，避免全局散落的指针。
// 生命周期由 main 保证：先构造各个组件，再调用 web::serve()。
struct AppContext {
  DbPool* db = nullptr;
  JobWorker* jobs = nullptr;
  std::string web_root;

  static AppContext& instance();
};

} // namespace web
} // namespace sb