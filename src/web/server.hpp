#pragma once

#include "result.hpp"

#include <string>

namespace sb {

class PgStore;

namespace web {

// 依赖自检：构造一个 httplib::Server 会引用 libcpp-httplib.so 的符号，
// 能在 --version 里跑通就说明 HTTP 依赖已正确链接。
std::string library_info();

struct ServerOptions {
  std::string bind = "127.0.0.1";
  int port = 8080;
  // 常驻扫描 worker 的线程数（scan.file 只入队，扫描在这里后台跑）
  int scan_threads = 4;
  // 静态页面目录；挂到 /ui，GET / 会跳转过去
  std::string web_root = "web";
};

// 启动 HTTP 服务（阻塞）。全部业务走唯一入口：
//
//   POST /api   {"op": "<op>", "params": { ... }}
//            -> {"data": ...}                成功
//            -> {"data": null, "errors": []} 业务失败（HTTP 仍 200）
//
// 已支持的 op：storage.list / storage.get / storage.upsert / file.list / scan.file / health
// 另外 GET / 会 302 到 /ui/（文件树页面，静态资源来自 web_root）。
Status serve(const ServerOptions& opts, PgStore& store);

} // namespace web
} // namespace sb
