#include "config.hpp"
#include "support/assertions.hpp"
#include "support/json_helpers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace sb {
namespace {

namespace fs = std::filesystem;

class TempConfig {
public:
  explicit TempConfig(const std::string& content) {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("sb_config_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
    std::ofstream out(path_);
    out << content;
  }

  ~TempConfig() {
    std::error_code ec;
    fs::remove(path_, ec);
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

class ScopedEnv {
public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name)) {
      old_ = old;
    }
    setenv(name, value, 1);
  }

  ~ScopedEnv() {
    if (old_) {
      setenv(name_.c_str(), old_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

private:
  std::string name_;
  std::optional<std::string> old_;
};

const char* kValidConfig = R"({
  "database": {
    "host": "db.example",
    "port": 5433,
    "dbname": "entity",
    "user": "alice",
    "password": "filepass",
    "sslmode": "require",
    "connect_timeout": 7,
    "application_name": "sb-test"
  },
  "server": {
    "bind": "0.0.0.0",
    "port": 9000,
    "scan_threads": 2,
    "db_threads": 3
  }
})";

TEST(UnitConfig, MissingFileIsNotAnError) {
  const auto path = (fs::temp_directory_path() / "sb_config_missing_123456").string();
  auto loaded = load_config(path);
  ASSERT_TRUE(loaded.ok());
  EXPECT_FALSE(loaded->loaded);
  EXPECT_EQ(loaded->path, path);
}

TEST(UnitConfig, LoadsAllFields) {
  const TempConfig file(kValidConfig);
  auto loaded = load_config(file.path());
  ASSERT_TRUE(loaded.ok());
  ASSERT_TRUE(loaded->loaded);
  EXPECT_EQ(loaded->db.host, "db.example");
  EXPECT_EQ(loaded->db.port, 5433);
  EXPECT_EQ(loaded->db.dbname, "entity");
  EXPECT_EQ(loaded->db.user, "alice");
  EXPECT_EQ(loaded->db.password, "filepass");
  EXPECT_EQ(loaded->db.sslmode, "require");
  EXPECT_EQ(loaded->db.connect_timeout, 7);
  EXPECT_EQ(loaded->db.application_name, "sb-test");
  EXPECT_EQ(loaded->server.bind, "0.0.0.0");
  EXPECT_EQ(loaded->server.port, 9000);
  EXPECT_EQ(loaded->server.scan_threads, 2);
  EXPECT_EQ(loaded->server.db_threads, 3);
}

TEST(UnitConfig, DefaultsForOmittedFields) {
  const TempConfig file(R"({"database":{}})");
  auto loaded = load_config(file.path());
  ASSERT_TRUE(loaded.ok());
  ASSERT_TRUE(loaded->loaded);
  EXPECT_EQ(loaded->db.host, "127.0.0.1");
  EXPECT_EQ(loaded->db.port, 5432);
  EXPECT_EQ(loaded->server.port, 8080);
  EXPECT_EQ(loaded->server.scan_threads, 4);
}

TEST(UnitConfig, InvalidShapes) {
  const char* bad[] = {
      "not json",
      "[]",
      "{}",
      R"({"database":[]})",
      R"({"database":{"port":"x"}})",
      R"({"database":{"host":1}})",
      R"({"database":{},"server":{"port":0}})",
      R"({"database":{},"server":{"scan_threads":0}})",
      R"({"database":{},"server":{"db_threads":65}})",
  };
  for (const char* body : bad) {
    SCOPED_TRACE(body);
    const TempConfig file(body);
    expect_error(load_config(file.path()), ErrorCode::Config);
  }
}

TEST(UnitConfig, EnvironmentPasswordOverridesFile) {
  ScopedEnv env("SB_PG_PASSWORD", "envpass");
  const TempConfig file(kValidConfig);
  auto loaded = load_config(file.path());
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded->db.password, "envpass");
}

TEST(UnitConfig, RedactedDumpHidesPassword) {
  const TempConfig file(kValidConfig);
  auto loaded = load_config(file.path());
  ASSERT_TRUE(loaded.ok());

  auto dump = config_dump_redacted(*loaded);
  ASSERT_TRUE(dump.ok());
  EXPECT_EQ(dump->find("filepass"), std::string::npos);

  const auto json = test::parse_json(*dump);
  EXPECT_EQ(json["database"]["password"].asString(), "***");
  EXPECT_EQ(json["database"]["host"].asString(), "db.example");
  EXPECT_EQ(json["_path"].asString(), file.path());
}

TEST(UnitConfig, RedactedDumpWhenNotLoaded) {
  LoadedConfig loaded;
  auto dump = config_dump_redacted(loaded);
  ASSERT_TRUE(dump.ok());
  EXPECT_NE(dump->find("未加载配置"), std::string::npos);
}

} // namespace
} // namespace sb