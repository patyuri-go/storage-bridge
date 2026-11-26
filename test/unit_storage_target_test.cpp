#include "support/assertions.hpp"
#include "support/mock_database.hpp"
#include "web/storage_target.hpp"

#include <gtest/gtest.h>

#include <string>

namespace sb::web {
namespace {

TEST(UnitStorageTarget, IsUuid) {
  EXPECT_TRUE(is_uuid("2bd2095e-c610-4242-a5e7-b2ffc8bdbb10"));
  EXPECT_TRUE(is_uuid("ABCDEF01-2345-6789-ABCD-EF0123456789"));
  EXPECT_FALSE(is_uuid(""));
  EXPECT_FALSE(is_uuid("2bd2095e-c610-4242-a5e7-b2ffc8bdbb1"));
  EXPECT_FALSE(is_uuid("2bd2095e_c610-4242-a5e7-b2ffc8bdbb10"));
  EXPECT_FALSE(is_uuid("2bd2095e-c610-4242-a5e7-b2ffc8bdbb1z"));
}

TEST(UnitStorageTarget, StripEndpoint) {
  EXPECT_EQ(strip_endpoint("http://h:5244/"), "h:5244");
  EXPECT_EQ(strip_endpoint("https://h"), "h");
  EXPECT_EQ(strip_endpoint("h:1"), "h:1");
}

TEST(UnitStorageTarget, NormalizeEndpoint) {
  EXPECT_EQ(*normalize_endpoint(" http://h:5244/ "), "h:5244");
  EXPECT_EQ(*normalize_endpoint("example.com"), "example.com");

  for (const char* bad : {"", "   ", "h/dav", "h h"}) {
    SCOPED_TRACE(bad);
    expect_error(normalize_endpoint(bad), ErrorCode::InvalidArgument);
  }
}

TEST(UnitStorageTarget, NormalizeBasePath) {
  EXPECT_EQ(*normalize_base_path("dav//tian/"), "/dav/tian/");
  EXPECT_EQ(*normalize_base_path("/"), "/");
  EXPECT_EQ(*normalize_base_path("  /a//b  "), "/a/b/");
  expect_error(normalize_base_path("   "), ErrorCode::InvalidArgument);
}

TEST(UnitStorageTarget, NormalizeRelPath) {
  EXPECT_EQ(*normalize_rel_path(""), "/");
  EXPECT_EQ(*normalize_rel_path("/"), "/");
  EXPECT_EQ(*normalize_rel_path("a//b/./c"), "/a/b/c");
  EXPECT_EQ(*normalize_rel_path(" /a/b/ "), "/a/b");
  EXPECT_EQ(*normalize_rel_path("a/./b"), "/a/b");
  expect_error(normalize_rel_path("/a/../b"), ErrorCode::InvalidArgument);
  expect_error(normalize_rel_path(".."), ErrorCode::InvalidArgument);
}

TEST(UnitStorageTarget, BuildStoragePath) {
  EXPECT_EQ(*build_storage_path("/dav/", "/"), "/dav/");
  EXPECT_EQ(*build_storage_path("/dav/", ""), "/dav/");
  EXPECT_EQ(*build_storage_path("/dav/", "/a/b"), "/dav/a/b/");
}

TEST(UnitStorageTarget, MapConflict) {
  const Error e = map_conflict(err(ErrorCode::Postgres, "重复", "23505"));
  EXPECT_EQ(e.code, ErrorCode::Conflict);
  EXPECT_NE(e.message.find("唯一约束冲突"), std::string::npos);

  const Error other = map_conflict(err(ErrorCode::Postgres, "其他", "42P01"));
  EXPECT_EQ(other.code, ErrorCode::Postgres);
  EXPECT_EQ(other.message, "其他");
}

TEST(UnitStorageTarget, MakeStorageTarget) {
  const std::string cfg = R"({"username":"u","password":"p","base_path":"/dav"})";
  auto target =
      make_storage_target("sid", "测试库", "webdav", "http://h:5244/", cfg);
  ASSERT_TRUE(target.ok());
  EXPECT_EQ(target->name, "测试库");
  EXPECT_EQ(target->root_url, "http://h:5244");
  EXPECT_EQ(target->base_path, "/dav/");
  EXPECT_EQ(target->username, "u");
  EXPECT_EQ(target->password, "p");

  expect_error(make_storage_target("sid", "n", "s3", "h:1", cfg), ErrorCode::InvalidArgument,
               "webdav");
  expect_error(make_storage_target("sid", "n", "webdav", "h:1", R"({"username":"u"})"),
               ErrorCode::Internal, "password");
  expect_error(make_storage_target("sid", "n", "webdav", "h:1", "not-json"), ErrorCode::Internal,
               "JSON");
}

TEST(UnitStorageTarget, LoadStorageTarget) {
  testing::NiceMock<::sb::test::MockDatabase> db;
  StorageNodeRecord rec;
  rec.storage_id = "sid";
  rec.name = "库";
  rec.type = "webdav";
  rec.endpoint = "h:1";
  rec.config_json = R"({"username":"u","password":"p","base_path":"/dav"})";

  EXPECT_CALL(db, get_storage("sid")).WillOnce(testing::Return(rec));
  auto target = load_storage_target(db, "sid");
  ASSERT_TRUE(target.ok());
  EXPECT_EQ(target->root_url, "http://h:1");
  EXPECT_EQ(target->base_path, "/dav/");

  EXPECT_CALL(db, get_storage("missing"))
      .WillOnce(testing::Return(err(ErrorCode::NotFound, "storage 不存在")));
  expect_error(load_storage_target(db, "missing"), ErrorCode::NotFound, "storage 不存在");
}

} // namespace
} // namespace sb::web