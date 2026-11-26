#include "support/http_client.hpp"
#include "support/integration_test.hpp"
#include "support/json_helpers.hpp"

#include <gtest/gtest.h>

namespace sb::test {
namespace {

TEST_F(IntegrationTest, HealthOk) {
  EXPECT_CALL(runtime().db(), ping()).WillOnce(testing::Return(Status{}));

  const HttpResponse response = http_get(runtime().port(), "/api/health");
  EXPECT_EQ(response.status, 200);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["status"].asString(), "ok");
  EXPECT_EQ(body["db"].asString(), "up");
}

TEST_F(IntegrationTest, HealthDegradedWhenDatabaseFails) {
  EXPECT_CALL(runtime().db(), ping())
      .WillOnce(testing::Return(err(ErrorCode::Postgres, "数据库不可用")));

  const HttpResponse response = http_get(runtime().port(), "/api/health");
  EXPECT_EQ(response.status, 503);
  const auto body = parse_json(response.body);
  EXPECT_EQ(body["status"].asString(), "degraded");
  EXPECT_EQ(body["db"].asString(), "down");
  EXPECT_EQ(body["error"].asString(), "数据库不可用");
}

} // namespace
} // namespace sb::test