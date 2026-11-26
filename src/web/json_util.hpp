#pragma once

#include "pg_store.hpp"
#include "result.hpp"
#include "web/app_context.hpp"
#include "web/db_pool.hpp"

#include <drogon/drogon.h>
#include <json/json.h>

#include <functional>
#include <string>
#include <utility>

namespace sb::web {

using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;

// 一个 handler 的成功结果：HTTP 状态码 + 响应体
struct Reply {
  drogon::HttpStatusCode status = drogon::k200OK;
  Json::Value body;
};

// ErrorCode -> HTTP 状态码
drogon::HttpStatusCode status_for(ErrorCode code);

std::string dump_json(const Json::Value& v);
drogon::HttpResponsePtr json_response(drogon::HttpStatusCode status, const Json::Value& body);
Json::Value error_json(const Error& e);
drogon::HttpResponsePtr error_response(const Error& e);

// 解析请求体为 JSON 对象；失败返回 invalid_argument
Result<Json::Value> parse_json_body(const drogon::HttpRequestPtr& req);

// 把阻塞工作（SOCI 查询、WebDAV 访问）丢到 DbPool 线程上跑，
// 结果回到 IO 线程后再回复客户端。
//   fn: Result<Reply>(IDatabase&)
template <class F> void submit_reply(const HttpCallback& callback, F&& fn) {
  AppContext& ctx = AppContext::instance();
  ctx.db->submit([callback, fn = std::forward<F>(fn)](IDatabase& db) mutable {
    drogon::HttpResponsePtr resp;
    try {
      auto out = fn(db);
      if (out) {
        resp = json_response(out->status, out->body);
      } else {
        resp = error_response(out.error());
      }
    } catch (const std::exception& e) {
      resp = error_response(err(ErrorCode::Internal, "请求处理异常", e.what()));
    } catch (...) {
      resp = error_response(err(ErrorCode::Internal, "请求处理异常"));
    }
    drogon::app().getLoop()->queueInLoop([callback, resp]() { callback(resp); });
  });
}

} // namespace sb::web