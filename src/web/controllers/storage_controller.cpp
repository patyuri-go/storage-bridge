#include "web/controllers/storage_controller.hpp"

#include "pg_store.hpp"
#include "util.hpp"
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

// ---- storage_nodes 输出（一律不回传密码） ----

Json::Value storage_record_json(const StorageNodeRecord& rec) {
  Json::Value cfg_out(Json::objectValue);
  cfg_out["username"] = "";
  cfg_out["base_path"] = "";
  cfg_out["has_password"] = false;

  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value cfg;
  std::string errs;
  if (reader->parse(rec.config_json.data(), rec.config_json.data() + rec.config_json.size(), &cfg,
                    &errs)) {
    if (cfg.isObject()) {
      if (cfg.isMember("username") && cfg["username"].isString()) {
        cfg_out["username"] = cfg["username"].asString();
      }
      if (cfg.isMember("base_path") && cfg["base_path"].isString()) {
        cfg_out["base_path"] = cfg["base_path"].asString();
      }
      if (cfg.isMember("password") && cfg["password"].isString()) {
        cfg_out["has_password"] = !cfg["password"].asString().empty();
      }
    }
  } else {
    LOG_WARN << cat("storage ", rec.storage_id, " config 解析失败: ", trim(errs));
  }

  Json::Value out(Json::objectValue);
  out["storage_id"] = rec.storage_id;
  out["name"] = rec.name;
  out["type"] = rec.type;
  out["endpoint"] = rec.endpoint;
  out["state"] = rec.state;
  out["config"] = cfg_out;
  return out;
}

Json::Value compact(const Json::Value& v) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  writer["emitUTF8"] = true;
  return Json::Value(Json::writeString(writer, v));
}

} // namespace

void StorageController::list(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  submit_reply(callback, [](IDatabase& db) -> Result<Reply> {
    auto rows = db.list_storages();
    if (!rows) {
      return rows.error();
    }
    Json::Value arr(Json::arrayValue);
    for (const auto& rec : *rows) {
      arr.append(storage_record_json(rec));
    }
    return Reply{drogon::k200OK, arr};
  });
}

void StorageController::get_one(const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                std::string storage_id) {
  storage_id = trim(storage_id);
  if (!is_uuid(storage_id)) {
    callback(error_response(err(ErrorCode::InvalidArgument, "storage_id 不是合法 UUID", "",
                                cat("GET /api/storages/", storage_id))));
    return;
  }
  submit_reply(callback, [storage_id](IDatabase& db) -> Result<Reply> {
    auto node = db.get_storage(storage_id);
    if (!node) {
      return node.error();
    }
    return Reply{drogon::k200OK, storage_record_json(*node)};
  });
}

void StorageController::upsert(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  auto parsed = parse_json_body(req);
  if (!parsed) {
    callback(error_response(parsed.error()));
    return;
  }
  const Json::Value& params = *parsed;

  if (!params.isMember("create") || !params["create"].isBool()) {
    callback(error_response(err(ErrorCode::InvalidArgument, "params.create 必填（布尔）")));
    return;
  }
  const bool create = params["create"].asBool();

  StorageNodeInput in;
  if (!create) {
    if (!params.isMember("storage_id") || !params["storage_id"].isString()) {
      callback(error_response(err(ErrorCode::InvalidArgument,
                                  "create=false 时 params.storage_id 必填（UUID 字符串）")));
      return;
    }
    in.storage_id = trim(params["storage_id"].asString());
    if (!is_uuid(in.storage_id)) {
      callback(error_response(err(ErrorCode::InvalidArgument, "params.storage_id 不是合法 UUID")));
      return;
    }
  }

  if (!params.isMember("name") || !params["name"].isString()) {
    callback(error_response(err(ErrorCode::InvalidArgument, "params.name 必填（string）")));
    return;
  }
  in.name = trim(params["name"].asString());
  if (in.name.empty()) {
    callback(error_response(err(ErrorCode::InvalidArgument, "params.name 不能为空")));
    return;
  }

  if (params.isMember("type") && !params["type"].isNull()) {
    if (!params["type"].isString()) {
      callback(error_response(err(ErrorCode::InvalidArgument, "params.type 必须是字符串")));
      return;
    }
    in.type = params["type"].asString();
  }
  if (in.type != "webdav") {
    callback(error_response(err(ErrorCode::InvalidArgument, "目前只支持 type=\"webdav\"")));
    return;
  }

  if (!params.isMember("endpoint") || !params["endpoint"].isString()) {
    callback(error_response(err(ErrorCode::InvalidArgument,
                                "params.endpoint 必填（ip:port 或域名）")));
    return;
  }
  auto endpoint = normalize_endpoint(params["endpoint"].asString());
  if (!endpoint) {
    callback(error_response(endpoint.error()));
    return;
  }
  in.endpoint = std::move(*endpoint);

  if (params.isMember("state") && !params["state"].isNull()) {
    if (!params["state"].isString()) {
      callback(error_response(err(ErrorCode::InvalidArgument, "params.state 必须是字符串")));
      return;
    }
    in.state = params["state"].asString();
  }
  if (in.state != "active" && in.state != "disabled") {
    callback(error_response(
        err(ErrorCode::InvalidArgument, "params.state 只能是 active 或 disabled")));
    return;
  }

  // config：WebDAV 目前只认 username / password / base_path
  if (!params.isMember("config") || !params["config"].isObject()) {
    callback(error_response(err(ErrorCode::InvalidArgument,
                                "params.config 必填（对象：username/password/base_path）")));
    return;
  }
  const Json::Value& cfg = params["config"];
  for (const char* key : {"username", "password", "base_path"}) {
    if (!cfg.isMember(key) || !cfg[key].isString()) {
      callback(error_response(
          err(ErrorCode::InvalidArgument, cat("params.config.", key, " 必填（string）"))));
      return;
    }
  }
  const std::string username = trim(cfg["username"].asString());
  const std::string password = cfg["password"].asString();
  if (username.empty()) {
    callback(error_response(err(ErrorCode::InvalidArgument, "params.config.username 不能为空")));
    return;
  }
  auto base_path = normalize_base_path(cfg["base_path"].asString());
  if (!base_path) {
    callback(error_response(base_path.error()));
    return;
  }

  Json::Value cfg_out(Json::objectValue);
  cfg_out["username"] = username;
  cfg_out["password"] = password;
  cfg_out["base_path"] = *base_path;
  in.config_json = compact(cfg_out).asString();

  submit_reply(callback, [in, create](IDatabase& db) -> Result<Reply> {
    if (!create) {
      // 更新前先确认存在：不存在给 404，而不是按旧 upsert 语义悄悄新建
      auto existing = db.get_storage(in.storage_id);
      if (!existing) {
        return existing.error();
      }
    }
    auto out = db.upsert_storage(in);
    if (!out) {
      return map_conflict(out.error());
    }
    StorageNodeRecord rec;
    rec.storage_id = out->storage_id;
    rec.name = in.name;
    rec.type = in.type;
    rec.endpoint = in.endpoint;
    rec.state = in.state;
    rec.config_json = in.config_json;
    Reply reply;
    reply.status = create ? drogon::k201Created : drogon::k200OK;
    reply.body = storage_record_json(rec);
    return reply;
  });
}

} // namespace sb::web