#include "web/controllers/scan_controller.hpp"

#include "job_worker.hpp"
#include "pg_store.hpp"
#include "scan_job.hpp"
#include "storage.hpp"
#include "util.hpp"
#include "web/app_context.hpp"
#include "web/json_util.hpp"
#include "web/storage_target.hpp"

#include <json/json.h>

#include <trantor/utils/Logger.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sb::web {

namespace {

// body 里的可选路径数组：省略 -> 空；必须是字符串数组，元素按 rel path 规范化
Result<std::vector<std::string>> optional_path_array(const Json::Value& body, const char* key) {
  std::vector<std::string> out;
  if (!body.isMember(key) || body[key].isNull()) {
    return out;
  }
  const Json::Value& arr = body[key];
  if (!arr.isArray()) {
    return err(ErrorCode::InvalidArgument, cat("params.", key, " 必须是字符串数组"));
  }
  for (const auto& item : arr) {
    if (!item.isString()) {
      return err(ErrorCode::InvalidArgument, cat("params.", key, " 必须是字符串数组"));
    }
    auto rel = normalize_rel_path(item.asString());
    if (!rel) {
      return rel.error();
    }
    out.push_back(*rel);
  }
  return out;
}

} // namespace

void ScanController::create(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                            std::string storage_id) {
  storage_id = trim(storage_id);
  if (!is_uuid(storage_id)) {
    callback(error_response(err(ErrorCode::InvalidArgument, "storage_id 不是合法 UUID", "",
                                cat("POST /api/storages/", storage_id, "/scans"))));
    return;
  }

  auto parsed = parse_json_body(req);
  if (!parsed) {
    callback(error_response(parsed.error()));
    return;
  }
  const Json::Value& params = *parsed;

  auto files = optional_path_array(params, "files");
  if (!files) {
    callback(error_response(files.error()));
    return;
  }
  auto folders = optional_path_array(params, "folders");
  if (!folders) {
    callback(error_response(folders.error()));
    return;
  }
  if (files->empty() && folders->empty()) {
    callback(error_response(
        err(ErrorCode::InvalidArgument, "params.files 和 params.folders 至少要给一个非空数组")));
    return;
  }

  // mode: full（默认，读内容算哈希）/ verify（先查大小，命中就跳过）
  ScanMode mode = ScanMode::Full;
  if (params.isMember("mode") && !params["mode"].isNull()) {
    if (!params["mode"].isString()) {
      callback(error_response(
          err(ErrorCode::InvalidArgument, "params.mode 必须是字符串（full / verify）")));
      return;
    }
    const std::string raw = trim(params["mode"].asString());
    const std::string lowered = to_lower(raw);
    if (lowered == "full") {
      mode = ScanMode::Full;
    } else if (lowered == "verify") {
      mode = ScanMode::Verify;
    } else {
      callback(error_response(
          err(ErrorCode::InvalidArgument, "params.mode 只能是 full 或 verify", raw)));
      return;
    }
  }

  JobWorker* jobs = AppContext::instance().jobs;

  submit_reply(callback,
               [storage_id, files = std::move(*files), folders = std::move(*folders), mode,
                jobs](IDatabase& db) -> Result<Reply> {
                 auto target = load_storage_target(db, storage_id);
                 if (!target) {
                   return target.error();
                 }

                 auto storage = Storage::create(storage_options_for(*target), AppContext::instance().webdav);
                 if (!storage) {
                   return storage.error();
                 }

                 auto ctx = std::make_shared<ScanContext>();
                 ctx->storage = std::shared_ptr<Storage>(std::move(*storage));
                 ctx->storage_id = storage_id;
                 ctx->db = AppContext::instance().db.get();
                 ctx->mode = mode;

                 // 先把所有目标都校验/建好，避免提交到一半才发现某个路径非法
                 std::vector<std::unique_ptr<Job>> built;
                 for (const std::string& rel : folders) {
                   auto joined = build_storage_path(target->base_path, rel);
                   if (!joined) {
                     return joined.error();
                   }
                   auto job = make_scan_folder_job(ctx, cat(target->root_url, *joined), rel);
                   if (!job) {
                     return job.error();
                   }
                   built.push_back(std::move(*job));
                 }
                 for (const std::string& rel : files) {
                   auto joined = build_storage_path(target->base_path, rel);
                   if (!joined) {
                     return joined.error();
                   }
                   std::string url = cat(target->root_url, *joined);
                   while (!url.empty() && url.back() == '/') {
                     url.pop_back();
                   }
                   auto job = make_scan_file_job(ctx, url, rel);
                   if (!job) {
                     return job.error();
                   }
                   built.push_back(std::move(*job));
                 }

                 const std::size_t total = built.size();
                 std::size_t queued = 0;
                 for (auto& job : built) {
                   Status st = jobs->submit(std::move(job));
                   if (!st) {
                     if (st.error().code == ErrorCode::Busy) {
                       return err(ErrorCode::Busy, "任务队列已满，未能全部加入",
                                  cat("已加入 ", std::to_string(queued), "/",
                                      std::to_string(total)));
                     }
                     return st.error();
                   }
                   ++queued;
                 }

                 LOG_INFO << cat("scan accepted: storage=", target->name, " (", storage_id,
                                 ") queued=", std::to_string(queued), " mode=", to_string(mode));

                 Json::Value body(Json::objectValue);
                 body["accepted"] = true;
                 body["storage_id"] = storage_id;
                 body["queued"] = static_cast<Json::UInt64>(queued);
                 body["mode"] = to_string(mode);
                 return Reply{drogon::k202Accepted, body};
               });
}

} // namespace sb::web