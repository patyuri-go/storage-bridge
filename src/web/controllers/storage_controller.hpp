#pragma once

#include <drogon/drogon.h>

#include <functional>

namespace sb::web {

// /api/storages 及其子资源
class StorageController : public drogon::HttpController<StorageController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(StorageController::list, "/api/storages", drogon::Get);
  ADD_METHOD_TO(StorageController::get_one, "/api/storages/{1}", drogon::Get);
  ADD_METHOD_TO(StorageController::upsert, "/api/storages", drogon::Post);
  METHOD_LIST_END

  void list(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback);
  void get_one(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               std::string storage_id);
  // create=true：新建（storage_id 由数据库生成）
  // create=false：按 storage_id 更新（不存在返回 404）
  void upsert(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace sb::web