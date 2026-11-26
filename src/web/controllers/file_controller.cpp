#include "web/controllers/file_controller.hpp"

#include "pg_store.hpp"
#include "scan_job.hpp"
#include "storage.hpp"
#include "util.hpp"
#include "web/app_context.hpp"
#include "web/json_util.hpp"
#include "web/storage_target.hpp"

#include <json/json.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace sb::web {

void FileController::list(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          std::string storage_id) {
  storage_id = trim(storage_id);
  if (!is_uuid(storage_id)) {
    callback(error_response(err(ErrorCode::InvalidArgument, "storage_id 不是合法 UUID", "",
                                cat("GET /api/storages/", storage_id, "/files"))));
    return;
  }

  std::string req_path = trim(req->getParameter("path"));
  if (req_path.empty()) {
    req_path = "/";
  }
  auto rel_path = normalize_rel_path(req_path);
  if (!rel_path) {
    callback(error_response(rel_path.error()));
    return;
  }

  submit_reply(callback, [storage_id, req_path, rel = *rel_path](IDatabase& db) -> Result<Reply> {
    auto target = load_storage_target(db, storage_id);
    if (!target) {
      return target.error();
    }

    auto joined = build_storage_path(target->base_path, rel);
    if (!joined) {
      return joined.error();
    }
    const std::string url = cat(target->root_url, *joined);

    auto storage = Storage::create(storage_options_for(*target), AppContext::instance().webdav);
    if (!storage) {
      return storage.error();
    }

    std::vector<ObjectRef> refs;
    Status listed = (*storage)->list(url, [&](const ObjectRef& ref) -> Status {
      refs.push_back(ref);
      return {};
    });
    if (!listed) {
      return listed.error();
    }

    // 目录在前，其余按名字升序
    std::sort(refs.begin(), refs.end(), [](const ObjectRef& a, const ObjectRef& b) {
      if (a.is_dir != b.is_dir) {
        return a.is_dir;
      }
      return a.name < b.name;
    });

    Json::Value entries(Json::arrayValue);
    for (const auto& ref : refs) {
      Json::Value e(Json::objectValue);
      e["name"] = ref.name;
      e["path"] = cat(*joined, ref.name);
      e["rel_path"] = join_key(rel, ref.name);
      e["uri"] = ref.uri;
      e["size"] = static_cast<Json::Int64>(ref.size);
      e["mtime"] = static_cast<Json::Int64>(ref.mtime);
      e["is_dir"] = ref.is_dir;
      entries.append(e);
    }

    Json::Value body(Json::objectValue);
    body["storage_id"] = storage_id;
    body["name"] = target->name;
    body["base_path"] = target->base_path;
    body["path"] = req_path;
    body["rel_path"] = rel;
    body["uri"] = url;
    body["count"] = static_cast<Json::UInt64>(refs.size());
    body["entries"] = entries;
    return Reply{drogon::k200OK, body};
  });
}

} // namespace sb::web