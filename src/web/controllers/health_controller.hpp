#pragma once

#include <drogon/drogon.h>

#include <functional>

namespace sb::web {

// GET /api/health：数据库可用 200，不可用 503
class HealthController : public drogon::HttpController<HealthController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(HealthController::check, "/api/health", drogon::Get);
  METHOD_LIST_END

  void check(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace sb::web