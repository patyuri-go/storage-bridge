#include "support/http_client.hpp"
#include "support/integration_test.hpp"
#include "support/json_helpers.hpp"
#include "support/mock_database.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sb::test {
namespace {

constexpr const char* kStorageId = "2bd2095e-c610-4242-a5e7-b2ffc8bdbb10";

StorageNodeRecord sample_record() {
  StorageNodeRecord rec;
  rec.storage_id = kStorageId;
  rec.name = "测试库";
  rec.type = "webdav";
  rec.endpoint = "10.0.0.1:5244";
  rec.config_json = R"({"username":"u","password":"p","base_path":"/dav"})";
  rec.state = "active";
  return rec;
}

std::string create_body() {
  return R"({
    "create": true,
    "name": "新库",
    "type": "webdav",
    "endpoint": "http://10.0.0.1:5244/",
    "state": "active",
    "config": {"username":"u","password":"p","base_path":"dav"}
  })";
}

TEST_F(IntegrationTest, ListStorages) {
  EXPECT_CALL(runtime().db(), list_storages())
      .WillOnce(testing::Return(std::vector<StorageNodeRecord>{sample_record()}));

  const HttpResponse response = http_get(runtime().port(), "/api/storages");
  EXPECT_EQ(response.status, 200);
  const auto body = parse_json(response.body);
  ASSERT_TRUE(body.isArray());
  ASSERT_EQ(body.size(), 1u);
  const auto& item = body[0];
  EXPECT_EQ(item["storage_id"].asString(), kStorageId);
  EXPECT_EQ(item["name"].asString(), "测试库");
  EXPECT_EQ(item["config"]["username"].asString(), "u");
  EXPECT_EQ(item["config"]["base_path"].asString(), "/dav");
  EXPECT_TRUE(item["config"]["has_password"].asBool());
  EXPECT_FALSE(item["config"].isMember("password"));
}

TEST_F(IntegrationTest, ListStoragesPropagatesDatabaseError) {
  EXPECT_CALL(runtime().db(), list_storages())
      .WillOnce(testing::Return(err(ErrorCode::Postgres, "查询失败")));

  const HttpResponse response = http_get(runtime().port(), "/api/storages");
  EXPECT_EQ(response.status, 500);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["error"]["code"].asString(), "postgres");
}

TEST_F(IntegrationTest, GetStorage) {
  EXPECT_CALL(runtime().db(), get_storage(kStorageId))
      .WillOnce(testing::Return(sample_record()));

  const HttpResponse response =
      http_get(runtime().port(), std::string("/api/storages/") + kStorageId);
  EXPECT_EQ(response.status, 200);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["storage_id"].asString(), kStorageId);
  EXPECT_EQ(body["name"].asString(), "测试库");
}

TEST_F(IntegrationTest, GetStorageRejectsInvalidUuid) {
  EXPECT_CALL(runtime().db(), get_storage(testing::_)).Times(0);

  const HttpResponse response = http_get(runtime().port(), "/api/storages/not-a-uuid");
  EXPECT_EQ(response.status, 400);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["error"]["code"].asString(), "invalid_argument");
  EXPECT_NE(body["error"]["message"].asString().find("UUID"), std::string::npos);
}

TEST_F(IntegrationTest, GetStorageNotFound) {
  EXPECT_CALL(runtime().db(), get_storage(kStorageId))
      .WillOnce(testing::Return(err(ErrorCode::NotFound, "storage 不存在")));

  const HttpResponse response =
      http_get(runtime().port(), std::string("/api/storages/") + kStorageId);
  EXPECT_EQ(response.status, 404);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["error"]["code"].asString(), "not_found");
}

TEST_F(IntegrationTest, UpsertRequiresCreateFlag) {
  EXPECT_CALL(runtime().db(), upsert_storage(testing::_)).Times(0);

  const HttpResponse response =
      http_post_json(runtime().port(), "/api/storages", R"({"name":"x"})");
  EXPECT_EQ(response.status, 400);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["error"]["code"].asString(), "invalid_argument");
  EXPECT_NE(body["error"]["message"].asString().find("create"), std::string::npos);
}

TEST_F(IntegrationTest, UpsertRejectsInvalidConfig) {
  EXPECT_CALL(runtime().db(), upsert_storage(testing::_)).Times(0);

  const HttpResponse response = http_post_json(
      runtime().port(), "/api/storages",
      R"({"create":true,"name":"x","type":"webdav","endpoint":"h:1",
          "config":{"username":"","password":"p","base_path":"/"}})");
  EXPECT_EQ(response.status, 400);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["error"]["code"].asString(), "invalid_argument");
}

TEST_F(IntegrationTest, UpsertCreatesStorage) {
  EXPECT_CALL(runtime().db(), upsert_storage(testing::_))
      .WillOnce(testing::Return(StorageNodeResult{"new-id", true}));

  const HttpResponse response =
      http_post_json(runtime().port(), "/api/storages", create_body());
  EXPECT_EQ(response.status, 201);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["storage_id"].asString(), "new-id");
  EXPECT_EQ(body["name"].asString(), "新库");
  EXPECT_EQ(body["config"]["base_path"].asString(), "/dav/");
  EXPECT_TRUE(body["config"]["has_password"].asBool());
  EXPECT_FALSE(body["config"].isMember("password"));
}

TEST_F(IntegrationTest, UpsertUpdateChecksExistingStorage) {
  EXPECT_CALL(runtime().db(), get_storage(kStorageId))
      .WillOnce(testing::Return(err(ErrorCode::NotFound, "storage 不存在")));
  EXPECT_CALL(runtime().db(), upsert_storage(testing::_)).Times(0);

  const HttpResponse response = http_post_json(
      runtime().port(), "/api/storages",
      R"({"create":false,"storage_id":"2bd2095e-c610-4242-a5e7-b2ffc8bdbb10",
          "name":"x","type":"webdav","endpoint":"h:1",
          "config":{"username":"u","password":"p","base_path":"/"}})");
  EXPECT_EQ(response.status, 404);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["error"]["code"].asString(), "not_found");
}

TEST_F(IntegrationTest, UpsertMapsUniqueConflictTo409) {
  EXPECT_CALL(runtime().db(), upsert_storage(testing::_))
      .WillOnce(testing::Return(err(ErrorCode::Postgres, "duplicate", "23505")));

  const HttpResponse response =
      http_post_json(runtime().port(), "/api/storages", create_body());
  EXPECT_EQ(response.status, 409);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["error"]["code"].asString(), "conflict");
  EXPECT_NE(body["error"]["message"].asString().find("唯一约束冲突"), std::string::npos);
}

} // namespace
} // namespace sb::test