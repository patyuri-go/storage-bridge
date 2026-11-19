#include "web/controllers/entity_controller.hpp"

#include "pg_store.hpp"
#include "result.hpp"
#include "storage.hpp"
#include "util.hpp"
#include "web/app_context.hpp"
#include "web/db_pool.hpp"
#include "web/entity_lookup.hpp"
#include "web/json_util.hpp"
#include "web/storage_target.hpp"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sb::web {
namespace {

// 探测分支读多少字节就够判定「这个位置可读」
constexpr std::size_t kProbeChunk = 64 * 1024;
// 所有分支都没成功时，客户端最多等多久
constexpr std::chrono::seconds kFirstByteDeadline{5};

struct ByteRange {
  bool present = false;       // 请求头里有 Range
  bool valid = false;         // 可满足的单段区间
  bool unsatisfiable = false; // 语法能懂但越界 -> 416
  std::int64_t start = 0;
  std::int64_t end = 0; // 含
};

bool parse_int64(const std::string& s, std::int64_t& out) {
  if (s.empty() || s.size() > 18) {
    return false;
  }
  std::int64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

// 只支持单段 Range；多段或语法不认识时忽略 Range，按整文件返回
ByteRange parse_range(const std::string& header, std::int64_t size) {
  ByteRange r;
  const std::string h = trim(header);
  if (h.empty()) {
    return r;
  }
  r.present = true;

  constexpr const char* kPrefix = "bytes=";
  if (h.rfind(kPrefix, 0) != 0) {
    return r;
  }
  const std::string spec = trim(h.substr(std::string(kPrefix).size()));
  if (spec.find(',') != std::string::npos) {
    return r;
  }
  const auto dash = spec.find('-');
  if (dash == std::string::npos) {
    return r;
  }
  const std::string first = trim(spec.substr(0, dash));
  const std::string last = trim(spec.substr(dash + 1));

  std::int64_t start = 0;
  std::int64_t end = 0;
  if (first.empty()) {
    // bytes=-N：最后 N 字节
    std::int64_t n = 0;
    if (!parse_int64(last, n)) {
      return r;
    }
    if (n == 0 || size == 0) {
      r.unsatisfiable = true;
      return r;
    }
    r.start = std::max<std::int64_t>(0, size - n);
    r.end = size - 1;
    r.valid = true;
    return r;
  }

  if (!parse_int64(first, start)) {
    return r;
  }
  if (last.empty()) {
    end = size - 1;
  } else if (!parse_int64(last, end)) {
    return r;
  }
  if (size == 0 || start >= size) {
    r.unsatisfiable = true;
    return r;
  }
  if (end < start) {
    return r;
  }
  end = std::min(end, size - 1);
  r.start = start;
  r.end = end;
  r.valid = true;
  return r;
}

std::string content_type_for(const std::string& mime) {
  // mime_type_for() 对未知扩展名存的是 "flac" 这种裸后缀，补成合法 Content-Type
  if (mime.find('/') != std::string::npos) {
    return mime;
  }
  return "application/octet-stream";
}

// 竞速共享状态：探测线程写，handler 线程等
struct RaceState {
  std::mutex mu;
  std::condition_variable cv;
  std::size_t pending = 0;
  bool winner_taken = false;
  bool abandoned = false; // 响应已经定了，晚到的成功不再需要交接 Reader
  std::unique_ptr<Reader> winner;
  std::string head;                  // winner 已经读到的首块
  std::vector<std::string> failures; // "storage 名: 错误"
};

// 探测一条位置：打开 + 读首块（最多 want 字节），把结论交给竞速状态并记账。
// 不取消：即使别人已经赢了，也跑完这一小段以判断该位置是否有效。
void probe_location(const std::shared_ptr<RaceState>& state, const std::shared_ptr<DbPool>& pool,
                    EntityLocation loc, std::int64_t offset, std::size_t want) {
  Error failure;
  std::unique_ptr<Reader> reader;
  std::string chunk;
  bool ok = false;

  auto target = make_storage_target(loc.storage_id, loc.storage_name, loc.storage_type,
                                    loc.storage_endpoint, loc.storage_config_json);
  if (!target) {
    failure = target.error();
  } else {
    auto storage = Storage::create(storage_options_for(*target));
    if (!storage) {
      failure = storage.error();
    } else {
      auto joined = build_storage_path(target->base_path, loc.key);
      if (!joined) {
        failure = joined.error();
      } else {
        const std::string url = cat(target->root_url, *joined);
        auto opened = Reader::open(**storage, url);
        if (!opened) {
          failure = opened.error();
        } else {
          std::string buf(want, '\0');
          auto got = (*opened)->read_at(offset, buf.data(), want);
          if (!got) {
            failure = got.error();
          } else if (*got == 0) {
            failure = err(ErrorCode::Storage, "读取到 0 字节", "", loc.key);
          } else {
            ok = true;
            chunk.assign(buf.data(), *got);
            reader = std::move(*opened);
          }
        }
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(state->mu);
    if (ok) {
      if (!state->winner_taken && !state->abandoned) {
        state->winner_taken = true;
        state->winner = std::move(reader);
        state->head = std::move(chunk);
      }
      // 迟到/没人要：reader 在这里析构，连接随之关闭
    } else {
      state->failures.push_back(cat(loc.storage_name, ": ", failure.message));
    }
    if (state->pending > 0) {
      --state->pending;
    }
  }
  state->cv.notify_all();

  // 记账：探测线程不在池内，可以 submit
  try {
    pool->submit([loc, ok](PgStore& db) {
      Status st = ok ? db.mark_location_ok(loc.entity_id, loc.storage_id, loc.key)
                     : db.mark_location_failed(loc.entity_id, loc.storage_id, loc.key);
      if (!st) {
        LOG_WARN << cat("更新 failed_at 失败: ", st.error().message);
      }
    });
  } catch (const std::exception& e) {
    LOG_WARN << cat("failed_at 记账被丢弃: ", e.what());
  }
}

// 响应体读取状态；回调由 drogon 的 IO 线程驱动
struct StreamState {
  std::unique_ptr<Reader> reader;
  std::string head; // 探测时已经读到的首块，先吐它
  std::size_t head_sent = 0;
  std::int64_t offset = 0;    // reader 当前应读的位置
  std::int64_t remaining = 0; // 响应体还要发多少字节（含 head）
};

// 回调返回写入字节数，0 表示结束；buf == nullptr 是 drogon 的清理通知
std::size_t fill_stream(char* buf, std::size_t len, const std::shared_ptr<StreamState>& state) {
  if (buf == nullptr) {
    state->reader.reset();
    return 0;
  }
  if (state->remaining <= 0) {
    return 0;
  }

  if (state->head_sent < state->head.size()) {
    const std::size_t n = std::min<std::size_t>(len, state->head.size() - state->head_sent);
    std::memcpy(buf, state->head.data() + state->head_sent, n);
    state->head_sent += n;
    state->remaining -= static_cast<std::int64_t>(n);
    return n;
  }

  if (!state->reader) {
    return 0;
  }
  const std::size_t want =
      std::min<std::size_t>(len, static_cast<std::size_t>(state->remaining));
  auto got = state->reader->read_at(state->offset, buf, want);
  if (!got) {
    LOG_WARN << cat("stream read failed: ", got.error().to_string());
    state->reader.reset();
    return 0;
  }
  if (*got == 0) {
    LOG_WARN << cat("stream ended early, remaining=", std::to_string(state->remaining));
    state->reader.reset();
    return 0;
  }
  state->offset += static_cast<std::int64_t>(*got);
  state->remaining -= static_cast<std::int64_t>(*got);
  return *got;
}

// 路径数组：必须是非空字符串数组；元素 trim 后规范化，空串拒绝。
// 规范化后去重，并把被父路径包含的项丢掉（/a 吸收 /a/b，但不吸收 /ab）。
Result<std::vector<std::string>> parse_lookup_paths(const Json::Value& body) {
  const char* kContext = "POST /api/storages/{id}/entities/lookup";
  if (!body.isObject() || !body.isMember("paths") || !body["paths"].isArray()) {
    return err(ErrorCode::InvalidArgument, "params.paths 必须是字符串数组", "", kContext);
  }
  const Json::Value& arr = body["paths"];
  if (arr.empty()) {
    return err(ErrorCode::InvalidArgument, "params.paths 不能为空数组", "", kContext);
  }

  std::vector<std::string> paths;
  for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
    if (!arr[i].isString()) {
      return err(ErrorCode::InvalidArgument, "params.paths 必须是字符串数组", "", kContext);
    }
    const std::string raw = trim(arr[i].asString());
    if (raw.empty()) {
      return err(ErrorCode::InvalidArgument,
                 cat("params.paths[", std::to_string(i), "] 不能是空字符串"), "", kContext);
    }
    auto rel = normalize_rel_path(raw);
    if (!rel) {
      return rel.error();
    }
    paths.push_back(*rel);
  }

  std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
      return a.size() < b.size();
    }
    return a < b;
  });

