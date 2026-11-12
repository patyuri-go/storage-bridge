#pragma once

#include "result.hpp"

#include <string>

namespace sb {

// 远端 PostgreSQL 连接信息（库名 entity）。配置文件里只放这一块。
struct DbConfig {
  std::string host = "127.0.0.1";
  int port = 5432;
  std::string dbname = "entity";
  std::string user = "postgres";
  std::string password; // 可用环境变量 SB_PG_PASSWORD 覆盖（不落日志）
  std::string sslmode = "prefer";
  int connect_timeout = 10;
  std::string application_name = "storage-bridge";

  // libpq 连接串（keyword=value）
  std::string conninfo() const;
  std::string redacted() const; // 打日志/--print-config 用
};

// 程序自身的 HTTP 监听地址（--serve 时生效；--listen 可覆盖）
struct ServerConfig {
  std::string bind = "127.0.0.1";
  int port = 8080;
  // 常驻扫描 worker 的线程数（1..64）
  int scan_threads = 4;
};

struct LoadedConfig {
  DbConfig db;
  ServerConfig server;
  bool loaded = false;
  std::string path;
};

// 读取 JSON 配置；文件不存在返回 loaded=false（不算错误）
Result<LoadedConfig> load_config(const std::string& path);

// 已加载配置的 redacted JSON（--print-config）
Result<std::string> config_dump_redacted(const LoadedConfig& loaded);

} // namespace sb
