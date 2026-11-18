#pragma once

#include <drogon/drogon.h>

#include <functional>
#include <string>

namespace sb::web {

// /api/entities/{id}/content：按实体 id 取出全部位置并发探测，谁先读到数据用谁；
// 其余位置继续读完首块，用于更新 failed_at（成功清空、失败写 now()）。
// 支持单段 Range（bytes=a-b / a- / -n），用于播放器拖动进度。
class EntityController : public drogon::HttpController<EntityController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(EntityController::content, "/api/entities/{1}/content", drogon::Get);
  METHOD_LIST_END

  void content(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               std::string entity_id);
};

} // namespace sb::web
