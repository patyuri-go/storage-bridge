#include "web/bootstrap.hpp"

#include "job_worker.hpp"
#include "util.hpp"
#include "web/app_context.hpp"
#include "web/db_pool.hpp"
#include "web/json_util.hpp"

#include <drogon/drogon.h>
#include <json/json.h>

#include <trantor/utils/Logger.h>

#include <csignal>
#include <filesystem>
#include <string>
#include <utility>

namespace sb::web {

namespace {

// /ui/ 下的静态文件：规范化后必须仍在 web_root 里，防止 ../ 穿越
drogon::HttpResponsePtr serve_static(const std::string& root, const std::string& path,
                                     const drogon::HttpRequestPtr& req) {
  std::string rel = path.substr(3); // 去掉 "/ui"
  if (rel.empty() || rel == "/") {
    rel = "/index.html";
  }
  while (!rel.empty() && rel.front() == '/') {
    rel.erase(rel.begin());
  }

  std::error_code ec;
  const std::filesystem::path full =
      std::filesystem::weakly_canonical(std::filesystem::path(root) / rel, ec);
  if (ec) {
    return error_response(err(ErrorCode::InvalidArgument, "非法路径", ec.message()));
  }
  const std::string full_str = full.string();
  if (full_str != root && full_str.rfind(root + "/", 0) != 0) {
    return error_response(err(ErrorCode::NotFound, "文件不存在"));
  }
  if (!std::filesystem::is_regular_file(full, ec)) {
    return error_response(err(ErrorCode::NotFound, "文件不存在", "", path));
  }
  return drogon::HttpResponse::newFileResponse(full_str, "", drogon::CT_NONE, "", req);
}

void log_routes(const ServerOptions& opts, bool web_ready) {
  LOG_INFO << cat("http listening on http://", opts.bind, ":", std::to_string(opts.port));
  LOG_INFO << "  GET  /                            -> /ui/ 文件树页面";
  LOG_INFO << "  GET  /api/storages                资源库列表";
  LOG_INFO << "  GET  /api/storages/{id}           单个资源库";
  LOG_INFO << "  POST /api/storages                新建 / 更新（body.create）";
  LOG_INFO << "  GET  /api/storages/{id}/files     列目录（?path=/a/b）";
  LOG_INFO << "  POST /api/storages/{id}/scans     入队扫描";
  LOG_INFO << "  GET  /api/health                  健康检查";
  if (!web_ready) {
    LOG_WARN << cat("web_root 目录不存在: ", opts.web_root);
  }
}

} // namespace

Status serve(const ServerOptions& opts, DbPool& db, JobWorker& jobs) {
  AppContext& ctx = AppContext::instance();
  ctx.db = &db;
  ctx.jobs = &jobs;
  ctx.web_root = opts.web_root;

  std::error_code ec;
  const bool web_ready =
      !opts.web_root.empty() && std::filesystem::is_directory(opts.web_root, ec);

  std::string root;
  if (web_ready) {
    root = std::filesystem::weakly_canonical(opts.web_root, ec).string();
    if (ec) {
      root = opts.web_root;
    }
  }

  auto& app = drogon::app();
  app.addListener(opts.bind, opts.port);

  // 静态页面：GET / 跳转，GET /ui(/...) 直接发文件
  app.registerHandler(
      "/",
      [web_ready](const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        if (web_ready) {
          callback(drogon::HttpResponse::newRedirectionResponse("/ui/"));
          return;
        }
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k503ServiceUnavailable);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody("web UI 未启用：请从项目根目录启动，或用 --web-root 指定 web 目录\n");
        callback(resp);
      },
      {drogon::Get});

  app.registerHandlerViaRegex(
      "^/ui(/.*)?$",
      [web_ready, root](const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        if (!web_ready) {
          callback(error_response(err(ErrorCode::NotFound, "web UI 未启用")));
          return;
        }
        const std::string path = req->path();
        if (path == "/ui") {
          callback(drogon::HttpResponse::newRedirectionResponse("/ui/"));
          return;
        }
        callback(serve_static(root, path, req));
      },
      {drogon::Get});

  // 未知接口统一给 JSON 404
  Json::Value body(Json::objectValue);
  Json::Value e(Json::objectValue);
  e["code"] = to_string(ErrorCode::NotFound);
  e["message"] = "未知接口";
  body["error"] = e;
  app.setCustom404Page(json_response(drogon::k404NotFound, body));

  app.setExceptionHandler([](const std::exception& e, const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    LOG_ERROR << cat("request failed: ", req->path(), " -> ", e.what());
    callback(error_response(err(ErrorCode::Internal, "请求处理异常", e.what())));
  });

  // 关闭：只做「丢弃队列 + 置取消标记 + 退出事件循环」，join 留给 main
  auto on_signal = []() {
    LOG_INFO << "signal received, cancelling scan queue";
    if (AppContext::instance().jobs != nullptr) {
      AppContext::instance().jobs->cancel();
    }
    drogon::app().quit();
  };
  app.setIntSignalHandler(on_signal);
  app.setTermSignalHandler(on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  log_routes(opts, web_ready);

  try {
    app.run();
  } catch (const std::exception& e) {
    return err(ErrorCode::Io, "HTTP 监听失败",
               cat(opts.bind, ":", std::to_string(opts.port), " ", e.what()));
  }

  LOG_INFO << "http server stopped";
  return {};
}

} // namespace sb::web