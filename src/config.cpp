#include "config.hpp"

#include "util.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

#include <json/json.h>

namespace sb {

namespace {

bool is_integral(const Json::Value& v) {
  return v.isInt() || v.isUInt() || v.isInt64() || v.isUInt64();
}

Result<std::string> json_string(const Json::Value& j, const char* key, const std::string& def,
                                const std::string& path) {
  if (!j.isMember(key) || j[key].isNull()) {
    return def;
  }
  const Json::Value& v = j[key];
  if (!v.isString()) {
    return err(ErrorCode::Config, cat("配置项 '", key, "' 必须是字符串"), "", path);
  }
  return v.asString();
}

Result<int> json_int(const Json::Value& j, const char* key, int def, const std::string& path) {
  if (!j.isMember(key) || j[key].isNull()) {
    return def;
  }
  const Json::Value& v = j[key];
  if (!is_integral(v)) {
    return err(ErrorCode::Config, cat("配置项 '", key, "' 必须是整数"), "", path);
  }
  return v.asInt();
}

} // namespace

std::string DbConfig::conninfo() const {
  std::ostringstream os;
  os << "host=" << host << " port=" << port << " dbname=" << dbname << " user=" << user
     << " sslmode=" << sslmode << " connect_timeout=" << connect_timeout
     << " application_name=" << application_name;
  if (!password.empty()) {
    // libpq 的引号值；密码里可能出现空格/单引号
    os << " password='";
    for (char c : password) {
      if (c == '\'' || c == '\\') {
        os << '\\';
      }
      os << c;
    }
    os << "'";
  }
  return os.str();
}

std::string DbConfig::redacted() const {
  return cat("postgresql://", user, ":***@", host, ":", std::to_string(port), "/", dbname,
             " (sslmode=", sslmode, ")");
}

Result<LoadedConfig> load_config(const std::string& path) {
  LoadedConfig out;
  out.path = path;

  std::ifstream in(path);
  if (!in) {
    return out; // 文件不存在：loaded=false，由调用方决定是否可接受
  }

  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errs;
  if (!Json::parseFromStream(builder, in, &root, &errs)) {
    if (errs.empty()) {
      errs = "无法解析";
    }
    return err(ErrorCode::Config, "配置文件不是合法 JSON", trim(errs), path);
  }
  if (!root.isObject()) {
    return err(ErrorCode::Config, "配置文件顶层必须是对象", "", path);
  }

  if (!root.isMember("database") || !root["database"].isObject()) {
    return err(ErrorCode::Config, "缺少对象 'database'", "", path);
  }
  const Json::Value& db = root["database"];

  auto& c = out.db;
  auto str_field = [&](const char* key, std::string& target) -> Status {
    auto v = json_string(db, key, target, path);
    if (!v) {
      return v.error();
    }
    target = std::move(*v);
    return {};
  };
  auto int_field = [&](const char* key, int& target) -> Status {
    auto v = json_int(db, key, target, path);
    if (!v) {
      return v.error();
    }
    target = *v;
    return {};
  };

  if (Status st = str_field("host", c.host); !st)
    return st.error();
  if (Status st = int_field("port", c.port); !st)
    return st.error();
  if (Status st = str_field("dbname", c.dbname); !st)
    return st.error();
  if (Status st = str_field("user", c.user); !st)
    return st.error();
  if (Status st = str_field("password", c.password); !st)
    return st.error();
  if (Status st = str_field("sslmode", c.sslmode); !st)
    return st.error();
  if (Status st = int_field("connect_timeout", c.connect_timeout); !st)
    return st.error();
  if (Status st = str_field("application_name", c.application_name); !st)
    return st.error();

  // server 块可选；缺省 127.0.0.1:8080
  if (root.isMember("server") && !root["server"].isNull()) {
    const Json::Value& srv = root["server"];
    if (!srv.isObject()) {
      return err(ErrorCode::Config, "配置项 'server' 必须是对象", "", path);
    }
    auto bind = json_string(srv, "bind", out.server.bind, path);
    if (!bind)
      return bind.error();
    out.server.bind = std::move(*bind);

    auto port = json_int(srv, "port", out.server.port, path);
    if (!port)
      return port.error();
    out.server.port = *port;
    if (out.server.port <= 0 || out.server.port > 65535) {
      return err(ErrorCode::Config, "配置项 'server.port' 必须在 1..65535", "", path);
    }

    auto threads = json_int(srv, "scan_threads", out.server.scan_threads, path);
    if (!threads)
      return threads.error();
    out.server.scan_threads = *threads;
    if (out.server.scan_threads < 1 || out.server.scan_threads > 64) {
      return err(ErrorCode::Config, "配置项 'server.scan_threads' 必须在 1..64", "", path);
    }

    auto db_threads = json_int(srv, "db_threads", out.server.db_threads, path);
    if (!db_threads)
      return db_threads.error();
    out.server.db_threads = *db_threads;
    if (out.server.db_threads < 1 || out.server.db_threads > 64) {
      return err(ErrorCode::Config, "配置项 'server.db_threads' 必须在 1..64", "", path);
    }
  }

  // 密码优先级：环境变量 > 配置文件（避免把生产密码提交进仓库）
  if (const char* env = std::getenv("SB_PG_PASSWORD"); env != nullptr && *env != '\0') {
    c.password = env;
  }

  out.loaded = true;
  return out;
}

Result<std::string> config_dump_redacted(const LoadedConfig& loaded) {
  if (!loaded.loaded) {
    return std::string("-- 未加载配置\n");
  }
  const auto& c = loaded.db;
  Json::Value root(Json::objectValue);
  root["_path"] = loaded.path;

  Json::Value server(Json::objectValue);
  server["bind"] = loaded.server.bind;
  server["port"] = loaded.server.port;
  server["scan_threads"] = loaded.server.scan_threads;
  server["db_threads"] = loaded.server.db_threads;
  root["server"] = server;

  Json::Value database(Json::objectValue);
  database["host"] = c.host;
  database["port"] = c.port;
  database["dbname"] = c.dbname;
  database["user"] = c.user;
  database["password"] = c.password.empty() ? "" : "***";
  database["sslmode"] = c.sslmode;
  database["connect_timeout"] = c.connect_timeout;
  database["application_name"] = c.application_name;
  root["database"] = database;

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  writer["emitUTF8"] = true;
  try {
    return Json::writeString(writer, root) + "\n";
  } catch (const std::exception& e) {
    return err(ErrorCode::Internal, "序列化配置失败", e.what(), loaded.path);
  }
}

} // namespace sb