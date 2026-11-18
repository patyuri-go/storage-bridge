#include "web/json_util.hpp"

#include "util.hpp"

#include <trantor/utils/Logger.h>

#include <memory>

namespace sb::web {

drogon::HttpStatusCode status_for(ErrorCode code) {
  switch (code) {
  case ErrorCode::InvalidArgument:
    return drogon::k400BadRequest;
  case ErrorCode::Auth:
    return drogon::k401Unauthorized;
  case ErrorCode::NotFound:
    return drogon::k404NotFound;
  case ErrorCode::Conflict:
    return drogon::k409Conflict;
  case ErrorCode::Busy:
    return drogon::k429TooManyRequests;
  case ErrorCode::Config:
  case ErrorCode::Network:
  case ErrorCode::Storage:
  case ErrorCode::Postgres:
  case ErrorCode::Io:
  case ErrorCode::Internal:
    return drogon::k500InternalServerError;
  }
  return drogon::k500InternalServerError;
}

std::string dump_json(const Json::Value& v) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  writer["emitUTF8"] = true;
  return Json::writeString(writer, v);
}

drogon::HttpResponsePtr json_response(drogon::HttpStatusCode status, const Json::Value& body) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(dump_json(body));
  return resp;
}

Json::Value error_json(const Error& e) {
  if (e.code == ErrorCode::Internal || e.code == ErrorCode::Postgres) {
    LOG_ERROR << cat("api error: ", e.to_string());
  }
  Json::Value j(Json::objectValue);
  j["code"] = to_string(e.code);
  j["message"] = e.message;
  if (!e.detail.empty()) {
    j["detail"] = e.detail;
  }
  if (!e.context.empty()) {
    j["context"] = e.context;
  }
  return j;
}

drogon::HttpResponsePtr error_response(const Error& e) {
  Json::Value body(Json::objectValue);
  body["error"] = error_json(e);
  return json_response(status_for(e.code), body);
}

Result<Json::Value> parse_json_body(const drogon::HttpRequestPtr& req) {
  const std::string_view raw = req->body();
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errs;
  const char* begin = raw.data();
  const char* end = begin + raw.size();
  if (!reader->parse(begin, end, &root, &errs)) {
    if (errs.empty()) {
      errs = "无法解析";
    }
    return err(ErrorCode::InvalidArgument, "请求体不是合法 JSON", trim(errs));
  }
  if (!root.isObject()) {
    return err(ErrorCode::InvalidArgument, "请求体必须是 JSON 对象");
  }
  return root;
}

} // namespace sb::web