#include "web/controllers/health_controller.hpp"

#include "pg_store.hpp"
#include "web/json_util.hpp"

#include <json/json.h>

#include <utility>

namespace sb::web {

void HealthController::check(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  submit_reply(callback, [](PgStore& db) -> Result<Reply> {
    Json::Value body(Json::objectValue);
    if (Status st = db.ping(); !st) {
      body["status"] = "degraded";
      body["db"] = "down";
      body["error"] = st.error().message;
      return Reply{drogon::k503ServiceUnavailable, body};
    }
    body["status"] = "ok";
    body["db"] = "up";
    return Reply{drogon::k200OK, body};
  });
}

} // namespace sb::web