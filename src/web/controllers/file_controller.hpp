#pragma once

#include <drogon/drogon.h>

#include <functional>

namespace sb::web {

// /api/storages/{id}/files?path=...：列某个 storage 下某个目录的直接子项
class FileController : public drogon::HttpController<FileController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(FileController::list, "/api/storages/{1}/files", drogon::Get);
  METHOD_LIST_END

  void list(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            std::string storage_id);
};

} // namespace sb::web