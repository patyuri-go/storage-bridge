#include "config.hpp"
#include "pg_store.hpp"
#include "util.hpp"
#include "web/server.hpp"

#include <openssl/opensslv.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace sb;

void print_usage(const char* argv0) {
  std::cout << R"(storage-bridge — 存储端（WebDAV/S3）↔ 远端 PostgreSQL 资源库

usage: )" << argv0
            << R"( --serve [options]

服务模式:
  --serve                  启动 HTTP 服务（POST /api 统一入口，读取远端 storage_nodes）
  --listen <ip:port>       覆盖配置文件里的 server.bind/server.port
  --web-root <dir>         文件树静态资源目录，默认 web（相对当前目录）

配置:
  --config <path>          监听地址 + PostgreSQL 连接信息 JSON，默认 config/storage-bridge.json
  --print-config           打印加载到的配置（密码打码）后退出

其他:
  --version                打印各依赖库版本
  -v/--verbose             打印 Debug 日志
  -q/--quiet               只打错误
  -h/--help
)";
}

struct Cli {
  std::string config_path;
  bool serve = false;
  std::string listen; // 空 = 用配置文件 server 块
  std::string web_root = "web";
  bool print_config = false;
};

bool next_value(int argc, char** argv, int& i, const char* flag, std::string& out) {
  if (i + 1 >= argc) {
    std::cerr << "missing value for " << flag << "\n";
    return false;
  }
  out = argv[++i];
  return true;
}

} // namespace

int main(int argc, char** argv) {
  Cli cli;
  set_log_level(LogLevel::Info);

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--version") {
      std::cout << "storage-bridge 0.1.0\n"
                << "  davix       " << SB_DAVIX_VERSION << "\n"
                << "  OpenSSL     " << OPENSSL_VERSION_TEXT << "\n"
                << "  " << sb::web::library_info() << "\n";
      return 0;
    } else if (arg == "--config") {
      if (!next_value(argc, argv, i, "--config", cli.config_path))
        return 2;
    } else if (arg == "--print-config") {
      cli.print_config = true;
    } else if (arg == "--serve") {
      cli.serve = true;
    } else if (arg == "--listen") {
      if (!next_value(argc, argv, i, "--listen", cli.listen))
        return 2;
    } else if (arg == "--web-root") {
      if (!next_value(argc, argv, i, "--web-root", cli.web_root))
        return 2;
    } else if (arg == "-v" || arg == "--verbose") {
      set_log_level(LogLevel::Debug);
    } else if (arg == "-q" || arg == "--quiet") {
      set_log_level(LogLevel::Error);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  // ---- 配置文件：只承载 PostgreSQL 连接信息与监听地址
  if (cli.config_path.empty()) {
    const std::string def = "config/storage-bridge.json";
    if (std::ifstream(def).good()) {
      cli.config_path = def;
    }
  }
  LoadedConfig cfg;
  if (!cli.config_path.empty()) {
    auto loaded = load_config(cli.config_path);
    if (!loaded) {
      std::cerr << "配置错误: " << loaded.error().to_string() << "\n";
      return 2;
    }
    cfg = std::move(*loaded);
    if (!cfg.loaded) {
      std::cerr << "config file not found: " << cli.config_path << "\n";
      return 2;
    }
  }
  if (cli.print_config) {
    auto text = config_dump_redacted(cfg);
    if (!text) {
      std::cerr << text.error().to_string() << "\n";
      return 1;
    }
    std::cout << *text;
    return 0;
  }

  if (!cli.serve) {
    std::cerr << "--serve is required\n\n";
    print_usage(argv[0]);
    return 2;
  }
  if (!cfg.loaded) {
    std::cerr << "--serve 需要配置文件（--config，默认 config/storage-bridge.json）\n";
    return 2;
  }

  web::ServerOptions web_opts;
  web_opts.bind = cfg.server.bind;
  web_opts.port = cfg.server.port;
  web_opts.web_root = cli.web_root;
  web_opts.scan_threads = cfg.server.scan_threads;

  // --listen 显式给出时覆盖配置文件
  if (!cli.listen.empty()) {
    const auto colon = cli.listen.rfind(':');
    if (colon == std::string::npos) {
      std::cerr << "--listen 需要 <ip:port>，例如 127.0.0.1:8080\n";
      return 2;
    }
    web_opts.bind = cli.listen.substr(0, colon);
    web_opts.port = std::atoi(cli.listen.c_str() + colon + 1);
    if (web_opts.bind.empty() || web_opts.port <= 0 || web_opts.port > 65535) {
      std::cerr << "非法的 --listen 地址: " << cli.listen << "\n";
      return 2;
    }
  }

  auto store = PgStore::create(cfg.db);
  if (!store) {
    std::cerr << "启动失败: " << store.error().to_string() << "\n";
    return 1;
  }
  if (Status st = web::serve(web_opts, **store); !st) {
    log(LogLevel::Error, cat("http 服务失败: ", st.error().to_string()));
    return 1;
  }
  return 0;
}