  std::vector<std::string> kept;
  for (const auto& path : paths) {
    bool covered = false;
    for (const auto& k : kept) {
      if (k == "/" || path == k ||
          (path.size() > k.size() && path.compare(0, k.size(), k) == 0 &&
           path[k.size()] == '/')) {
        covered = true;
        break;
      }
    }
    if (!covered) {
      kept.push_back(path);
    }
  }
  std::sort(kept.begin(), kept.end());
  return kept;
}

} // namespace

void EntityController::content(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                               std::string entity_id) {
  entity_id = trim(entity_id);
  if (!is_uuid(entity_id)) {
    callback(error_response(err(ErrorCode::InvalidArgument, "entity_id 不是合法 UUID", "",
                                cat("GET /api/entities/", entity_id, "/content"))));
    return;
  }

  HttpCallback respond = std::move(callback);
  const std::string range_header = req->getHeader("range");
  const std::shared_ptr<DbPool> pool = AppContext::instance().db;

  pool->submit([respond, entity_id, range_header, req, pool](PgStore& db) mutable {
    drogon::HttpResponsePtr resp;
    try {
      auto locations = db.list_entity_locations(entity_id);
      if (!locations) {
        resp = error_response(locations.error());
      } else if (locations->empty()) {
        resp = error_response(
            err(ErrorCode::NotFound, "实体不存在或没有可用位置", "", entity_id));
      } else {
        const std::string ctype = content_type_for(locations->front().mime_type);
        const std::int64_t size = locations->front().size;
        const ByteRange range = parse_range(range_header, size);

        if (range.unsatisfiable) {
          Json::Value body(Json::objectValue);
          body["error"] = error_json(err(ErrorCode::InvalidArgument, "Range 超出文件大小",
                                          cat("size=", std::to_string(size)),
                                          cat("GET /api/entities/", entity_id, "/content")));
          resp = json_response(drogon::k416RequestedRangeNotSatisfiable, body);
          resp->addHeader("Content-Range", cat("bytes */", std::to_string(size)));
        } else {
          const std::int64_t offset = range.valid ? range.start : 0;
          const std::int64_t length = range.valid ? (range.end - range.start + 1) : size;

          if (length <= 0) {
            // 空文件
            resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeString(ctype);
            resp->addHeader("Accept-Ranges", "bytes");
            resp->addHeader("Content-Length", "0");
          } else {
            const std::size_t want =
                static_cast<std::size_t>(std::min<std::int64_t>(kProbeChunk, length));
            auto state = std::make_shared<RaceState>();
            state->pending = locations->size();

            for (const auto& loc : *locations) {
              try {
                std::thread(probe_location, state, pool, loc, offset, want).detach();
              } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(state->mu);
                if (state->pending > 0) {
                  --state->pending;
                }
                state->failures.push_back(
                    cat(loc.storage_name, ": 探测线程创建失败: ", e.what()));
              }
            }
            state->cv.notify_all();

            std::unique_ptr<Reader> winner;
            std::string head;
            std::vector<std::string> failures;
            std::size_t unfinished = 0;
            bool all_failed = false;

            {
              std::unique_lock<std::mutex> lock(state->mu);
              state->cv.wait_for(lock, kFirstByteDeadline, [&] {
                return state->winner_taken || state->pending == 0;
              });
              state->abandoned = true;
              if (state->winner_taken) {
                winner = std::move(state->winner);
                head = std::move(state->head);
              } else {
                all_failed = (state->pending == 0);
                unfinished = state->pending;
                failures = state->failures;
              }
            }

            if (winner) {
              auto stream = std::make_shared<StreamState>();
              stream->reader = std::move(winner);
              stream->head = std::move(head);
              stream->offset = offset + static_cast<std::int64_t>(stream->head.size());
              stream->remaining = length;

              const std::size_t content_length = static_cast<std::size_t>(length);
              resp = drogon::HttpResponse::newStreamResponse(
                  [stream](char* buf, std::size_t n) { return fill_stream(buf, n, stream); }, "",
                  drogon::CT_CUSTOM, ctype, req);
              resp->setStatusCode(range.valid ? drogon::k206PartialContent : drogon::k200OK);
              resp->addHeader("Accept-Ranges", "bytes");
              // 显式给 content-length，drogon 就不会改用 chunked
              resp->addHeader("Content-Length", std::to_string(content_length));
              if (range.valid) {
                resp->addHeader("Content-Range",
                                cat("bytes ", std::to_string(range.start), "-",
                                    std::to_string(range.end), "/", std::to_string(size)));
              }
              LOG_DEBUG << cat("stream start: ", entity_id, " offset=", std::to_string(offset),
                               " length=", std::to_string(length));
            } else {
              std::string detail;
              for (const auto& f : failures) {
                if (!detail.empty()) {
                  detail += "; ";
                }
                detail += f;
              }
              if (!all_failed) {
                if (!detail.empty()) {
                  detail += "; ";
                }
                detail += cat("其余 ", std::to_string(unfinished), " 个位置未在 5 秒内返回");
              }
              if (detail.empty()) {
                detail = "没有可用的位置";
              }
              LOG_WARN << cat("all locations failed: ", entity_id, " ", detail);
              resp = error_response(err(ErrorCode::Network, "所有位置都读取失败", detail,
                                        cat("GET /api/entities/", entity_id, "/content")));
            }
          }
        }
      }
    } catch (const std::exception& e) {
      resp = error_response(err(ErrorCode::Internal, "请求处理异常", e.what()));
    } catch (...) {
      resp = error_response(err(ErrorCode::Internal, "请求处理异常"));
    }
    drogon::app().getLoop()->queueInLoop([respond, resp]() { respond(resp); });
  });
}

