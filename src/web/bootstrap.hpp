#pragma once

#include "result.hpp"

#include <string>

namespace sb {

class DbPool;
class JobWorker;

namespace web {

struct ServerOptions {
  std::string bind = "127.0.0.1";
  int port = 8080;
  // 静态页面目录；挂到 /ui，GET / 会跳转过去
  std::string web_root = "web";
};

// 启动 HTTP 服务（阻塞，直到 app().quit()）。
//
// 路由（由 HttpController 在链接期静态注册）：
//   GET  /api/storages               资源库列表（裸数组）
//   GET  /api/storages/{id}          单个资源库
//   POST /api/storages               create=true 新建 / create=false 更新
//   GET  /api/storages/{id}/files    列目录（?path=/a/b）
//   POST /api/storages/{id}/scans    入队扫描，202
//   GET  /api/health                 200 ok / 503 degraded
//
// 另外 GET / 会 302 到 /ui/（文件树页面，静态资源来自 web_root）。
Status serve(const ServerOptions& opts, DbPool& db, JobWorker& jobs);

} // namespace web
} // namespace sb