#include "web/server.hpp"

#include "job_worker.hpp"
#include "pg_store.hpp"
#include "scan_job.hpp"
#include "storage.hpp"
#include "util.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sb::web {

namespace {

std::atomic<bool> g_stop{false};
httplib::Server* g_server = nullptr;

void on_signal(int) {
  g_stop = true;
  if (g_server != nullptr) {
    g_server->stop();
  }
}

bool is_uuid(const std::string& s) {
  if (s.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

// 去掉 scheme 前缀和尾部斜杠：存储表里的 endpoint 只保留 host[:port]
std::string strip_endpoint(std::string endpoint) {
  const auto scheme = endpoint.find("://");
  if (scheme != std::string::npos) {
    endpoint = endpoint.substr(scheme + 3);
  }
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  return endpoint;
}

// endpoint 只接受 "ip:port" 或 "域名"；路径一律放 config.base_path
Result<std::string> normalize_endpoint(std::string endpoint) {
  std::string v = strip_endpoint(trim(std::move(endpoint)));
  if (v.empty()) {
    return err(ErrorCode::InvalidArgument, "endpoint 不能为空（应为 ip:port 或域名）");
  }
  if (v.find('/') != std::string::npos) {
    return err(ErrorCode::InvalidArgument,
               "endpoint 只放 ip:port 或域名，路径请放在 config.base_path");
  }
  if (v.find(' ') != std::string::npos) {
    return err(ErrorCode::InvalidArgument, "endpoint 不能包含空格");
  }
  return v;
}

// WebDAV 的起始目录：确保以 / 开头、以 / 结尾、折叠重复斜杠
Result<std::string> normalize_base_path(std::string path) {
  std::string v = trim(std::move(path));
  if (v.empty()) {
    return err(ErrorCode::InvalidArgument, "config.base_path 不能为空（例如 /dav/tian_yi/My/BT/）");
  }
  if (v.front() != '/') {
    v.insert(v.begin(), '/');
  }
  std::string collapsed;
  collapsed.reserve(v.size() + 1);
  for (char c : v) {
    if (c == '/' && !collapsed.empty() && collapsed.back() == '/') {
      continue;
    }
    collapsed.push_back(c);
  }
  if (collapsed.back() != '/') {
    collapsed.push_back('/');
  }
  return collapsed;
}

// 规范化请求里的相对路径："" / "/" -> "/"，"/aaa/" -> "/aaa"；拒绝 ..
Result<std::string> normalize_rel_path(std::string rel) {
  std::string v = trim(std::move(rel));
  if (v.empty() || v == "/") {
    return std::string("/");
  }
  if (v.front() != '/') {
    v.insert(v.begin(), '/');
  }
  std::string out;
  std::size_t i = 1;
  while (i < v.size()) {
    const std::size_t j = v.find('/', i);
    const std::size_t end = (j == std::string::npos) ? v.size() : j;
    const std::string seg = v.substr(i, end - i);
    i = end + 1;
    if (seg.empty() || seg == ".") {
      continue;
    }
    if (seg == "..") {
      return err(ErrorCode::InvalidArgument, "path 不能包含 ..");
    }
    out += '/';
    out += seg;
  }
  return out.empty() ? std::string("/") : out;
}

// 把已规范化的相对路径拼到 base_path 后面。
// 前置条件：base_path 以 / 结尾，rel 以 / 开头（normalize_rel_path 的返回值）。
Result<std::string> build_storage_path(const std::string& base_path, const std::string& rel) {
  if (rel.empty() || rel == "/") {
    return base_path;
  }
  return cat(base_path, rel.substr(1), "/");
}

// 唯一约束冲突（name/storage_id）映射成 409
Error map_conflict(Error e) {
  if (e.detail == "23505") {
    e.code = ErrorCode::Conflict;
    e.message = "name 已存在（唯一约束冲突）";
  }
  return e;
}

// ---- 统一信封：{"op":..,"params":..} -> {"data":..} / {"data":null,"errors":[..]} ----

nlohmann::json error_json(const Error& e) {
  if (e.code == ErrorCode::Internal || e.code == ErrorCode::Postgres) {
    log(LogLevel::Error, cat("api error: ", e.to_string()));
  }
  nlohmann::json j = {
      {"code", to_string(e.code)},
      {"message", e.message},
  };
  if (!e.detail.empty()) {
    j["detail"] = e.detail;
  }
  if (!e.context.empty()) {
    j["context"] = e.context;
  }
  return j;
}

void send_json(httplib::Response& res, int status, const nlohmann::json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_data(httplib::Response& res, const nlohmann::json& data) {
  send_json(res, 200, {{"data", data}});
}

// 业务错误默认 HTTP 200（错误明细在 errors[]）；只有信封本身不合法才给 4xx
void send_errors(httplib::Response& res, int status, const std::vector<Error>& errors) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& e : errors) {
    arr.push_back(error_json(e));
  }
  send_json(res, status, {{"data", nullptr}, {"errors", arr}});
}

void send_one_error(httplib::Response& res, int status, const Error& e) {
  send_errors(res, status, {e});
}

// ---- params 取值工具 ----

Result<std::string> require_string(const nlohmann::json& params, const char* key,
                                   const char* hint) {
  const std::string path = cat("params.", key);
  if (!params.contains(key) || !params[key].is_string()) {
    return err(ErrorCode::InvalidArgument, cat(path, " 必填（", hint, "）"));
  }
  return params[key].get<std::string>();
}

// 三个 op 共用的 storage_id 校验：必填字符串 + trim + UUID 格式
Result<std::string> require_storage_id(const nlohmann::json& params) {
  auto raw = require_string(params, "storage_id", "storage 的 UUID");
  if (!raw) {
    return raw.error();
  }
  const std::string storage_id = trim(*raw);
  if (!is_uuid(storage_id)) {
    return err(ErrorCode::InvalidArgument, "params.storage_id 不是合法 UUID");
  }
  return storage_id;
}

// ---- storage_nodes 输出（一律不回传密码） ----

nlohmann::json storage_record_json(const StorageNodeRecord& rec) {
  nlohmann::json cfg_out = {
      {"username", ""},
      {"base_path", ""},
      {"has_password", false},
  };
  try {
    const nlohmann::json cfg = nlohmann::json::parse(rec.config_json);
    if (cfg.is_object()) {
      if (cfg.contains("username") && cfg["username"].is_string()) {
        cfg_out["username"] = cfg["username"].get<std::string>();
      }
      if (cfg.contains("base_path") && cfg["base_path"].is_string()) {
        cfg_out["base_path"] = cfg["base_path"].get<std::string>();
      }
      if (cfg.contains("password") && cfg["password"].is_string()) {
        cfg_out["has_password"] = !cfg["password"].get<std::string>().empty();
      }
    }
  } catch (const nlohmann::json::exception& e) {
    log(LogLevel::Warn, cat("storage ", rec.storage_id, " config 解析失败: ", e.what()));
  }

  return {
      {"storage_id", rec.storage_id}, {"name", rec.name},   {"type", rec.type},
      {"endpoint", rec.endpoint},     {"state", rec.state}, {"config", cfg_out},
  };
}

nlohmann::json storage_node_json(const StorageNodeInput& in, const std::string& base_path,
                                 const StorageNodeResult& out) {
  return {
      {"storage_id", out.storage_id},
      {"created", out.created},
      {"name", in.name},
      {"type", in.type},
      {"endpoint", in.endpoint},
      {"base_path", base_path},
      {"state", in.state},
  };
}

// ---- op 实现 ----

// storage.list：列出全部 storage_nodes，按 name 升序，config 里不回传密码
Result<nlohmann::json> op_storage_list(PgStore& store, JobWorker&, const nlohmann::json&) {
  auto rows = store.list_storages();
  if (!rows) {
    return rows.error();
  }

  nlohmann::json arr = nlohmann::json::array();
  for (const auto& rec : *rows) {
    arr.push_back(storage_record_json(rec));
  }
  return nlohmann::json{{"count", arr.size()}, {"storages", arr}};
}

// storage.get：按 storage_id 取单个节点
Result<nlohmann::json> op_storage_get(PgStore& store, JobWorker&, const nlohmann::json& params) {
  auto storage_id = require_storage_id(params);
  if (!storage_id) {
    return storage_id.error();
  }

  auto node = store.get_storage(*storage_id);
  if (!node) {
    return node.error();
  }
  return storage_record_json(*node);
}

// storage.upsert：带 storage_id 为更新（不存在则按该 ID 创建），不带为新建
Result<nlohmann::json> op_storage_upsert(PgStore& store, JobWorker&, const nlohmann::json& params) {
  StorageNodeInput in;

  if (params.contains("storage_id") && !params["storage_id"].is_null()) {
    if (!params["storage_id"].is_string()) {
      return err(ErrorCode::InvalidArgument, "params.storage_id 必须是 UUID 字符串");
    }
    in.storage_id = trim(params["storage_id"].get<std::string>());
    if (!in.storage_id.empty() && !is_uuid(in.storage_id)) {
      return err(ErrorCode::InvalidArgument, "params.storage_id 不是合法 UUID");
    }
  }

  auto raw_name = require_string(params, "name", "string，不能为空");
  if (!raw_name) {
    return raw_name.error();
  }
  in.name = trim(*raw_name);
  if (in.name.empty()) {
    return err(ErrorCode::InvalidArgument, "params.name 不能为空");
  }

  if (params.contains("type") && !params["type"].is_null()) {
    if (!params["type"].is_string()) {
      return err(ErrorCode::InvalidArgument, "params.type 必须是字符串");
    }
    in.type = params["type"].get<std::string>();
  }
  if (in.type != "webdav") {
    return err(ErrorCode::InvalidArgument, "目前只支持 type=\"webdav\"");
  }

  auto raw_endpoint = require_string(params, "endpoint", "ip:port 或域名");
  if (!raw_endpoint) {
    return raw_endpoint.error();
  }
  auto endpoint = normalize_endpoint(*raw_endpoint);
  if (!endpoint) {
    return endpoint.error();
  }
  in.endpoint = std::move(*endpoint);

  if (params.contains("state") && !params["state"].is_null()) {
    if (!params["state"].is_string()) {
      return err(ErrorCode::InvalidArgument, "params.state 必须是字符串");
    }
    in.state = params["state"].get<std::string>();
  }
  if (in.state != "active" && in.state != "disabled") {
    return err(ErrorCode::InvalidArgument, "params.state 只能是 active 或 disabled");
  }

  // config：WebDAV 目前只认 username / password / base_path
  if (!params.contains("config") || !params["config"].is_object()) {
    return err(ErrorCode::InvalidArgument,
               "params.config 必填（对象：username/password/base_path）");
  }
  const auto& cfg = params["config"];
  for (const char* key : {"username", "password", "base_path"}) {
    if (!cfg.contains(key) || !cfg[key].is_string()) {
      return err(ErrorCode::InvalidArgument, cat("params.config.", key, " 必填（string）"));
    }
  }
  const std::string username = trim(cfg["username"].get<std::string>());
  const std::string password = cfg["password"].get<std::string>();
  if (username.empty()) {
    return err(ErrorCode::InvalidArgument, "params.config.username 不能为空");
  }
  auto base_path = normalize_base_path(cfg["base_path"].get<std::string>());
  if (!base_path) {
    return base_path.error();
  }

  const nlohmann::json cfg_out = {
      {"username", username},
      {"password", password},
      {"base_path", *base_path},
  };
  in.config_json = cfg_out.dump();

  auto out = store.upsert_storage(in);
  if (!out) {
    return map_conflict(out.error());
  }
  return storage_node_json(in, *base_path, *out);
}

// file.list：列某个 storage 下某个目录的直接子项
// storage_nodes 的一行 → 能直接拼 URL 的目标（含凭据）。目前只支持 webdav
struct StorageTarget {
  std::string name;
  std::string root_url;  // 例如 http://10.0.0.1:5244
  std::string base_path; // 规范化后的 base_path，首尾都带 /
  std::string username;
  std::string password;
};

Result<StorageTarget> load_storage_target(PgStore& store, const std::string& storage_id) {
  auto node = store.get_storage(storage_id);
  if (!node) {
    return node.error();
  }
  if (node->type != "webdav") {
    return err(ErrorCode::InvalidArgument, "目前只支持 type=\"webdav\"", "", storage_id);
  }

  nlohmann::json cfg;
  try {
    cfg = nlohmann::json::parse(node->config_json);
  } catch (const nlohmann::json::exception& e) {
    return err(ErrorCode::Internal, "storage.config 不是合法 JSON", e.what(), storage_id);
  }
  for (const char* key : {"username", "password", "base_path"}) {
    if (!cfg.contains(key) || !cfg[key].is_string()) {
      return err(ErrorCode::Internal, cat("storage.config 缺少 ", key), "", storage_id);
    }
  }

  auto base_path = normalize_base_path(cfg["base_path"].get<std::string>());
  if (!base_path) {
    return base_path.error();
  }

  const std::string endpoint = strip_endpoint(node->endpoint);

  StorageTarget out;
  out.name = node->name;
  // 按已确认的方案：webdav 默认走 http://（表里没有 protocol 字段）
  out.root_url = cat("http://", endpoint);
  out.base_path = *base_path;
  out.username = cfg["username"].get<std::string>();
  out.password = cfg["password"].get<std::string>();
  return out;
}

StorageOptions storage_options_for(const StorageTarget& target) {
  StorageOptions opts;
  opts.user = target.username;
  opts.password = target.password;
  opts.user_agent = "storage-bridge/0.1";
  return opts;
}

// params 里的可选路径数组：省略 -> 空；必须是字符串数组，元素按 rel path 规范化
Result<std::vector<std::string>> optional_path_array(const nlohmann::json& params,
                                                     const char* key) {
  std::vector<std::string> out;
  if (!params.contains(key)) {
    return out;
  }
  if (!params[key].is_array()) {
    return err(ErrorCode::InvalidArgument, cat("params.", key, " 必须是字符串数组"));
  }
  for (const auto& item : params[key]) {
    if (!item.is_string()) {
      return err(ErrorCode::InvalidArgument, cat("params.", key, " 必须是字符串数组"));
    }
    auto rel = normalize_rel_path(item.get<std::string>());
    if (!rel) {
      return rel.error();
    }
    out.push_back(*rel);
  }
  return out;
}

Result<nlohmann::json> op_file_list(PgStore& store, JobWorker&, const nlohmann::json& params) {
  auto storage_id = require_storage_id(params);
  if (!storage_id) {
    return storage_id.error();
  }

  auto raw_path = require_string(params, "path", "例如 /aaa 或 /");
  if (!raw_path) {
    return raw_path.error();
  }
  const std::string req_path = *raw_path;
  auto rel_path = normalize_rel_path(req_path);
  if (!rel_path) {
    return rel_path.error();
  }

  auto target = load_storage_target(store, *storage_id);
  if (!target) {
    return target.error();
  }

  auto joined = build_storage_path(target->base_path, *rel_path);
  if (!joined) {
    return joined.error();
  }
  const std::string url = cat(target->root_url, *joined);

  auto storage = Storage::create(storage_options_for(*target));
  if (!storage) {
    return storage.error();
  }

  nlohmann::json entries = nlohmann::json::array();
  Status listed = (*storage)->list(url, [&](const ObjectRef& ref) -> Status {
    entries.push_back({
        {"name", ref.name},
        {"path", cat(*joined, ref.name)},
        {"rel_path", join_key(*rel_path, ref.name)},
        {"uri", ref.uri},
        {"size", ref.size},
        {"mtime", ref.mtime},
        {"is_dir", ref.is_dir},
    });
    return {};
  });
  if (!listed) {
    return listed.error();
  }

  std::sort(entries.begin(), entries.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    const bool ad = a["is_dir"].get<bool>();
    const bool bd = b["is_dir"].get<bool>();
    if (ad != bd) {
      return ad;
    }
    return a["name"].get<std::string>() < b["name"].get<std::string>();
  });

  return nlohmann::json{
      {"storage_id", *storage_id},      {"name", target->name},
      {"base_path", target->base_path}, {"path", req_path.empty() ? "/" : req_path},
      {"rel_path", *rel_path},          {"uri", url},
      {"count", entries.size()},        {"entries", entries},
  };
}

// scan.file：哈希模式。params = {storage_id, files?:[...], folders?:[...]}
// 每个文件读完算 SHA-256，再按 entities / entity_locations 的口径写库。
// 只报「添加」是否失败；读不动、算不出哈希属于扫描失败，不在返回值里体现。
Result<nlohmann::json> op_scan_file(PgStore& store, JobWorker& jobs, const nlohmann::json& params) {
  auto storage_id = require_storage_id(params);
  if (!storage_id) {
    return storage_id.error();
  }

  auto files = optional_path_array(params, "files");
  if (!files) {
    return files.error();
  }
  auto folders = optional_path_array(params, "folders");
  if (!folders) {
    return folders.error();
  }
  if (files->empty() && folders->empty()) {
    return err(ErrorCode::InvalidArgument, "params.files 和 params.folders 至少要给一个非空数组");
  }

  // mode: full（默认，读内容算哈希）/ verify（先查大小，命中就跳过）
  ScanMode mode = ScanMode::Full;
  if (params.contains("mode")) {
    if (!params["mode"].is_string()) {
      return err(ErrorCode::InvalidArgument, "params.mode 必须是字符串（full / verify）");
    }
    const std::string raw = trim(params["mode"].get<std::string>());
    const std::string lowered = to_lower(raw);
    if (lowered == "full") {
      mode = ScanMode::Full;
    } else if (lowered == "verify") {
      mode = ScanMode::Verify;
    } else {
      return err(ErrorCode::InvalidArgument, "params.mode 只能是 full 或 verify", raw);
    }
  }

  auto target = load_storage_target(store, *storage_id);
  if (!target) {
    return target.error();
  }

  auto storage = Storage::create(storage_options_for(*target));
  if (!storage) {
    return storage.error();
  }

  auto ctx = std::make_shared<ScanContext>();
  ctx->storage = std::shared_ptr<Storage>(std::move(*storage));
  ctx->storage_id = *storage_id;
  ctx->pg = &store;
  ctx->mode = mode;

  // 先把所有目标都校验/建好，避免提交到一半才发现某个路径非法
  std::vector<std::unique_ptr<Job>> built;
  for (const std::string& rel : *folders) {
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
  for (const std::string& rel : *files) {
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
    Status st = jobs.submit(std::move(job));
    if (!st) {
      if (st.error().code == ErrorCode::Busy) {
        return err(ErrorCode::Busy, "任务队列已满，未能全部加入",
                   cat("已加入 ", std::to_string(queued), "/", std::to_string(total)));
      }
      return st.error();
    }
    ++queued;
  }

  log(LogLevel::Info, cat("scan.file accepted: storage=", target->name, " (", *storage_id,
                          ") queued=", std::to_string(queued), " mode=", to_string(mode)));

  // 只说明"任务已加入队列"，扫描在后台线程里跑，结果以日志为准
  return nlohmann::json{
      {"accepted", true},
      {"storage_id", *storage_id},
      {"queued", queued},
      {"mode", to_string(mode)},
  };
}

Result<nlohmann::json> op_health(PgStore& store, JobWorker&, const nlohmann::json&) {
  if (Status st = store.ping(); !st) {
    return nlohmann::json{
        {"status", "degraded"},
        {"db", "down"},
        {"error", st.error().message},
    };
  }
  return nlohmann::json{{"status", "ok"}, {"db", "up"}};
}

using OpHandler = Result<nlohmann::json> (*)(PgStore&, JobWorker&, const nlohmann::json&);

struct OpEntry {
  const char* name;
  OpHandler fn;
};

const OpEntry kOps[] = {
    {"storage.list", op_storage_list},     {"storage.get", op_storage_get},
    {"storage.upsert", op_storage_upsert}, {"file.list", op_file_list},
    {"scan.file", op_scan_file},           {"health", op_health},
};

// POST /api：唯一入口，{"op":..,"params":{..}} -> {"data":..} / {"data":null,"errors":[..]}
void handle_api(PgStore& store, JobWorker& jobs, const httplib::Request& req,
                httplib::Response& res) {
  nlohmann::json body;
  try {
    body = nlohmann::json::parse(req.body);
  } catch (const nlohmann::json::exception& e) {
    send_one_error(res, 400, err(ErrorCode::InvalidArgument, "请求体不是合法 JSON", e.what()));
    return;
  }
  if (!body.is_object()) {
    send_one_error(res, 400, err(ErrorCode::InvalidArgument, "请求体必须是 JSON 对象"));
    return;
  }
  if (!body.contains("op") || !body["op"].is_string()) {
    send_one_error(res, 400, err(ErrorCode::InvalidArgument, "op 必填（例如 \"storage.list\"）"));
    return;
  }
  const std::string op = trim(body["op"].get<std::string>());
  if (op.empty()) {
    send_one_error(res, 400, err(ErrorCode::InvalidArgument, "op 不能为空"));
    return;
  }

  nlohmann::json params = nlohmann::json::object();
  if (body.contains("params") && !body["params"].is_null()) {
    if (!body["params"].is_object()) {
      send_one_error(res, 400, err(ErrorCode::InvalidArgument, "params 必须是 JSON 对象"));
      return;
    }
    params = body["params"];
  }

  for (const auto& entry : kOps) {
    if (op != entry.name) {
      continue;
    }
    auto out = entry.fn(store, jobs, params);
    if (!out) {
      send_one_error(res, 200, out.error());
      return;
    }
    send_data(res, *out);
    return;
  }

  std::string known;
  for (const auto& entry : kOps) {
    if (!known.empty()) {
      known += ", ";
    }
    known += entry.name;
  }
  send_one_error(res, 200,
                 err(ErrorCode::InvalidArgument, cat("未知 op: ", op), cat("已支持: ", known)));
}

} // namespace

std::string library_info() {
  // 真正构造一次 Server（不是只读宏），确保动态库符号可解析
  httplib::Server probe;
  const bool alive = probe.is_running();

  std::ostringstream os;
  os << "cpp-httplib " << CPPHTTPLIB_VERSION << " (server_ok=" << (alive ? "1" : "0") << ")"
     << ", nlohmann_json " << NLOHMANN_JSON_VERSION_MAJOR << '.' << NLOHMANN_JSON_VERSION_MINOR
     << '.' << NLOHMANN_JSON_VERSION_PATCH;
  return os.str();
}

Status serve(const ServerOptions& opts, PgStore& store) {
  httplib::Server svr;

  // 常驻扫描 worker：scan.file 只负责入队，真正的扫描在这几个线程里后台跑。
  // 队列上限 1000（文件+目录合计），超了 scan.file 返回 busy。
  const int threads = opts.scan_threads > 0 ? opts.scan_threads : 1;
  JobWorker jobs(static_cast<std::size_t>(threads), 1000);
  if (Status st = jobs.start(); !st) {
    return st;
  }

  svr.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try {
          std::rethrow_exception(ep);
        } catch (const std::exception& e) {
          msg = e.what();
        } catch (...) {
          msg = "unknown exception";
        }
        // 框架层兜底：第三方库（httplib/json）仍可能抛异常
        send_one_error(res, 500, err(ErrorCode::Internal, "请求处理异常", msg));
      });

  svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
    log(LogLevel::Debug,
        cat("http ", req.method, " ", req.path, " -> ", std::to_string(res.status)));
  });

  // 静态文件树页面：/ui/ 挂到 web_root；GET / 跳转，避免和 API 路由混在一起
  bool web_ready = false;
  if (!opts.web_root.empty()) {
    std::error_code ec;
    if (std::filesystem::is_directory(opts.web_root, ec)) {
      web_ready = svr.set_mount_point("/ui", opts.web_root);
      if (!web_ready) {
        log(LogLevel::Warn, cat("web_root 挂载失败: ", opts.web_root));
      }
    } else {
      log(LogLevel::Warn, cat("web_root 目录不存在: ", opts.web_root));
    }
  }
  svr.Get("/", [web_ready](const httplib::Request&, httplib::Response& res) {
    if (web_ready) {
      res.set_redirect("/ui/");
      return;
    }
    res.status = 503;
    res.set_content("web UI 未启用：请从项目根目录启动，或用 --web-root 指定 web 目录\n",
                    "text/plain; charset=utf-8");
  });

  svr.Post("/api", [&store, &jobs](const httplib::Request& req, httplib::Response& res) {
    handle_api(store, jobs, req, res);
  });

  g_server = &svr;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  log(LogLevel::Info, cat("http listening on http://", opts.bind, ":", std::to_string(opts.port)));
  log(LogLevel::Info, "  GET  /        -> /ui/ 文件树页面");
  log(LogLevel::Info, "  POST /api     统一信封 {op, params} -> {data} / {data, errors[]}");
  for (const auto& entry : kOps) {
    log(LogLevel::Info, cat("          op = ", entry.name));
  }

  log(LogLevel::Info,
      cat("  scan worker   ", std::to_string(threads), " threads, queue limit 1000"));

  const bool listened = svr.listen(opts.bind, opts.port);
  g_server = nullptr;

  // 先停 HTTP（不再收新任务），再让 worker 把队列里的跑完
  log(LogLevel::Info, "http server stopped, draining scan queue...");
  jobs.stop();
  {
    const JobWorkerStats st = jobs.stats();
    log(LogLevel::Info,
        cat("scan worker stopped: completed=", std::to_string(st.completed),
            " failed=", std::to_string(st.failed), " yields=", std::to_string(st.yields)));
  }

  if (!listened) {
    return err(ErrorCode::Io, "HTTP 监听失败", cat(opts.bind, ":", std::to_string(opts.port)));
  }
  return {};
}

} // namespace sb::web
