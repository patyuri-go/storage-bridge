#pragma once

#include <drogon/drogon.h>

#include <functional>
#include <string>

namespace sb::web {

// /api/entities/{id}/content：按实体 id 取出全部位置并发探测，谁先读到数据用谁；
// 其余位置继续读完首块，用于更新 failed_at（成功清空、失败写 now()）。
// 支持单段 Range（bytes=a-b / a- / -n），用于播放器拖动进度。
//
// /api/storages/{id}/entities/lookup：在指定存储上递归遍历若干 rel_path，
// 返回文件 rel_path -> entity_id 的映射，以及「存储上有、库里没有」的文件数。
class EntityController : public drogon::HttpController<EntityController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(EntityController::content, "/api/entities/{1}/content", drogon::Get);
  ADD_METHOD_TO(EntityController::lookup, "/api/storages/{1}/entities/lookup", drogon::Post);
  METHOD_LIST_END

  void content(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               std::string entity_id);

  void lookup(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
              std::string storage_id);
};

} // namespace sb::web
