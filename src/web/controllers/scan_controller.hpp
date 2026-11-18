#pragma once

#include <drogon/drogon.h>

#include <functional>

namespace sb::web {

// /api/storages/{id}/scans：把扫描任务加进 JobWorker 队列（立即返回 202）
class ScanController : public drogon::HttpController<ScanController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ScanController::create, "/api/storages/{1}/scans", drogon::Post);
  METHOD_LIST_END

  void create(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
              std::string storage_id);
};

} // namespace sb::web