void EntityController::lookup(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              std::string storage_id) {
  storage_id = trim(storage_id);
  if (!is_uuid(storage_id)) {
    callback(error_response(err(ErrorCode::InvalidArgument, "storage_id 不是合法 UUID", "",
                                cat("POST /api/storages/", storage_id, "/entities/lookup"))));
    return;
  }

  auto parsed = parse_json_body(req);
  if (!parsed) {
    callback(error_response(parsed.error()));
    return;
  }
  auto paths = parse_lookup_paths(*parsed);
  if (!paths) {
    callback(error_response(paths.error()));
    return;
  }

  HttpCallback respond = std::move(callback);
  const std::shared_ptr<DbPool> pool = AppContext::instance().db;
  const std::string context = cat("POST /api/storages/", storage_id, "/entities/lookup");

  try {
    std::thread([respond, pool, storage_id, paths = *paths, context]() mutable {
      auto send = [respond](drogon::HttpResponsePtr resp) {
        drogon::app().getLoop()->queueInLoop([respond, resp]() { respond(resp); });
      };

      // 遍历是远端 IO，不占 DbPool 线程；只有查库的两小段走池
      Result<StorageTarget> target = err(ErrorCode::Internal, "加载存储配置失败");
      try {
        target = pool->submit([storage_id](PgStore& db) {
          return load_storage_target(db, storage_id);
        }).get();
      } catch (const std::exception& e) {
        send(error_response(err(ErrorCode::Internal, "请求处理异常", e.what(), context)));
        return;
      }
      if (!target) {
        send(error_response(target.error()));
        return;
      }

      const LookupWalk walk = walk_storage_paths(*target, paths);
      if (!walk.any_input_ok) {
        std::string detail;
        for (const auto& item : walk.errors) {
          if (!detail.empty()) {
            detail += "; ";
          }
          detail += cat(item.first, ": ", item.second);
        }
        if (detail.empty()) {
          detail = "没有可访问的路径";
        }
        send(error_response(err(ErrorCode::Storage, "所有路径都无法访问", detail, context)));
        return;
      }

      Result<std::map<std::string, std::string>> hits =
          err(ErrorCode::Internal, "查询实体位置失败");
      try {
        hits = pool->submit([storage_id, files = walk.files](PgStore& db) {
          return db.lookup_locations(storage_id, files);
        }).get();
      } catch (const std::exception& e) {
        send(error_response(err(ErrorCode::Internal, "请求处理异常", e.what(), context)));
        return;
      }
      if (!hits) {
        send(error_response(hits.error()));
        return;
      }

      Json::Value body(Json::objectValue);
      body["storage_id"] = storage_id;

      Json::Value files(Json::arrayValue);
      std::size_t matched = 0;
      for (const auto& rel : walk.files) {
        const auto it = hits->find(rel);
        if (it == hits->end()) {
          continue;
        }
        Json::Value item(Json::objectValue);
        item["rel_path"] = rel;
        item["entity_id"] = it->second;
        files.append(item);
        ++matched;
      }

      Json::Value errors(Json::arrayValue);
      for (const auto& item : walk.errors) {
        Json::Value e(Json::objectValue);
        e["rel_path"] = item.first;
        e["message"] = item.second;
        errors.append(e);
      }

      body["files"] = files;
      body["scanned"] = static_cast<Json::UInt64>(walk.files.size());
      body["missing"] = static_cast<Json::UInt64>(walk.files.size() - matched);
      body["truncated"] = walk.truncated;
      body["error_count"] = static_cast<Json::UInt64>(walk.error_count);
      body["errors"] = errors;

      LOG_INFO << cat("lookup ", storage_id, ": paths=", std::to_string(paths.size()),
                      " scanned=", std::to_string(walk.files.size()),
                      " missing=", std::to_string(walk.files.size() - matched),
                      " errors=", std::to_string(walk.error_count),
                      walk.truncated ? " truncated" : "");
      send(json_response(drogon::k200OK, body));
    }).detach();
  } catch (const std::exception& e) {
    respond(error_response(err(ErrorCode::Internal, "请求处理异常", e.what(), context)));
  }
}

} // namespace sb::web
