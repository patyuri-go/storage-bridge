#pragma once

#include "result.hpp"

#include <gtest/gtest.h>

#include <string>

namespace sb {

template <typename T, typename E>
void expect_error(const Result<T, E>& result, ErrorCode code, const std::string& fragment = {}) {
  ASSERT_FALSE(result.ok()) << "期望失败，实际成功";
  EXPECT_EQ(result.error().code, code);
  if (!fragment.empty()) {
    EXPECT_NE(result.error().message.find(fragment), std::string::npos)
        << "message = " << result.error().message;
  }
}

} // namespace sb