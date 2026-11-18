#pragma once

#include <memory>
#include <string>

namespace sb {

class DbPool;
class JobWorker;

namespace web {

// 进程级共享状态：控制器从 AppContext 取依赖，避免全局散落的指针。
// 生命周期由 main 保证：先构造各个组件，再调用 web::serve()。
// DbPool 用 shared_ptr 持有：后台下载探测线程会捕获一份，保证服务关停后
// 晚到的写库请求不会踩到已销毁对象（向已停止的池提交会失败并丢弃）。
struct AppContext {
  std::shared_ptr<DbPool> db;
  JobWorker* jobs = nullptr;
  std::string web_root;

  static AppContext& instance();
};

} // namespace web
} // namespace sb