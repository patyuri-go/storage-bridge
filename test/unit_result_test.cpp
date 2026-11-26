#include "result.hpp"
#include "support/assertions.hpp"

#include <gtest/gtest.h>

#include <string>

namespace sb {
namespace {

TEST(UnitResult, HoldsValue) {
  Result<int> value = 42;
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(value.value(), 42);
  EXPECT_EQ(*value, 42);
  EXPECT_EQ(value.value_or(7), 42);
}

TEST(UnitResult, HoldsError) {
  Result<int> value = err(ErrorCode::NotFound, "没有这个对象", "detail-x", "/ctx");
  ASSERT_FALSE(value.ok());
  EXPECT_EQ(value.error().code, ErrorCode::NotFound);
  EXPECT_EQ(value.error().message, "没有这个对象");
  EXPECT_EQ(value.error().detail, "detail-x");
  EXPECT_EQ(value.error().context, "/ctx");
  EXPECT_EQ(value.value_or(7), 7);
}

TEST(UnitResult, StatusSuccessAndFailure) {
  Status ok;
  EXPECT_TRUE(ok.ok());

  Status bad = err(ErrorCode::Busy, "队列已满");
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().code, ErrorCode::Busy);
}

TEST(UnitResult, ErrorToString) {
  const Error e = err(ErrorCode::Postgres, "查询失败", "23505", "entity_locations");
  EXPECT_EQ(e.to_string(), "[postgres] 查询失败 (23505) @ entity_locations");

  const Error bare = err(ErrorCode::Internal, "内部错误");
  EXPECT_EQ(bare.to_string(), "[internal] 内部错误");
}

TEST(UnitResult, ErrorCodeNames) {
  const std::pair<ErrorCode, const char*> cases[] = {
      {ErrorCode::InvalidArgument, "invalid_argument"},
      {ErrorCode::Config, "config"},
      {ErrorCode::NotFound, "not_found"},
      {ErrorCode::Busy, "busy"},
      {ErrorCode::Conflict, "conflict"},
      {ErrorCode::Auth, "auth"},
      {ErrorCode::Network, "network"},
      {ErrorCode::Storage, "storage"},
      {ErrorCode::Postgres, "postgres"},
      {ErrorCode::Io, "io"},
      {ErrorCode::Internal, "internal"},
  };
  for (const auto& [code, name] : cases) {
    SCOPED_TRACE(name);
    EXPECT_STREQ(to_string(code), name);
  }
}

} // namespace
} // namespace sb